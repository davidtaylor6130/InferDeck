#include "llama_cpp_wrapper/llama_cpp_model.hpp"
#include "llama_cpp_wrapper/llama_chat_adapter.hpp"
#include "llama_cpp_wrapper/streaming_tool_call_state.hpp"
#include "llama_cpp_wrapper/continuous_batch_scheduler.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi1_4.h>
#include <excpt.h>
#include <psapi.h>
#include <wrl/client.h>
#endif

#include "llama.h"
#include "ggml.h"
#include "foundation/logging.hpp"
#include "base64.hpp"
#include "chat.h"
#include "mtmd-helper.h"
#include "sampling.h"
#include "speculative.h"
#include <nlohmann/json.hpp>

namespace inferdeck::llama_wrapper {

namespace {

using inferdeck::foundation::Error;
using inferdeck::foundation::ErrorCode;
using inferdeck::foundation::Result;
using inferdeck::foundation::LOG_INFO;
using inferdeck::foundation::LOG_WARN;
using inferdeck::foundation::LOG_ERROR;
using inferdeck::model::InferenceRequest;
using inferdeck::model::InferenceResult;
using inferdeck::model::ChatMessage;
using inferdeck::model::InferenceDelta;
using inferdeck::model::EmbeddingResult;
using inferdeck::model::ToolCall;
using inferdeck::model::ToolCallDelta;

#ifdef _WIN32
static void log_stack_overflow(int iteration, int prompt_tokens, const char* model_name) {
    std::ofstream log("logs/inference.log", std::ios::app);
    if (log.is_open()) {
        log << "[STACK_OVERFLOW] predict_stream iteration=" << iteration
            << " prompt_tokens=" << prompt_tokens
            << " model=" << (model_name ? model_name : "unknown")
            << " timestamp=" << std::time(nullptr) << "\n";
    }
    std::cerr << "[STACK_OVERFLOW] predict_stream iteration=" << iteration
              << " prompt_tokens=" << prompt_tokens
              << " model=" << (model_name ? model_name : "unknown") << std::endl;
}
#endif

inline Error make_error(ErrorCode code, std::string msg) {
  return Error{code, std::move(msg)};
}

int count_output_tokens(const llama_vocab* vocab, const std::string& text) {
  if (!vocab || text.empty()) return 0;
  try {
    return static_cast<int>(common_tokenize(vocab, text, false, true).size());
  } catch (...) {
    return 0;
  }
}

std::string random_string(std::size_t n = 32) {
  static constexpr char chars[] =
      "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
  static thread_local std::mt19937 rng{std::random_device{}()};
  std::uniform_int_distribution<std::size_t> dist(0, sizeof(chars) - 2);
  std::string out;
  out.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    out.push_back(chars[dist(rng)]);
  }
  return out;
}

std::string gen_tool_call_id() {
  return random_string();
}

static bool maybe_truncate_prompt(
    std::vector<llama_token>& prompt_tokens,
    int n_ctx,
    int req_max_tokens,
    const std::string& model_name) {
  const int n_tokens = static_cast<int>(prompt_tokens.size());
  if (n_tokens < n_ctx) return false;
  // Guard the clamp bounds: when n_ctx < 1024 the upper bound (n_ctx/4) falls
  // below 256, so std::clamp(x, 256, hi) would have lo > hi, which is UB.
  const int reserve_hi = n_ctx / 4;
  const int reserve = std::clamp(req_max_tokens > 0 ? req_max_tokens : 1024,
                                 std::min(256, reserve_hi), reserve_hi);
  const int target = n_ctx - reserve - 1;
  const int keep_head = std::min(1024, target / 4);
  const int keep_tail = target - keep_head;
  std::vector<llama_token> kept;
  kept.reserve(static_cast<std::size_t>(target));
  kept.insert(kept.end(), prompt_tokens.begin(), prompt_tokens.begin() + keep_head);
  kept.insert(kept.end(), prompt_tokens.end() - keep_tail, prompt_tokens.end());
  LOG_WARN("llama_prompt_truncated",
           "model={} original_tokens={} kept_tokens={} keep_head={} keep_tail={} n_ctx={}",
           model_name,
           n_tokens,
           kept.size(),
           keep_head,
           keep_tail,
           n_ctx);
  prompt_tokens = std::move(kept);
  return true;
}

static bool process_prompt_chunks(
    llama_context* ctx,
    const std::vector<llama_token>& prompt_tokens,
    int start_token_index,
    int seq_id,
    int n_batch,
    const std::string& model_name) {
  const int n_tokens = static_cast<int>(prompt_tokens.size());
  int n_prompt_processed = std::max(0, std::min(start_token_index, n_tokens));
  while (n_prompt_processed < n_tokens) {
    int n_chunk = std::min(n_batch, n_tokens - n_prompt_processed);
    llama_batch batch = llama_batch_init(n_chunk, 0, 1);
    for (int i = 0; i < n_chunk; ++i) {
      batch.token[i] = prompt_tokens[n_prompt_processed + i];
      batch.pos[i] = n_prompt_processed + i;
      batch.n_seq_id[i] = 1;
      batch.seq_id[i][0] = seq_id;
      batch.logits[i] = (i == n_chunk - 1) ? 1 : 0;
    }
    batch.n_tokens = n_chunk;
    const int rc = llama_decode(ctx, batch);
    if (rc != 0) {
      LOG_ERROR("llama_prompt_decode_failed",
                "model={} rc={} chunk_start={} chunk_tokens={} prompt_tokens={} n_batch={} n_ctx={} n_ctx_seq={}",
                model_name,
                rc,
                n_prompt_processed,
                n_chunk,
                n_tokens,
                n_batch,
                llama_n_ctx(ctx),
                llama_n_ctx_seq(ctx));
      llama_batch_free(batch);
      return false;
    }
    llama_batch_free(batch);
    n_prompt_processed += n_chunk;
  }
  return true;
}

static bool g_backend_initialized = false;

std::string lower_copy(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

ggml_type cache_type_from_string(const std::string& type) {
  const auto t = lower_copy(type);
  if (t == "f32") return GGML_TYPE_F32;
  if (t == "f16") return GGML_TYPE_F16;
  if (t == "bf16") return GGML_TYPE_BF16;
  if (t == "q8_0") return GGML_TYPE_Q8_0;
  if (t == "q4_0") return GGML_TYPE_Q4_0;
  if (t == "q4_1") return GGML_TYPE_Q4_1;
  if (t == "q5_0") return GGML_TYPE_Q5_0;
  if (t == "q5_1") return GGML_TYPE_Q5_1;
  if (t == "iq4_nl") return GGML_TYPE_IQ4_NL;
  return GGML_TYPE_F16;
}

llama_flash_attn_type flash_attn_from_string(const std::string& value) {
  const auto v = lower_copy(value);
  if (v == "on" || v == "enabled" || v == "true" || v == "1") {
    return LLAMA_FLASH_ATTN_TYPE_ENABLED;
  }
  if (v == "off" || v == "disabled" || v == "false" || v == "0") {
    return LLAMA_FLASH_ATTN_TYPE_DISABLED;
  }
  return LLAMA_FLASH_ATTN_TYPE_AUTO;
}

struct ProcessMemorySnapshot {
  std::uint64_t working_set_mb{0};
  std::uint64_t private_mb{0};
  std::uint64_t system_commit_mb{0};
  std::uint64_t gpu_local_mb{0};
  std::uint64_t gpu_nonlocal_mb{0};
  bool gpu_memory_available{false};
};

std::optional<ProcessMemorySnapshot> process_memory_snapshot() {
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS_EX pmc{};
  if (!GetProcessMemoryInfo(GetCurrentProcess(),
                            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                            sizeof(pmc))) {
    return std::nullopt;
  }
  MEMORYSTATUSEX mem{};
  mem.dwLength = sizeof(mem);
  ProcessMemorySnapshot out;
  out.working_set_mb = static_cast<std::uint64_t>(pmc.WorkingSetSize / (1024 * 1024));
  out.private_mb = static_cast<std::uint64_t>(pmc.PrivateUsage / (1024 * 1024));
  if (GlobalMemoryStatusEx(&mem)) {
    out.system_commit_mb =
        static_cast<std::uint64_t>((mem.ullTotalPageFile - mem.ullAvailPageFile) / (1024 * 1024));
  }
  Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
  if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory.GetAddressOf())))) {
    for (UINT i = 0;; ++i) {
      Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
      if (factory->EnumAdapters1(i, adapter.GetAddressOf()) == DXGI_ERROR_NOT_FOUND) {
        break;
      }
      Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;
      if (FAILED(adapter.As(&adapter3))) {
        continue;
      }
      DXGI_QUERY_VIDEO_MEMORY_INFO local{};
      DXGI_QUERY_VIDEO_MEMORY_INFO nonlocal{};
      if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local))) {
        out.gpu_local_mb += static_cast<std::uint64_t>(local.CurrentUsage / (1024 * 1024));
        out.gpu_memory_available = true;
      }
      if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonlocal))) {
        out.gpu_nonlocal_mb += static_cast<std::uint64_t>(nonlocal.CurrentUsage / (1024 * 1024));
        out.gpu_memory_available = true;
      }
    }
  }
  return out;
