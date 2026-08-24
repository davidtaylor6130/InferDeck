#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace inferdeck::inference {

inline constexpr int kUseContextBudget = -1;

enum class Principal : std::uint8_t {
    Public,
    DataPlane,
    ControlRead,
    ControlWrite,
    Internal,
};

enum class Endpoint : std::uint8_t {
    Chat,
    Responses,
    Embeddings,
    Images,
    Speech,
    Transcriptions,
    Internal,
};

enum class CompatibilityProfile : std::uint8_t {
    StrictOpenAI,
    OpenAIDerivative,
    Internal,
};

struct RequestContext {
    std::string request_id;
    Principal principal{Principal::Public};
    Endpoint endpoint{Endpoint::Internal};
    std::chrono::steady_clock::time_point deadline{};
    std::function<bool()> cancelled;
    std::string requested_model;
    std::string resolved_model;
    bool stream{false};
    CompatibilityProfile compatibility_profile{
        CompatibilityProfile::StrictOpenAI};
};

enum class MessageRole : std::uint8_t {
    Developer,
    System,
    User,
    Assistant,
    Tool,
    Function,
};

struct TextContent {
    std::string text;
};

struct ImageContent {
    std::vector<std::byte> bytes;
    std::string media_type;
    std::string detail;
};

struct AudioContent {
    std::vector<std::byte> bytes;
    std::string media_type;
};

using Content = std::variant<TextContent, ImageContent, AudioContent>;

struct FunctionCall {
    std::string id;
    std::string name;
    std::string arguments;
};

struct Message {
    MessageRole role{MessageRole::User};
    std::vector<Content> content;
    std::vector<FunctionCall> tool_calls;
    std::string reasoning;
    std::string tool_call_id;
    std::string name;

    Message() = default;
    Message(MessageRole message_role, std::string text)
        : role(message_role) {
        content.emplace_back(TextContent{std::move(text)});
    }
    Message(std::string_view message_role, std::string text)
        : Message(parse_role(message_role), std::move(text)) {}

    static MessageRole parse_role(std::string_view value) {
        if (value == "developer") return MessageRole::Developer;
        if (value == "system") return MessageRole::System;
        if (value == "assistant") return MessageRole::Assistant;
        if (value == "tool") return MessageRole::Tool;
        if (value == "function") return MessageRole::Function;
        return MessageRole::User;
    }
};

struct FunctionTool {
    std::string name;
    std::string description;
    std::string parameters_schema{"{}"};
};

enum class ToolChoiceKind : std::uint8_t {
    Auto,
    None,
    Required,
    Function,
};

struct ToolChoice {
    ToolChoiceKind kind{ToolChoiceKind::Auto};
    std::string function_name;
};

enum class StructuredOutputKind : std::uint8_t {
    Text,
    JsonObject,
    JsonSchema,
    Grammar,
};

struct StructuredOutput {
    StructuredOutputKind kind{StructuredOutputKind::Text};
    std::string name;
    std::string description;
    std::string schema;
    bool strict{false};
};

enum class ReasoningFormat : std::uint8_t {
    Automatic,
    None,
    DeepSeek,
    DeepSeekLegacy,
};

struct Sampling {
    std::optional<float> temperature;
    std::optional<float> top_p;
    std::optional<int> top_k;
    std::optional<float> min_p;
    std::optional<float> repeat_penalty;
    std::optional<int> repeat_last_n;
    std::optional<float> frequency_penalty;
    std::optional<float> presence_penalty;
    std::optional<float> tfs_z;
    std::optional<int> mirostat;
    std::optional<float> mirostat_eta;
    std::optional<float> mirostat_tau;
    std::vector<std::pair<std::int32_t, float>> logit_bias;
    std::int64_t seed{-1};
};

struct GenerationRequest {
    std::string prompt;
    std::vector<Message> messages;
    std::vector<FunctionTool> tools;
    ToolChoice tool_choice;
    StructuredOutput output;
    Sampling sampling;
    std::vector<std::string> stop;
    std::optional<std::string> reasoning_effort;
    std::optional<bool> enable_reasoning;
    int max_output_tokens{kUseContextBudget};
    std::optional<int> context_window;
    std::optional<int> prompt_keep_tokens;
    bool logprobs{false};
    int top_logprobs{0};
    bool parallel_tool_calls{true};
    bool add_generation_prompt{true};
    ReasoningFormat reasoning_format{ReasoningFormat::Automatic};
};

struct SessionRequest {
    RequestContext context;
    GenerationRequest generation;
};

struct ToolCall {
    std::string id;
    std::string type{"function"};
    std::string function_name;
    std::string function_arguments;
};

struct ToolCallDelta {
    std::size_t index{0};
    std::string id;
    std::string type;
    std::string function_name;
    std::string function_arguments;
};

struct TopTokenLogprob {
    std::string token;
    float logprob{0.0f};
    std::vector<std::uint8_t> bytes;
};

struct TokenLogprob {
    std::string token;
    float logprob{0.0f};
    std::vector<std::uint8_t> bytes;
    std::vector<TopTokenLogprob> top_logprobs;
};

struct GenerationDelta {
    std::string content;
    std::string reasoning_text;
    std::vector<ToolCallDelta> tool_calls;
    std::vector<TokenLogprob> logprobs;
};

struct TextOutput {
    std::string text;
};

struct ReasoningOutput {
    std::string text;
};

struct ToolCallOutput {
    ToolCall call;
};

struct ToolCallDeltaOutput {
    ToolCallDelta call;
};

struct UsageOutput {
    int input_tokens{0};
    int cached_input_tokens{0};
    int output_tokens{0};
};

struct FinishOutput {
    std::string reason{"stop"};
};

struct RefusalOutput {
    std::string text;
};

enum class DomainErrorCode : std::uint8_t {
    InvalidRequest,
    UnsupportedFeature,
    ModelNotFound,
    CapabilityUnavailable,
    AdmissionRejected,
    ContextLengthExceeded,
    Cancelled,
    DeadlineExceeded,
    RuntimeFailure,
};

struct DomainError {
    DomainErrorCode code{DomainErrorCode::RuntimeFailure};
    std::string message;
    std::string parameter;
    bool retryable{false};
};

struct ErrorOutput {
    DomainError error;
};

using OutputEvent = std::variant<TextOutput, ReasoningOutput,
                                 ToolCallDeltaOutput, ToolCallOutput,
                                 UsageOutput, FinishOutput, RefusalOutput,
                                 ErrorOutput>;

struct GenerationResult {
    std::string text;
    std::string reasoning_text;
    std::string finish_reason{"stop"};
    int prompt_tokens{0};
    int cached_prompt_tokens{0};
    int completion_tokens{0};
    int reasoning_tokens{0};
    float duration_ms{0.0f};
    float generation_duration_ms{0.0f};
    float prompt_duration_ms{0.0f};
    float first_token_duration_ms{0.0f};
    float tokens_per_second{0.0f};
    int mtp_drafted_tokens{0};
    int mtp_accepted_tokens{0};
    std::vector<ToolCall> tool_calls;
    std::vector<TokenLogprob> logprobs;
};

using RequestOutcome = std::variant<GenerationResult, DomainError>;

enum class Capability : std::uint8_t {
    TextInput,
    ImageInput,
    AudioInput,
    TextOutput,
    ReasoningOutput,
    FunctionTools,
    StructuredOutput,
    Embeddings,
    ImageGeneration,
    Speech,
    Transcription,
};

struct CapabilityDeclaration {
    Capability capability{Capability::TextInput};
    bool streaming{false};
    std::size_t maximum_items{0};
};

}
