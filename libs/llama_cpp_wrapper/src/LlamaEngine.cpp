#include "llama_cpp/LlamaEngine.hpp"
#include "llama_cpp/LlamaServerManager.hpp"
#include "llama_cpp/GGUFParser.hpp"
#include "core/Logger.hpp"
#include "core/Metrics.hpp"

#include <chrono>
#include <algorithm>
#include <cmath>
#include <random>
#include <thread>
#include <vector>
#include <sstream>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#endif

using json = nlohmann::json;

namespace inferdeck::core {

LlamaEngine& LlamaEngine::Get() {
    static LlamaEngine instance;
    return instance;
}

LlamaEngine::LlamaEngine() = default;
LlamaEngine::~LlamaEngine() {
    Shutdown();
}

bool LlamaEngine::Initialize(const std::string& model_path,
                              const std::string& precision,
                              int gpu_layers,
                              int context_size) {
    std::lock_guard<std::mutex> lock(mutex_);

    model_path_ = model_path;
    precision_ = precision;
    gpu_layers_ = gpu_layers;
    context_size_ = context_size;

    std::filesystem::path p(model_path);
    current_model_name_ = p.stem().string();

    Logger::Get().Info("Starting llama-server with model: " + model_path);
    Logger::Get().Info("GPU layers: " + std::to_string(gpu_layers));
    Logger::Get().Info("Context size: " + std::to_string(context_size));

    auto& server_mgr = LlamaServerManager::Get();

    if (!server_mgr.Start(model_path, gpu_layers, context_size, 18080)) {
        Logger::Get().Error("Failed to start llama-server");
        return false;
    }

    if (!server_mgr.WaitForReady(120)) {
        Logger::Get().Error("llama-server failed to become ready");
        return false;
    }

    initialized_ = true;
    Logger::Get().Info("LlamaEngine initialized successfully");
    Logger::Get().Info("Model: " + current_model_name_);
    Logger::Get().Info("Precision: " + precision);

    return true;
}

bool LlamaEngine::IsInitialized() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return initialized_;
}

bool LlamaEngine::SwitchModel(const std::string& model_path) {
    auto& server_mgr = LlamaServerManager::Get();
    if (!server_mgr.Restart(model_path, gpu_layers_, context_size_)) {
        Logger::Get().Error("Failed to switch model");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::filesystem::path p(model_path);
        current_model_name_ = p.stem().string();
        model_path_ = model_path;
    }

    Logger::Get().Info("Model switched to: " + current_model_name_);

    if (!server_mgr.WaitForReady(120)) {
        Logger::Get().Error("New model failed to become ready");
        return false;
    }

    return true;
}

bool LlamaEngine::LoadModel(const std::string& model_path) {
    return SwitchModel(model_path);
}

std::string LlamaEngine::role_to_string(MessageRole role) const {
    switch (role) {
        case MessageRole::System: return "system";
        case MessageRole::User: return "user";
        case MessageRole::Assistant: return "assistant";
        case MessageRole::Tool: return "tool";
        default: return "user";
    }
}

struct HttpResult {
    DWORD status_code = 0;
    std::string body;
};