#else
  return std::nullopt;
#endif
}

void log_memory_snapshot(const char* event, const std::string& model_name) {
  const auto snap = process_memory_snapshot();
  if (!snap) {
    LOG_INFO(event, "model={} process_memory=unavailable", model_name);
    return;
  }
  if (snap->gpu_memory_available) {
    LOG_INFO(event,
             "model={} working_set_mb={} private_mb={} system_commit_mb={} gpu_local_mb={} gpu_nonlocal_mb={}",
             model_name,
             snap->working_set_mb,
             snap->private_mb,
             snap->system_commit_mb,
             snap->gpu_local_mb,
             snap->gpu_nonlocal_mb);
    return;
  }
  LOG_INFO(event,
           "model={} working_set_mb={} private_mb={} system_commit_mb={} gpu_memory=unavailable",
           model_name,
           snap->working_set_mb,
           snap->private_mb,
           snap->system_commit_mb);
}

void log_slot_release(
    const std::string& model_name,
    int n_tokens,
    bool truncated,
    int n_decoded,
    int n_ctx) {
  LOG_INFO("llama_slot_release",
           "model={} n_tokens={} truncated={} n_decoded={} n_ctx={}",
           model_name,
           n_tokens,
           truncated,
           n_decoded,
           n_ctx);
}

