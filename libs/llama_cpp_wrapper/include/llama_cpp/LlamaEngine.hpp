#pragma once

#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <optional>
#include <cstdint>
#include <atomic>

namespace inferdeck::core {

enum class MessageRole {
    System,
    User,
    Assistant,
    Tool
};

struct ChatMessage {
    MessageRole role;
    std::string content;
    std::string tool_call_id;
    std::string name;
    std::string tool_calls_json;
};

struct InferenceParams {
    int max_tokens = -1;
    float temperature = 0.7f;
    float top_p = 0.9f;
    float repetition_penalty = 1.0f;
    bool stream = false;
    std::string stop;
    std::vector<float> presence_penalty;
    std::string user;
    std::string tools_json;
    std::string tool_format_override;
};

enum class TokenType {
    Content,
    Reasoning
};

struct ToolCall {
    std::string id;
    std::string type;
    std::string function_name;
    std::string function_arguments;
};

struct InferenceResult {
    std::string text;
    std::string reasoning_text;
    int prompt_tokens = 0;
    int completion_tokens = 0;
    int total_tokens = 0;
    float duration_ms = 0.0f;
    std::vector<ToolCall> tool_calls;
    int http_status = 200;
    std::string error_message;

    bool HasError() const { return !error_message.empty() || http_status >= 400; }
};

struct InferenceStats {
    uint64_t total_requests = 0;
    uint64_t successful_requests = 0;
    uint64_t failed_requests = 0;
    float avg_latency_ms = 0.0f;
    float max_latency_ms = 0.0f;
    float min_latency_ms = 999999.0f;
    uint64_t tokens_generated = 0;
    uint64_t tokens_processed = 0;
};

struct GpuInfo {
    uint32_t device_index = 0;
    std::string name;
    uint64_t memory_total = 0;
    uint64_t memory_free = 0;
    uint32_t compute_units = 0;
    bool is_discrete = true;
};

struct HttpStreamResult {
    std::string content_text;
    std::string reasoning_text;
    int http_status = 0;
    std::string error_message;
};

using TokenCallback = std::function<void(const std::string& token, TokenType type, int cumulative_tokens)>;
using StreamHeartbeatCallback = std::function<void()>;

class LlamaEngine {
public:
    bool Initialize(const std::string& model_path,
                    const std::string& precision = "auto",
                    int gpu_layers = -1,
                    int context_size = 100000,
                    const std::string& mmproj_path = "");

    bool SwitchModel(const std::string& model_path);
    bool LoadModel(const std::string& model_path);

    static LlamaEngine& Get();
    bool IsInitialized() const;
    InferenceResult Predict(const std::vector<ChatMessage>& messages,
                            const InferenceParams& params = {});
    InferenceResult PredictStream(const std::vector<ChatMessage>& messages,
                                   const InferenceParams& params = {},
                                   TokenCallback on_token = nullptr,
                                   StreamHeartbeatCallback on_heartbeat = nullptr);
    InferenceStats GetStats() const;
    std::string GetModelName() const;
    std::string GetModelFamily() const;
    std::string GetPrecision() const;
    void AbortActiveRequest(const std::string& reason);
    void Shutdown();
    GpuInfo GetGpuInfo() const;

private:
    LlamaEngine();
    ~LlamaEngine();
    LlamaEngine(const LlamaEngine&) = delete;
    LlamaEngine& operator=(const LlamaEngine&) = delete;

    std::string role_to_string(MessageRole role) const;
    InferenceResult Generate(const std::vector<ChatMessage>& messages,
                             const InferenceParams& params,
                             TokenCallback on_token,
                             StreamHeartbeatCallback on_heartbeat);
    void FreeRuntime();

    std::string model_path_;
    std::string precision_;
    int gpu_layers_ = -1;
    int context_size_ = 100000;
    std::string mmproj_path_;
    std::string current_model_name_;
    void* model_ = nullptr;
    void* context_ = nullptr;
    mutable std::mutex generation_mutex_;
    std::atomic<bool> abort_requested_{false};

    InferenceStats stats_;
    mutable std::mutex mutex_;
    bool initialized_ = false;
};

} // namespace inferdeck::core