static HttpResult HttpPost(const std::string& path, const std::string& json_body, int port) {
#ifdef _WIN32
    HINTERNET hSession = WinHttpOpen(L"InferDeck/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return {};

    std::wstring host = L"127.0.0.1";
    std::wstring path_w(path.begin(), path.end());

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), static_cast<INTERNET_PORT>(port), 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return {};
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", path_w.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return {};
    }

    // Send raw UTF-8 bytes - NOT wide strings
    std::wstring headers = L"Content-Type: application/json\r\n";
    BOOL result = WinHttpSendRequest(hRequest, headers.c_str(), -1,
        (LPVOID)json_body.c_str(), static_cast<DWORD>(json_body.size()),
        static_cast<DWORD>(json_body.size()), 0);

    if (!result) {
        Logger::Get().Error("WinHttpSendRequest failed: " + std::to_string(GetLastError()));
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return {};
    }

    result = WinHttpReceiveResponse(hRequest, nullptr);
    if (!result) {
        Logger::Get().Error("WinHttpReceiveResponse failed: " + std::to_string(GetLastError()));
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return {};
    }

    HttpResult http_result;

    DWORD statusCode = 0;
    DWORD statusCodeSize = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeSize, WINHTTP_NO_HEADER_INDEX);
    http_result.status_code = statusCode;

    DWORD bytesAvailable = 0;
    DWORD bytesRead = 0;
    char buffer[65536];

    while (WinHttpQueryDataAvailable(hRequest, &bytesAvailable) && bytesAvailable > 0) {
        if (bytesAvailable > sizeof(buffer)) bytesAvailable = sizeof(buffer);
        if (WinHttpReadData(hRequest, buffer, bytesAvailable, &bytesRead)) {
            http_result.body.append(buffer, bytesRead);
        } else {
            break;
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return http_result;
#else
    return {};
#endif
}

std::string LlamaEngine::HttpPostJson(const std::string& path, const std::string& json_body) const {
    auto& server_mgr = LlamaServerManager::Get();
    return HttpPost(path, json_body, server_mgr.GetPort()).body;
}

HttpStreamResult LlamaEngine::HttpPostStream(const std::string& path, const std::string& json_body, TokenCallback on_token) const {
#ifdef _WIN32
    auto& server_mgr = LlamaServerManager::Get();
    int port = server_mgr.GetPort();

    HINTERNET hSession = WinHttpOpen(L"InferDeck/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return {};

    std::wstring host = L"127.0.0.1";
    std::wstring path_w(path.begin(), path.end());

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), static_cast<INTERNET_PORT>(port), 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return {};
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", path_w.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return {};
    }

    // Send raw UTF-8 bytes
    std::wstring headers = L"Content-Type: application/json\r\n";
    BOOL result = WinHttpSendRequest(hRequest, headers.c_str(), -1,
        (LPVOID)json_body.c_str(), static_cast<DWORD>(json_body.size()),
        static_cast<DWORD>(json_body.size()), 0);

    if (!result) {
        Logger::Get().Error("WinHttpSendRequest failed: " + std::to_string(GetLastError()));
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return {};
    }

    result = WinHttpReceiveResponse(hRequest, nullptr);
    if (!result) {
        Logger::Get().Error("WinHttpReceiveResponse failed: " + std::to_string(GetLastError()));
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return {};
    }

    HttpStreamResult stream_result;
    DWORD bytesAvailable = 0;
    DWORD bytesRead = 0;
    char buffer[65536];
    std::string leftover;

    while (WinHttpQueryDataAvailable(hRequest, &bytesAvailable) && bytesAvailable > 0) {
        if (bytesAvailable > sizeof(buffer)) bytesAvailable = sizeof(buffer);
        if (WinHttpReadData(hRequest, buffer, bytesAvailable, &bytesRead)) {
            std::string chunk(buffer, bytesRead);
            std::string data = leftover + chunk;

            size_t pos = 0;
            while ((pos = data.find("data: ", pos)) != std::string::npos) {
                size_t end = data.find("\n", pos);
                if (end == std::string::npos) {
                    leftover = data.substr(pos);
                    break;
                }
                std::string line = data.substr(pos + 6, end - pos - 6);
                pos = end + 1;

                if (line == "[DONE]") break;
                if (line.empty()) continue;

                try {
                    auto j = json::parse(line);
                    if (j.contains("choices") && j["choices"].is_array() && !j["choices"].empty()) {
                        auto& choice = j["choices"][0];
                        if (choice.contains("delta")) {
                            auto& delta = choice["delta"];
                            std::string content_token;
                            std::string reasoning_token;
                            if (delta.contains("content") && !delta["content"].is_null() && delta["content"].is_string()) {
                                content_token = delta["content"].get<std::string>();
                            }
                            if (delta.contains("reasoning_content") && !delta["reasoning_content"].is_null() && delta["reasoning_content"].is_string()) {
                                reasoning_token = delta["reasoning_content"].get<std::string>();
                            }
                            if (!content_token.empty()) {
                                stream_result.content_text += content_token;
                                if (on_token) on_token(content_token, TokenType::Content, static_cast<int>(stream_result.content_text.size()));
                            }
                            if (!reasoning_token.empty()) {
                                stream_result.reasoning_text += reasoning_token;
                                if (on_token) on_token(reasoning_token, TokenType::Reasoning, static_cast<int>(stream_result.reasoning_text.size()));
                            }
                        }
                    }
                } catch (const std::exception& e) {
                    Logger::Get().Warn("SSE parse error: " + std::string(e.what()));
                }
            }

            if (pos >= data.size()) leftover.clear();
            else leftover = data.substr(pos);
        } else {
            break;
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return stream_result;
#else
    return {};
#endif
}

InferenceResult LlamaEngine::Predict(const std::vector<ChatMessage>& messages,
                                      const InferenceParams& params) {
    auto start = std::chrono::high_resolution_clock::now();

    json body;
    body["messages"] = json::array();
    for (const auto& msg : messages) {
        json msg_json;
        msg_json["role"] = role_to_string(msg.role);
        if (msg.role == MessageRole::Tool) {
            msg_json["tool_call_id"] = msg.tool_call_id;
            msg_json["content"] = msg.content;
        } else {
            msg_json["content"] = msg.content;
            if (!msg.name.empty()) msg_json["name"] = msg.name;
            if (!msg.tool_calls_json.empty()) {
                msg_json["tool_calls"] = json::parse(msg.tool_calls_json);
            }
        }
        body["messages"].push_back(msg_json);
    }
    body["max_tokens"] = params.max_tokens;
    body["temperature"] = params.temperature;
    body["top_p"] = params.top_p;
    body["stream"] = false;
    if (!params.tools_json.empty()) {
        body["tools"] = json::parse(params.tools_json);
    }

    auto& server_mgr = LlamaServerManager::Get();
    auto http_result = HttpPost("/v1/chat/completions", body.dump(), server_mgr.GetPort());

    auto end = std::chrono::high_resolution_clock::now();
    float duration = std::chrono::duration<float, std::milli>(end - start).count();

    InferenceResult result;
    result.duration_ms = duration;
    result.http_status = static_cast<int>(http_result.status_code);

    if (http_result.status_code != 200) {
        result.error_message = http_result.body.empty()
            ? "llama-server returned HTTP " + std::to_string(http_result.status_code)
            : http_result.body;
        Logger::Get().Error("llama-server returned HTTP " + std::to_string(http_result.status_code) + ": " + http_result.body);
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.total_requests++;
        stats_.failed_requests++;
        return result;
    }

    try {
        auto j = json::parse(http_result.body);
        if (j.contains("error")) {
            Logger::Get().Error("llama-server error: " + j["error"].dump());
            result.error_message = j["error"].dump();
        }
        if (j.contains("choices") && j["choices"].is_array() && !j["choices"].empty()) {
            auto& message = j["choices"][0]["message"];
            if (message.contains("content") && !message["content"].is_null()) {
                if (message["content"].is_string()) {
                    result.text = message["content"].get<std::string>();
                } else if (message["content"].is_array()) {
                    for (const auto& part : message["content"]) {
                        if (part.contains("type") && part["type"] == "text" && part.contains("text")) {
                            result.text += part["text"].get<std::string>();
                        } else if (part.is_string()) {
                            result.text += part.get<std::string>();
                        }
                    }
                }
            }
            if (message.contains("reasoning_content") && !message["reasoning_content"].is_null()) {
                if (message["reasoning_content"].is_string()) {
                    result.reasoning_text = message["reasoning_content"].get<std::string>();
                } else if (message["reasoning_content"].is_array()) {
                    for (const auto& part : message["reasoning_content"]) {
                        if (part.contains("text") && part["text"].is_string()) {
                            result.reasoning_text += part["text"].get<std::string>();
                        } else if (part.is_string()) {
                            result.reasoning_text += part.get<std::string>();
                        }
                    }
                }
            }
            if (message.contains("tool_calls") && message["tool_calls"].is_array()) {
                for (const auto& tc : message["tool_calls"]) {
                    ToolCall tool_call;
                    tool_call.id = tc.value("id", "");
                    tool_call.type = tc.value("type", "function");
                    if (tc.contains("function") && tc["function"].is_object()) {
                        tool_call.function_name = tc["function"].value("name", "");
                        tool_call.function_arguments = tc["function"].value("arguments", "");
                    }
                    result.tool_calls.push_back(tool_call);
                }
            }
        }
        if (j.contains("usage")) {
            result.prompt_tokens = j["usage"].value("prompt_tokens", 0);
            result.completion_tokens = j["usage"].value("completion_tokens", 0);
            result.total_tokens = j["usage"].value("total_tokens", 0);
        }
    } catch (const std::exception& e) {
        Logger::Get().Error("Failed to parse llama-server response: " + std::string(e.what()));
        Logger::Get().Error("Raw response: " + http_result.body);
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.total_requests++;
        stats_.successful_requests++;
        stats_.avg_latency_ms = (stats_.avg_latency_ms * (stats_.successful_requests - 1) + duration) / stats_.successful_requests;
        stats_.max_latency_ms = std::max(stats_.max_latency_ms, duration);
        stats_.min_latency_ms = std::min(stats_.min_latency_ms, duration);
        stats_.tokens_generated += result.completion_tokens;
        stats_.tokens_processed += result.prompt_tokens;
    }

    Logger::Get().Info("Inference complete: " + std::to_string(result.completion_tokens) + " tokens in " +
                       std::to_string(duration) + "ms");

    return result;
}

InferenceResult LlamaEngine::PredictStream(const std::vector<ChatMessage>& messages,
                                            const InferenceParams& params,
                                            TokenCallback on_token) {
    auto start = std::chrono::high_resolution_clock::now();

    json body;
    body["messages"] = json::array();
    for (const auto& msg : messages) {
        json msg_json;
        msg_json["role"] = role_to_string(msg.role);
        if (msg.role == MessageRole::Tool) {
            msg_json["tool_call_id"] = msg.tool_call_id;
            msg_json["content"] = msg.content;
        } else {
            msg_json["content"] = msg.content;
            if (!msg.name.empty()) msg_json["name"] = msg.name;
            if (!msg.tool_calls_json.empty()) {
                msg_json["tool_calls"] = json::parse(msg.tool_calls_json);
            }
        }
        body["messages"].push_back(msg_json);
    }
    body["max_tokens"] = params.max_tokens;
    body["temperature"] = params.temperature;
    body["top_p"] = params.top_p;
    body["stream"] = true;
    if (!params.tools_json.empty()) {
        body["tools"] = json::parse(params.tools_json);
    }

    HttpStreamResult stream_result = HttpPostStream("/v1/chat/completions", body.dump(), on_token);

    auto end = std::chrono::high_resolution_clock::now();
    float duration = std::chrono::duration<float, std::milli>(end - start).count();

    InferenceResult result;
    result.text = stream_result.content_text;
    result.reasoning_text = stream_result.reasoning_text;
    result.duration_ms = duration;
    result.completion_tokens = static_cast<int>((stream_result.content_text.size() + stream_result.reasoning_text.size()) / 4);
    result.total_tokens = result.completion_tokens;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.total_requests++;
        stats_.successful_requests++;
        stats_.avg_latency_ms = (stats_.avg_latency_ms * (stats_.successful_requests - 1) + duration) / stats_.successful_requests;
        stats_.max_latency_ms = std::max(stats_.max_latency_ms, duration);
        stats_.min_latency_ms = std::min(stats_.min_latency_ms, duration);
        stats_.tokens_generated += result.completion_tokens;
    }

    Logger::Get().Info("Stream inference complete: " + std::to_string(result.completion_tokens) + " tokens in " +
                       std::to_string(duration) + "ms");

    return result;
}

InferenceStats LlamaEngine::GetStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

std::string LlamaEngine::GetModelName() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_model_name_;
}

std::string LlamaEngine::GetPrecision() const {
    return precision_;
}

void LlamaEngine::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) {
        LlamaServerManager::Get().Stop();
        initialized_ = false;
        Logger::Get().Info("LlamaEngine shut down");
    }
}

GpuInfo LlamaEngine::GetGpuInfo() const {
    GpuInfo info;
    info.name = "AMD GPU (HIP/Radeon)";
    info.is_discrete = true;
    return info;
}

} // namespace inferdeck::core
