#include "llama_cpp_wrapper/llama_cpp_model.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

#include "llama.h"
#include "foundation/logging.hpp"

namespace inferdeck::llama_wrapper {

namespace {

using inferdeck::foundation::Error;
using inferdeck::foundation::ErrorCode;
using inferdeck::foundation::Result;
using inferdeck::foundation::LOG_INFO;
using inferdeck::foundation::LOG_ERROR;
using inferdeck::model::InferenceRequest;
using inferdeck::model::InferenceResult;

inline Error make_error(ErrorCode code, std::string msg) {
  return Error{code, std::move(msg)};
}

static bool g_backend_initialized = false;

std::string normalize_path(const std::string& p) {
  std::filesystem::path path(p);
  std::error_code ec;
  if (std::filesystem::exists(path, ec)) return std::filesystem::absolute(path, ec).string();
  return p;
}

}

std::string LlamaCppModel::version() {
  const char* info = llama_print_system_info();
  return info ? std::string(info) : std::string("unknown");
}

void LlamaCppModel::init_backend() {
  if (!g_backend_initialized) {
    llama_backend_init();
    g_backend_initialized = true;
    LOG_INFO("llama_backend_init", "Vulkan backend initialized");
  }
}
void LlamaCppModel::shutdown_backend() {
  if (g_backend_initialized) {
    llama_backend_free();
    g_backend_initialized = false;
  }
}

LlamaCppModel::LlamaCppModel(inferdeck::model::ModelInfo info, LlamaCppConfig cfg)
    : info_(std::move(info)), cfg_(std::move(cfg)) {
  resolved_gguf_path_ = normalize_path(info_.gguf_path);
}

LlamaCppModel::~LlamaCppModel() {
  if (loaded_.load()) {
    unload();
  }
}

Result<void> LlamaCppModel::load() {
  std::lock_guard lk(mtx_);
  if (loaded_.load()) return Result<void>{};
  if (resolved_gguf_path_.empty()) {
    return Result<void>(std::unexpect,
        make_error(ErrorCode::NotFound, "empty gguf_path"));
  }
  std::error_code ec;
  if (!std::filesystem::exists(resolved_gguf_path_, ec)) {
    return Result<void>(std::unexpect,
        make_error(ErrorCode::NotFound, "gguf not found: " + resolved_gguf_path_.string()));
  }

  llama_model_params mparams = llama_model_default_params();
  mparams.use_mmap = cfg_.use_mmap;
  mparams.use_mlock = cfg_.use_mlock;
  mparams.n_gpu_layers = -1;

  llama_backend_init();
  const char* sys_info = llama_print_system_info();
  if (sys_info) {
    LOG_INFO("llama_system_info", "{}", sys_info);
  }

  model_ = llama_model_load_from_file(resolved_gguf_path_.string().c_str(), mparams);
  if (model_ == nullptr) {
    const char* err = llama_print_system_info();
    LOG_ERROR("model_load_failed", "llama_model_load_from_file returned null for {}", resolved_gguf_path_.string());
    if (err) LOG_ERROR("model_load_failed", "system_info: {}", err);
    return Result<void>(std::unexpect,
        make_error(ErrorCode::Internal,
                   "llama_model_load_from_file returned null for " + resolved_gguf_path_.string()));
  }
  vocab_ = llama_model_get_vocab(model_);
  if (vocab_ == nullptr) {
    llama_model_free(model_);
    model_ = nullptr;
    return Result<void>(std::unexpect,
        make_error(ErrorCode::ParseError, "llama_model_get_vocab returned null"));
  }
  auto ctx_res = init_contexts_locked();
  if (!ctx_res.has_value()) {
    llama_model_free(model_);
    model_ = nullptr;
    return ctx_res;
  }
  loaded_.store(true);
  return Result<void>{};
}

Result<void> LlamaCppModel::init_contexts_locked() {
  slots_.clear();
  slots_.resize(info_.n_slots);
  for (int i = 0; i < info_.n_slots; ++i) {
    llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx = static_cast<std::uint32_t>(std::max(512, info_.context_size));
    cparams.n_threads = cfg_.n_threads;
    cparams.n_batch = static_cast<std::uint32_t>(std::max(1, cfg_.n_batch));
    cparams.n_ubatch = static_cast<std::uint32_t>(std::max(1, cfg_.n_ubatch));
    auto* ctx = llama_init_from_model(model_, cparams);
    if (ctx == nullptr) {
      return Result<void>(std::unexpect,
          make_error(ErrorCode::OutOfMemory,
                     "llama_init_from_model returned null for slot " + std::to_string(i)));
    }
    slots_[i].ctx = std::unique_ptr<llama_context, void(*)(llama_context*)>(
        ctx, [](llama_context* c) { if (c) llama_free(c); });
    slots_[i].busy = false;
  }
  return Result<void>{};
}

Result<void> LlamaCppModel::unload() {
  std::lock_guard lk(mtx_);
  if (!loaded_.load()) return Result<void>{};
  for (auto& s : slots_) {
    s.ctx.reset();
    s.busy = false;
  }
  slots_.clear();
  if (model_) {
    llama_model_free(model_);
    model_ = nullptr;
  }
  vocab_ = nullptr;
  loaded_.store(false);
  return Result<void>{};
}

int LlamaCppModel::vram_usage_mb() const noexcept {
  return info_.vram_required_mb;
}

int LlamaCppModel::n_free_slots() const noexcept {
  std::lock_guard lk(mtx_);
  int free = 0;
  for (const auto& s : slots_) if (!s.busy) ++free;
  return free;
}

Result<int> LlamaCppModel::acquire_slot() {
  std::lock_guard lk(mtx_);
  if (!loaded_.load()) {
    return Result<int>(std::unexpect,
        make_error(ErrorCode::Internal, "model not loaded"));
  }
  for (int i = 0; i < static_cast<int>(slots_.size()); ++i) {
    if (!slots_[i].busy) {
      slots_[i].busy = true;
      return Result<int>(i);
    }
  }
  return Result<int>(std::unexpect, make_error(ErrorCode::Unavailable, "no free slots"));
}

Result<void> LlamaCppModel::release_slot(int slot_id) {
  std::lock_guard lk(mtx_);
  if (slot_id < 0 || slot_id >= static_cast<int>(slots_.size())) {
    return Result<void>(std::unexpect,
        make_error(ErrorCode::InvalidArgument,
                   "slot_id out of range: " + std::to_string(slot_id)));
  }
  slots_[slot_id].busy = false;
  return Result<void>{};
}

bool LlamaCppModel::slot_busy(int slot_id) const noexcept {
  std::lock_guard lk(mtx_);
  if (slot_id < 0 || slot_id >= static_cast<int>(slots_.size())) return false;
  return slots_[slot_id].busy;
}

Result<void> LlamaCppModel::build_sampler_locked(
    llama_sampler** out, const InferenceRequest& req) {
  const std::uint32_t seed = req.seed >= 0 ? static_cast<std::uint32_t>(req.seed)
                                           : static_cast<std::uint32_t>(0xFFFFFFFFu);
  llama_sampler_chain_params sparams = llama_sampler_chain_default_params();
  llama_sampler* chain = llama_sampler_chain_init(sparams);
  if (chain == nullptr) {
    return Result<void>(std::unexpect, make_error(ErrorCode::Internal, "llama_sampler_chain_init returned null"));
  }
  auto append = [&](llama_sampler* s) {
    if (s) llama_sampler_chain_add(chain, s);
  };
  append(llama_sampler_init_top_k(static_cast<int32_t>(req.top_k)));
  append(llama_sampler_init_top_p(req.top_p, 1));
  append(llama_sampler_init_min_p(0.05f, 1));
  append(llama_sampler_init_temp(req.temperature));
  append(llama_sampler_init_dist(seed));
  *out = chain;
  return Result<void>{};
}

Result<InferenceResult> LlamaCppModel::predict(int slot_id, const InferenceRequest& req) {
  if (slot_id < 0 || slot_id >= static_cast<int>(slots_.size())) {
    return Result<InferenceResult>(std::unexpect,
        make_error(ErrorCode::InvalidArgument, "slot_id out of range"));
  }
  std::lock_guard lk(mtx_);
  if (!loaded_.load()) {
    return Result<InferenceResult>(std::unexpect,
        make_error(ErrorCode::Internal, "model not loaded"));
  }
  auto& slot = slots_[slot_id];
  if (!slot.busy || !slot.ctx) {
    return Result<InferenceResult>(std::unexpect,
        make_error(ErrorCode::InvalidArgument, "slot not acquired or no context"));
  }

  std::vector<llama_token> prompt_tokens(req.prompt.size() + 16);
  int n_tokens = llama_tokenize(vocab_,
                                req.prompt.data(),
                                static_cast<int>(req.prompt.size()),
                                prompt_tokens.data(),
                                static_cast<int>(prompt_tokens.size()),
                                true, true);
  if (n_tokens < 0) {
    prompt_tokens.resize(static_cast<std::size_t>(-n_tokens));
    n_tokens = llama_tokenize(vocab_,
                              req.prompt.data(),
                              static_cast<int>(req.prompt.size()),
                              prompt_tokens.data(),
                              static_cast<int>(prompt_tokens.size()),
                              true, true);
    if (n_tokens < 0) {
      return Result<InferenceResult>(std::unexpect,
          make_error(ErrorCode::Internal, "tokenization failed"));
    }
  }
  prompt_tokens.resize(static_cast<std::size_t>(n_tokens));

  llama_batch batch = llama_batch_init(n_tokens, 0, 1);
  for (int i = 0; i < n_tokens; ++i) {
    batch.token[i] = prompt_tokens[i];
    batch.pos[i] = i;
    batch.n_seq_id[i] = 1;
    batch.seq_id[i][0] = 0;
    batch.logits[i] = (i == n_tokens - 1) ? 1 : 0;
  }
  batch.n_tokens = n_tokens;
  if (llama_decode(slot.ctx.get(), batch) != 0) {
    llama_batch_free(batch);
    return Result<InferenceResult>(std::unexpect,
        make_error(ErrorCode::Internal, "llama_decode (prompt) failed"));
  }

  llama_sampler* smp = nullptr;
  auto sres = build_sampler_locked(&smp, req);
  if (!sres.has_value()) {
    llama_batch_free(batch);
    return Result<InferenceResult>(std::unexpect, sres.error());
  }

  InferenceResult out;
  out.prompt_tokens = n_tokens;
  const int max_tokens = std::max(1, req.max_tokens);
  const auto start = std::chrono::steady_clock::now();
  std::string generated;
  generated.reserve(static_cast<std::size_t>(max_tokens) * 4);
  int n_cur = n_tokens;
  for (int i = 0; i < max_tokens; ++i) {
    const llama_token id = llama_sampler_sample(smp, slot.ctx.get(), -1);
    if (llama_vocab_is_eog(vocab_, id)) break;
    char buf[256];
    const int n = llama_token_to_piece(vocab_, id, buf, sizeof(buf), 0, true);
    if (n > 0) generated.append(buf, static_cast<std::size_t>(n));
    out.completion_tokens += 1;

    llama_batch one = llama_batch_init(1, 0, 1);
    one.token[0] = id;
    one.pos[0] = n_cur++;
    one.n_seq_id[0] = 1;
    one.seq_id[0][0] = 0;
    one.logits[0] = 1;
    one.n_tokens = 1;
    if (llama_decode(slot.ctx.get(), one) != 0) {
      llama_batch_free(one);
      llama_batch_free(batch);
      llama_sampler_free(smp);
      return Result<InferenceResult>(std::unexpect,
          make_error(ErrorCode::Internal, "llama_decode (token) failed"));
    }
    llama_batch_free(one);
  }
  const auto end = std::chrono::steady_clock::now();
  out.duration_ms = std::chrono::duration<float, std::milli>(end - start).count();
  if (out.completion_tokens > 0 && out.duration_ms > 0.0f) {
    out.tokens_per_second = (out.completion_tokens * 1000.0f) / out.duration_ms;
  }
  out.text = std::move(generated);
  slot.last_prompt_tokens.assign(prompt_tokens.begin(), prompt_tokens.end());

  llama_sampler_free(smp);
  llama_batch_free(batch);
  return Result<InferenceResult>(std::move(out));
}

}