void log_token_decode_failed(
    const std::string& model_name,
    int rc,
    int token_index,
    llama_token token_id,
    int pos,
    int n_cur,
    int n_ctx,
    int n_decoded) {
  LOG_ERROR("llama_token_decode_failed",
            "model={} rc={} token_index={} token_id={} pos={} n_cur={} n_ctx={} n_decoded={}",
            model_name,
            rc,
            token_index,
            token_id,
            pos,
            n_cur,
            n_ctx,
            n_decoded);
  log_memory_snapshot("llama_token_decode_failed_memory", model_name);
}

std::string normalize_path(const std::string& p) {
  std::filesystem::path path(p);
  std::error_code ec;
  if (std::filesystem::exists(path, ec)) return std::filesystem::absolute(path, ec).string();
  return p;
}

std::vector<llama_token> tokenize_stop_strings(
    const llama_vocab* vocab, const std::vector<std::string>& stops) {
  std::vector<llama_token> tokens;
  for (const auto& stop_str : stops) {
    if (stop_str.empty()) continue;
    llama_token token{};
    int n = llama_tokenize(vocab, stop_str.c_str(), static_cast<int>(stop_str.size()),
                           &token, 1, false, true);
    if (n == 1) {
      tokens.push_back(token);
    }
  }
  return tokens;
}

std::string token_to_piece(const llama_vocab* vocab, llama_token token) {
  std::array<char, 256> local{};
  int n = llama_token_to_piece(
      vocab, token, local.data(), static_cast<int>(local.size()), 0, true);
  if (n >= 0) {
    return std::string(local.data(), static_cast<std::size_t>(n));
  }

  std::vector<char> buffer(
      static_cast<std::size_t>(-static_cast<int64_t>(n)));
  while (true) {
    n = llama_token_to_piece(
        vocab, token, buffer.data(), static_cast<int>(buffer.size()), 0, true);
    if (n >= 0) {
      return std::string(buffer.data(), static_cast<std::size_t>(n));
    }
    const auto required = static_cast<std::size_t>(-static_cast<int64_t>(n));
    if (required <= buffer.size()) return {};
    buffer.resize(required);
  }
}

