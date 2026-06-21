#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "chat.h"
#include "common.h"
#include "model/imodel.hpp"
#include "llama_cpp_wrapper/continuous_batch_scheduler.hpp"

struct llama_model;
struct llama_context;
struct llama_vocab;
struct llama_sampler;
struct common_chat_templates;
using llama_token = int32_t;

namespace inferdeck::llama_wrapper {

using ChatTemplateMeta = inferdeck::model::ChatTemplateMeta;

struct ChatTemplateResult {
  std::string prompt;
  std::vector<std::string> stop_strings;
  common_chat_parser_params parser_params;
  common_params_sampling sampling_params;
  ChatTemplateMeta meta;
};

struct LlamaCppConfig {
  int n_threads{8};
  int n_batch{512};
  int n_ubatch{512};
  bool use_mmap{false};
  bool use_mlock{false};
  std::optional<int> n_gpu_layers{};
  std::string flash_attn{"auto"};
  bool kv_offload{true};
  bool op_offload{true};
  std::string cache_type_k{"q8_0"};
  std::string cache_type_v{"q8_0"};
  bool swa_full{false};
  bool truncate_prompt{true};
  std::string chat_template{};
  std::string reasoning_format{};  // "auto", "deepseek", "deepseek_legacy", "none"
};

class LlamaCppModel final : public inferdeck::model::IModel {
public:
  LlamaCppModel(inferdeck::model::ModelInfo info, LlamaCppConfig cfg = {});
  ~LlamaCppModel() override;

  LlamaCppModel(const LlamaCppModel&) = delete;
  LlamaCppModel& operator=(const LlamaCppModel&) = delete;

  const inferdeck::model::ModelInfo& info() const noexcept override { return info_; }

  const inferdeck::model::ChatTemplateMeta& chat_template_meta() const noexcept override { return chat_template_meta_; }

  inferdeck::foundation::Result<void> load() override;
  inferdeck::foundation::Result<void> unload() override;
  bool is_loaded() const noexcept override { return loaded_.load(); }

  int vram_usage_mb() const noexcept override;
  int n_slots() const noexcept override { return info_.n_slots; }
  int n_free_slots() const noexcept override;

  inferdeck::foundation::Result<int> acquire_slot() override;
  inferdeck::foundation::Result<void> release_slot(int slot_id) override;
  bool slot_busy(int slot_id) const noexcept override;
  inferdeck::foundation::Result<void> reset_all_slots() noexcept;

  inferdeck::foundation::Result<inferdeck::model::InferenceResult> predict(
      int slot_id, const inferdeck::model::InferenceRequest& req) override;
  inferdeck::foundation::Result<inferdeck::model::InferenceResult> predict_stream(
      int slot_id, const inferdeck::model::InferenceRequest& req,
      const inferdeck::model::IModel::TokenCallback& callback,
      const std::atomic<bool>* cancel = nullptr) override;

  static std::string version();
  static void init_backend();
  static void shutdown_backend();

private:
  // Per-slot bookkeeping (no llama_context here — all slots share shared_ctx_)
  struct SlotState {
    bool busy{false};
    std::vector<int> last_prompt_tokens;
    std::vector<uint8_t> recurrent_checkpoint;
    int checkpoint_pos{0};
  };

  inferdeck::foundation::Result<void> init_shared_context_locked();
  inferdeck::foundation::Result<void> build_sampler_locked(
      llama_sampler** out, const inferdeck::model::InferenceRequest& req);
  inferdeck::foundation::Result<ChatTemplateResult> apply_chat_template(
      const inferdeck::model::InferenceRequest& req);

  // Per-inference setup: tokenize, KV-state snapshot, sampler construction.
  struct PredictSetup {
    std::vector<llama_token> prompt_tokens;
    std::vector<std::string> stop_strings;
    std::vector<llama_token> stop_tokens;
    common_chat_parser_params parser_params;
    common_params_sampling sampling_params;
    common_sampler* smp{nullptr};
    int max_tokens{0};
    int n_ctx_seq{0};
    std::vector<int> last_prompt_tokens;
    std::vector<uint8_t> recurrent_checkpoint;
    int checkpoint_pos{0};
  };
  inferdeck::foundation::Result<PredictSetup> prepare_inference(
      int slot_id, const inferdeck::model::InferenceRequest& req);

  // Drain task.out_queue until done, calling on_token for each produced token.
  // on_token returns false to request early stop (sets task.caller_cancel).
  using OnToken = std::function<bool(llama_token)>;
  inferdeck::foundation::Result<void> drain_task(SlotTask& task, const OnToken& on_token);

  inferdeck::model::ModelInfo info_;
  LlamaCppConfig cfg_;
  std::atomic<bool> loaded_{false};
  mutable std::mutex mtx_;        // guards slot state (acquire/release/status/last_prompt_tokens)
  llama_model* model_{nullptr};
  const llama_vocab* vocab_{nullptr};
  // Shared context: n_ctx = context_size * n_slots, n_seq_max = n_slots
  llama_context* shared_ctx_{nullptr};
  std::unique_ptr<ContinuousBatchScheduler> scheduler_;
  std::vector<SlotState> slots_;
  std::filesystem::path resolved_gguf_path_;
  common_chat_templates* chat_templates_{nullptr};
  ChatTemplateMeta chat_template_meta_;
};

class LlamaCppModelError : public std::runtime_error {
public:
  enum class Kind { FileNotFound, BadGguf, InitFailed, OutOfMemory, SlotFull, InvalidSlot, DecodeFailed };
  LlamaCppModelError(Kind k, std::string msg)
      : std::runtime_error(std::move(msg)), kind(k) {}
  Kind kind;
};

}
