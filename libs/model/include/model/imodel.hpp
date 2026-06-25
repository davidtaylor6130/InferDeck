#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "foundation/result.hpp"

namespace inferdeck::model {

// Server-side sampler defaults (issue #42). Values here mirror stock
// llama-server defaults: DRY off, repeat_penalty neutral. Explicit per-request
// (OpenAI body) values override these; see make_inference_request /
// apply_chat_template. Settable globally under `gateway.sampling` and
// overridable per entry in `model_registry`.
struct SamplingConfig {
    float temperature{0.8f};
    float top_p{0.95f};
    int   top_k{40};
    float min_p{0.05f};
    float repeat_penalty{1.0f};   // 1.0 = disabled
    int   repeat_last_n{64};
    float dry_multiplier{0.0f};   // 0.0 = disabled
    float dry_base{1.75f};
    int   dry_allowed_length{2};
    int   dry_penalty_last_n{-1};  // -1 = context size
    std::vector<std::string> dry_seq_breakers{"\n", ":", "\"", "*"};
};

struct ModelInfo {
    std::string name{};
    std::string family{};
    std::string gguf_path{};
    std::string mmproj_path{};
    int n_slots{2};
    int vram_required_mb{0};
    int context_size{65536};
    std::optional<int> n_gpu_layers{};
    bool has_vision{false};
    std::string reasoning_format{};  // "auto", "deepseek", "deepseek_legacy", "none"
    SamplingConfig sampling{};       // per-model defaults (merged over global at parse time)
};

struct ChatMessage {
    std::string role{};
    std::string content{};
    std::string tool_call_id{};
    std::string name{};
};

struct ToolCall {
    std::string id{};
    std::string type{"function"};
    std::string function_name{};
    std::string function_arguments{};
};

struct ToolCallDelta {
    std::size_t index{0};
    std::string id{};
    std::string type{};
    std::string function_name{};
    std::string function_arguments{};
};

struct InferenceDelta {
    std::string content{};
    std::string reasoning_text{};
    std::vector<ToolCallDelta> tool_calls{};
};

struct InferenceRequest {
    std::string prompt{};
    std::vector<ChatMessage> messages{};
    std::string tools_json{};
    std::string openai_body_json{};
    int max_tokens{512};
    // Sampler params are optional so the server can tell an explicit client
    // value apart from "unset" (issue #42). When unset, the server-side
    // SamplingConfig default applies; when set, the client value wins.
    std::optional<float> temperature{};
    std::optional<float> top_p{};
    std::optional<int> top_k{};
    std::optional<float> repeat_penalty{};
    std::optional<int> repeat_last_n{};
    int seed{-1};
    std::optional<std::string> tool_format{};
    std::optional<std::string> grammar{};
};

struct ChatTemplateMeta {
    std::string thinking_start_tag;
    std::string thinking_end_tag;
    std::vector<std::string> preserved_tokens;
    bool supports_thinking = false;
};

struct InferenceResult {
    std::string text{};
    std::string reasoning_text{};
    std::string finish_reason{"stop"};
    int prompt_tokens{0};
    int cached_prompt_tokens{0};
    int completion_tokens{0};
    float duration_ms{0.0f};
    float tokens_per_second{0.0f};
    std::vector<std::string> tool_calls_json{};
    std::vector<ToolCall> tool_calls{};
};

class IModel {
public:
    virtual ~IModel() = default;

    virtual const ModelInfo& info() const = 0;

    virtual const ChatTemplateMeta& chat_template_meta() const {
        static const ChatTemplateMeta meta{};
        return meta;
    }

    virtual foundation::Result<void> load() = 0;
    virtual foundation::Result<void> unload() = 0;
    virtual bool is_loaded() const = 0;

    virtual int vram_usage_mb() const = 0;
    virtual int n_slots() const = 0;
    virtual int n_free_slots() const = 0;

    virtual foundation::Result<int> acquire_slot() = 0;
    virtual foundation::Result<void> release_slot(int slot_id) = 0;
    virtual bool slot_busy(int slot_id) const = 0;
    virtual foundation::Result<void> reset_all_slots() {
        return foundation::Ok();
    }

    using TokenCallback = std::function<bool(const InferenceDelta& delta)>;

    virtual foundation::Result<InferenceResult> predict(
        int slot_id, const InferenceRequest& req) = 0;

    // `cancel`, when non-null and set to true, requests that an in-flight
    // generation stop as soon as possible (checked between tokens / after
    // prefill). Defaulted so non-streaming implementations need not override.
    virtual foundation::Result<InferenceResult> predict_stream(
        int slot_id, const InferenceRequest& req, const TokenCallback& callback,
        const std::atomic<bool>* cancel = nullptr) {
        (void)cancel;
        return foundation::Err<InferenceResult>(foundation::ErrorCode::Internal,
            "streaming not implemented");
    }
};

} // namespace inferdeck::model