ToolCall to_tool_call(const common_chat_tool_call& tc) {
  ToolCall out;
  out.id = tc.id;
  out.type = "function";
  out.function_name = tc.name;
  out.function_arguments = tc.arguments;
  return out;
}

std::string json_arguments(const nlohmann::json& args) {
  return args.is_string() ? args.get<std::string>() : args.dump();
}

std::optional<ToolCall> parse_tool_call_object(const nlohmann::json& obj, std::size_t index) {
  if (!obj.is_object()) return std::nullopt;

  ToolCall call;
  call.id = obj.value("id", "call_" + std::to_string(index));
  call.type = "function";

  if (obj.contains("function") && obj["function"].is_object()) {
    const auto& fn = obj["function"];
    call.function_name = fn.value("name", "");
    if (fn.contains("arguments")) call.function_arguments = json_arguments(fn["arguments"]);
  } else {
    call.function_name = obj.value("name", "");
    if (obj.contains("arguments")) call.function_arguments = json_arguments(obj["arguments"]);
  }

  if (call.function_name.empty()) return std::nullopt;
  if (call.function_arguments.empty()) call.function_arguments = "{}";
  return call;
}

std::optional<std::vector<ToolCall>> parse_fallback_tool_calls(const std::string& text) {
  std::vector<std::string> candidates;
  candidates.push_back(text);

  const auto fence = text.find("```");
  if (fence != std::string::npos) {
    auto body_start = text.find('\n', fence);
    if (body_start != std::string::npos) {
      ++body_start;
      const auto body_end = text.find("```", body_start);
      if (body_end != std::string::npos && body_end > body_start) {
        candidates.push_back(text.substr(body_start, body_end - body_start));
      }
    }
  }

  const auto first_brace = text.find('{');
  const auto last_brace = text.rfind('}');
  if (first_brace != std::string::npos && last_brace != std::string::npos && last_brace > first_brace) {
    candidates.push_back(text.substr(first_brace, last_brace - first_brace + 1));
  }

  for (const auto& candidate : candidates) {
    try {
      auto json = nlohmann::json::parse(candidate);
      std::vector<ToolCall> calls;
      if (json.is_object() && json.contains("tool_calls") && json["tool_calls"].is_array()) {
        std::size_t i = 0;
        for (const auto& item : json["tool_calls"]) {
          if (auto call = parse_tool_call_object(item, i++)) calls.push_back(std::move(*call));
        }
      } else if (json.is_array()) {
        std::size_t i = 0;
        for (const auto& item : json) {
          if (auto call = parse_tool_call_object(item, i++)) calls.push_back(std::move(*call));
        }
      } else if (auto call = parse_tool_call_object(json, 0)) {
        calls.push_back(std::move(*call));
      }
      if (!calls.empty()) return calls;
    } catch (...) {}
  }

  return std::nullopt;
}

InferenceDelta to_delta(const common_chat_msg_diff& diff) {
  InferenceDelta out;
  out.content = diff.content_delta;
  out.reasoning_text = diff.reasoning_content_delta;
  if (diff.tool_call_index != std::string::npos) {
    ToolCallDelta tc;
    tc.index = diff.tool_call_index;
    tc.id = diff.tool_call_delta.id;
    if (!tc.id.empty()) tc.type = "function";
    tc.function_name = diff.tool_call_delta.name;
    tc.function_arguments = diff.tool_call_delta.arguments;
    out.tool_calls.push_back(std::move(tc));
  }
  return out;
}

class StreamingChatParserState {
public:
  explicit StreamingChatParserState(common_chat_parser_params params)
      : parser_params_(std::move(params)) {
    if (!parser_params_.echo) {
      try {
        chat_msg_ = common_chat_parse("", true, parser_params_);
      } catch (...) {
        chat_msg_ = common_chat_msg{};
      }
    }
  }

