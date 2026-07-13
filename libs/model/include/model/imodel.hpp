#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "foundation/result.hpp"
#include "model/ibackend.hpp"

namespace inferdeck::model {

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

struct EmbeddingRequest {
    std::vector<std::string> inputs;
    std::optional<int> dimensions{};
};

struct EmbeddingResult {
    std::vector<std::vector<float>> embeddings;
    int prompt_tokens{0};
    float duration_ms{0.0f};
};

class IEmbeddingBackend {
public:
    virtual ~IEmbeddingBackend() = default;
    virtual foundation::Result<EmbeddingResult> embed(
        int slot_id, const EmbeddingRequest& request,
        const std::function<bool()>& cancelled = {}) = 0;
};

class IModel : public IBackend {
public:
    virtual ~IModel() = default;

    virtual const ChatTemplateMeta& chat_template_meta() const {
        static const ChatTemplateMeta meta{};
        return meta;
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
