#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "model/imodel.hpp"

struct llama_model;
struct llama_context;
struct llama_vocab;
struct llama_sampler;

namespace inferdeck::llama_wrapper {

struct LlamaCppConfig {
  int n_threads{8};
  int n_batch{512};
  int n_ubatch{512};
  bool use_mmap{true};
  bool use_mlock{false};
  std::string chat_template{};
};

class LlamaCppModel final : public inferdeck::model::IModel {
public:
  LlamaCppModel(inferdeck::model::ModelInfo info, LlamaCppConfig cfg = {});
  ~LlamaCppModel() override;

  LlamaCppModel(const LlamaCppModel&) = delete;
  LlamaCppModel& operator=(const LlamaCppModel&) = delete;

  const inferdeck::model::ModelInfo& info() const noexcept override { return info_; }

  inferdeck::foundation::Result<void> load() override;
  inferdeck::foundation::Result<void> unload() override;
  bool is_loaded() const noexcept override { return loaded_.load(); }

  int vram_usage_mb() const noexcept override;
  int n_slots() const noexcept override { return info_.n_slots; }
  int n_free_slots() const noexcept override;

  inferdeck::foundation::Result<int> acquire_slot() override;
  inferdeck::foundation::Result<void> release_slot(int slot_id) override;
  bool slot_busy(int slot_id) const noexcept override;

  inferdeck::foundation::Result<inferdeck::model::InferenceResult> predict(
      int slot_id, const inferdeck::model::InferenceRequest& req) override;

  static std::string version();
  static void init_backend();
  static void shutdown_backend();

private:
  struct SlotState {
    bool busy{false};
    std::unique_ptr<llama_context, void(*)(llama_context*)> ctx{nullptr, nullptr};
    std::vector<int> last_prompt_tokens;
  };

  inferdeck::foundation::Result<void> init_contexts_locked();
  inferdeck::foundation::Result<void> build_sampler_locked(
      llama_sampler** out, const inferdeck::model::InferenceRequest& req);

  inferdeck::model::ModelInfo info_;
  LlamaCppConfig cfg_;
  std::atomic<bool> loaded_{false};
  mutable std::mutex mtx_;
  llama_model* model_{nullptr};
  const llama_vocab* vocab_{nullptr};
  std::vector<SlotState> slots_;
  std::filesystem::path resolved_gguf_path_;
};

class LlamaCppModelError : public std::runtime_error {
public:
  enum class Kind { FileNotFound, BadGguf, InitFailed, OutOfMemory, SlotFull, InvalidSlot, DecodeFailed };
  LlamaCppModelError(Kind k, std::string msg)
      : std::runtime_error(std::move(msg)), kind(k) {}
  Kind kind;
};

}