  std::vector<common_chat_msg_diff> update(
      const std::string& text_added,
      bool is_partial,
      bool filter_tool_calls) {
    std::vector<common_chat_msg_diff> diffs;
    generated_text_ += text_added;
    auto previous = chat_msg_;
    auto next = common_chat_parse(generated_text_, is_partial, parser_params_);
    if (next.empty()) {
      return diffs;
    }

    next.set_tool_call_ids(generated_tool_call_ids_, gen_tool_call_id);
    chat_msg_ = std::move(next);
    auto all_diffs = common_chat_msg_diff::compute_diffs(previous, chat_msg_);
    if (!filter_tool_calls) {
      return all_diffs;
    }

    for (auto& d : all_diffs) {
      for (std::size_t i = 0; i < chat_msg_.tool_calls.size(); ++i) {
        if (sent_tool_call_names_.count(i) || chat_msg_.tool_calls[i].name.empty()) {
          continue;
        }
        if (d.tool_call_index != i || !d.tool_call_delta.arguments.empty()) {
          common_chat_msg_diff header;
          header.tool_call_index = i;
          header.tool_call_delta.id = chat_msg_.tool_calls[i].id;
          header.tool_call_delta.name = chat_msg_.tool_calls[i].name;
          diffs.push_back(std::move(header));
          sent_tool_call_names_.insert(i);
        }
      }

      if (d.tool_call_index == std::string::npos) {
        diffs.push_back(std::move(d));
      } else {
        const std::size_t i = d.tool_call_index;
        if (sent_tool_call_names_.count(i)) {
          if (!d.tool_call_delta.arguments.empty()) {
            d.tool_call_delta.name.clear();
            d.tool_call_delta.id.clear();
            diffs.push_back(std::move(d));
          }
        } else {
          if (!d.tool_call_delta.arguments.empty() || !is_partial) {
            d.tool_call_delta.name = chat_msg_.tool_calls[i].name;
            d.tool_call_delta.id = chat_msg_.tool_calls[i].id;
            diffs.push_back(std::move(d));
            sent_tool_call_names_.insert(i);
          }
        }
      }
    }

    if (!is_partial) {
      for (std::size_t i = 0; i < chat_msg_.tool_calls.size(); ++i) {
        if (!sent_tool_call_names_.count(i) && !chat_msg_.tool_calls[i].name.empty()) {
          common_chat_msg_diff header;
          header.tool_call_index = i;
          header.tool_call_delta.id = chat_msg_.tool_calls[i].id;
          header.tool_call_delta.name = chat_msg_.tool_calls[i].name;
          diffs.push_back(std::move(header));
          sent_tool_call_names_.insert(i);
        }
      }
    }

    return diffs;
  }

private:
  common_chat_parser_params parser_params_;
  common_chat_msg chat_msg_;
  std::string generated_text_;
  std::vector<std::string> generated_tool_call_ids_;
  std::unordered_set<std::size_t> sent_tool_call_names_;
};

void apply_parsed_message(InferenceResult& out, const common_chat_msg& msg) {
  out.text = msg.content;
  out.reasoning_text = msg.reasoning_content;
  out.tool_calls.clear();
  for (const auto& tc : msg.tool_calls) {
    out.tool_calls.push_back(to_tool_call(tc));
  }
}

common_chat_msg parse_final_message_with_ids(
    const std::string& generated,
    const common_chat_parser_params& parser_params) {
  auto msg = common_chat_parse(generated, false, parser_params);
  std::vector<std::string> ids;
  msg.set_tool_call_ids(ids, gen_tool_call_id);
  return msg;
}

void apply_fallback_tool_calls(InferenceResult& out, const std::string& generated) {
  auto calls = parse_fallback_tool_calls(generated);
  if (!calls || calls->empty()) return;
  out.text.clear();
  out.tool_calls = std::move(*calls);
}

bool fallback_tool_call_complete(const std::string& generated) {
  return generated.find("```") != std::string::npos && parse_fallback_tool_calls(generated).has_value();
}

} // namespace

#include "llama_cpp_runtime.ipp"

#include "llama_cpp_embedding.ipp"

#include "llama_cpp_prompt_cache.ipp"
#include "llama_cpp_decoding.ipp"

}
