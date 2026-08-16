#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "foundation/result.hpp"
#include "model/ibackend.hpp"

namespace inferdeck::model {

inline constexpr int k_max_tokens_use_context_budget = -1;

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
    int max_tokens{k_max_tokens_use_context_budget};
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
    std::vector<std::string> thinking_end_tags;
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
    float generation_duration_ms{0.0f};
    float prompt_duration_ms{0.0f};
    float tokens_per_second{0.0f};
    int mtp_drafted_tokens{0};
    int mtp_accepted_tokens{0};
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

struct ImageGenerationRequest {
    std::string prompt;
    std::string negative_prompt;
    int width{1024};
    int height{1024};
    int count{1};
    int steps{20};
    std::int64_t seed{-1};
    float guidance_scale{7.0f};
};

struct ImageGenerationResult {
    std::vector<std::vector<std::byte>> png_images;
    float duration_ms{0.0f};
};

struct SpeechRequest {
    std::string input;
    std::string voice;
    std::string format{"wav"};
    float speed{1.0f};
};

struct AudioResult {
    std::vector<std::byte> bytes;
    std::string content_type;
    float duration_ms{0.0f};
};

struct TranscriptionRequest {
    std::vector<float> pcm;
    int sample_rate{16000};
    std::string language;
    std::string prompt;
    float temperature{0.0f};
};

struct TranscriptionSegment {
    int id{0};
    float start_seconds{0.0f};
    float end_seconds{0.0f};
    std::string text;
    std::vector<int> tokens;
    float avg_logprob{0.0f};
    float no_speech_probability{0.0f};
};

struct TranscriptionResult {
    std::string text;
    std::string language;
    float duration_seconds{0.0f};
    float inference_ms{0.0f};
    std::vector<TranscriptionSegment> segments;
};

class IImageBackend {
public:
    virtual ~IImageBackend() = default;
    virtual foundation::Result<ImageGenerationResult> generate_images(
        int slot_id, const ImageGenerationRequest& request,
        const std::function<bool(int)>& progress = {}) = 0;
};

class ISpeechBackend {
public:
    virtual ~ISpeechBackend() = default;
    virtual foundation::Result<AudioResult> synthesize(
        int slot_id, const SpeechRequest& request,
        const std::function<bool(const std::byte*, std::size_t)>& stream = {}) = 0;
};

class ITranscriptionBackend {
public:
    virtual ~ITranscriptionBackend() = default;
    virtual foundation::Result<TranscriptionResult> transcribe(
        int slot_id, const TranscriptionRequest& request,
        const std::function<bool(int)>& progress = {}) = 0;
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
        (void)slot_id;
        (void)req;
        (void)callback;
        (void)cancel;
        return foundation::Err<InferenceResult>(foundation::ErrorCode::Internal,
            "streaming not implemented");
    }
};

} // namespace inferdeck::model
