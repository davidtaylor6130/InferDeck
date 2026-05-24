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
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ws2_32.lib")
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

static std::string DecodeChunkedBody(const std::string& encoded) {
    std::string decoded;
    std::size_t pos = 0;
    while (pos < encoded.size()) {
        std::size_t line_end = encoded.find("\r\n", pos);
        if (line_end == std::string::npos) break;
        std::string size_text = encoded.substr(pos, line_end - pos);
        std::size_t extension_pos = size_text.find(';');
        if (extension_pos != std::string::npos) size_text = size_text.substr(0, extension_pos);
        std::size_t chunk_size = 0;
        try {
            chunk_size = static_cast<std::size_t>(std::stoull(size_text, nullptr, 16));
        } catch (...) {
            break;
        }
        pos = line_end + 2;
        if (chunk_size == 0) break;
        if (pos + chunk_size > encoded.size()) break;
        decoded.append(encoded, pos, chunk_size);
        pos += chunk_size;
        if (encoded.compare(pos, 2, "\r\n") == 0) pos += 2;
    }
    return decoded;
}

static constexpr DWORD kBackendResolveTimeoutMs = 30000;
static constexpr DWORD kBackendConnectTimeoutMs = 30000;
static constexpr DWORD kBackendSendTimeoutMs = 300000;
static constexpr DWORD kBackendReceiveTimeoutMs = 1800000;
static constexpr DWORD kBackendStreamReceiveTimeoutMs = 15000;
static constexpr auto kBackendStreamTotalTimeout = std::chrono::minutes(30);

static HttpResult HttpPost(const std::string& path, const std::string& json_body, int port) {
#ifdef _WIN32
    HttpResult http_result;

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        Logger::Get().Error("WSAStartup failed: " + std::to_string(WSAGetLastError()));
        return http_result;
    }

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        Logger::Get().Error("socket failed: " + std::to_string(WSAGetLastError()));
        WSACleanup();
        return http_result;
    }

    DWORD recv_timeout = kBackendReceiveTimeoutMs;
    DWORD send_timeout = kBackendSendTimeoutMs;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&recv_timeout), sizeof(recv_timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&send_timeout), sizeof(send_timeout));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        Logger::Get().Error("connect to llama-server failed: " + std::to_string(WSAGetLastError()));
        closesocket(sock);
        WSACleanup();
        return http_result;
    }

    std::string request =
        "POST " + path + " HTTP/1.1\r\n"
        "Host: 127.0.0.1:" + std::to_string(port) + "\r\n"
        "Content-Type: application/json\r\n"
        "Accept: application/json\r\n"
        "Connection: close\r\n"
        "Content-Length: " + std::to_string(json_body.size()) + "\r\n\r\n" +
        json_body;

    const char* send_ptr = request.data();
    int remaining = static_cast<int>(request.size());
    while (remaining > 0) {
        int sent = send(sock, send_ptr, remaining, 0);
        if (sent == SOCKET_ERROR || sent == 0) {
            Logger::Get().Error("send to llama-server failed: " + std::to_string(WSAGetLastError()));
            closesocket(sock);
            WSACleanup();
            return http_result;
        }
        send_ptr += sent;
        remaining -= sent;
    }

    std::string raw;
    raw.reserve(65536);
    char buffer[65536];
    std::size_t header_end = std::string::npos;
    std::size_t content_length = 0;
    bool have_content_length = false;
    bool chunked = false;

    while (true) {
        int received = recv(sock, buffer, sizeof(buffer), 0);
        if (received == 0) break;
        if (received == SOCKET_ERROR) {
            const auto error = WSAGetLastError();
            if (header_end != std::string::npos && error == WSAETIMEDOUT) {
                break;
            }
            Logger::Get().Error("recv from llama-server failed: " + std::to_string(error));
            break;
        }
        raw.append(buffer, static_cast<std::size_t>(received));

        if (header_end == std::string::npos) {
            header_end = raw.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                auto headers = raw.substr(0, header_end);
                std::istringstream status_stream(headers);
                std::string http_version;
                status_stream >> http_version >> http_result.status_code;

                std::string lower_headers = headers;
                std::transform(lower_headers.begin(), lower_headers.end(), lower_headers.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                auto content_pos = lower_headers.find("content-length:");
                if (content_pos != std::string::npos) {
                    content_pos += std::string("content-length:").size();
                    while (content_pos < lower_headers.size() && std::isspace(static_cast<unsigned char>(lower_headers[content_pos]))) {
                        ++content_pos;
                    }
                    std::size_t end_pos = lower_headers.find("\r\n", content_pos);
                    try {
                        content_length = static_cast<std::size_t>(std::stoull(lower_headers.substr(content_pos, end_pos - content_pos)));
                        have_content_length = true;
                    } catch (...) {
                        have_content_length = false;
                    }
                }
                chunked = lower_headers.find("transfer-encoding: chunked") != std::string::npos;
                if (!have_content_length && !chunked) {
                    DWORD post_header_timeout = 2000;
                    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&post_header_timeout), sizeof(post_header_timeout));
                }
            }
        }

        if (header_end != std::string::npos && have_content_length) {
            const std::size_t body_start = header_end + 4;
            if (raw.size() >= body_start + content_length) break;
        }
        if (header_end != std::string::npos && chunked) {
            const std::size_t body_start = header_end + 4;
            auto body = raw.substr(body_start);
            if (body.find("\r\n0\r\n\r\n") != std::string::npos || body.find("\n0\r\n\r\n") != std::string::npos) break;
        }
    }

    closesocket(sock);
    WSACleanup();

    if (header_end == std::string::npos) {
        Logger::Get().Error("llama-server response ended before headers");
        return http_result;
    }
    const std::size_t body_start = header_end + 4;
    if (chunked) {
        http_result.body = DecodeChunkedBody(raw.substr(body_start));
    } else {
        http_result.body = raw.substr(body_start, have_content_length ? content_length : std::string::npos);
    }
    return http_result;
#if 0
    HINTERNET hSession = WinHttpOpen(L"InferDeck/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return {};
    WinHttpSetTimeouts(hSession, kBackendResolveTimeoutMs, kBackendConnectTimeoutMs, kBackendSendTimeoutMs, kBackendReceiveTimeoutMs);

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
    WinHttpSetTimeouts(hRequest, kBackendResolveTimeoutMs, kBackendConnectTimeoutMs, kBackendSendTimeoutMs, kBackendReceiveTimeoutMs);

    // Send raw UTF-8 bytes - NOT wide strings
    std::wstring headers = L"Content-Type: application/json\r\nConnection: close\r\n";
    BOOL result = WinHttpSendRequest(hRequest, headers.c_str(), static_cast<DWORD>(-1L),
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
            if (bytesRead == 0) break;
            http_result.body.append(buffer, bytesRead);
        } else {
            Logger::Get().Error("WinHttpReadData failed: " + std::to_string(GetLastError()));
            break;
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return http_result;
#endif
#else
    return {};
#endif
}

std::string LlamaEngine::HttpPostJson(const std::string& path, const std::string& json_body) const {
    auto& server_mgr = LlamaServerManager::Get();
    return HttpPost(path, json_body, server_mgr.GetPort()).body;
}

HttpStreamResult LlamaEngine::HttpPostStream(const std::string& path,
                                             const std::string& json_body,
                                             TokenCallback on_token,
                                             StreamHeartbeatCallback on_heartbeat) const {
#ifdef _WIN32
    auto& server_mgr = LlamaServerManager::Get();
    int port = server_mgr.GetPort();

    HINTERNET hSession = WinHttpOpen(L"InferDeck/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        HttpStreamResult failed;
        failed.error_message = "WinHttpOpen failed";
        return failed;
    }
    WinHttpSetTimeouts(hSession, kBackendResolveTimeoutMs, kBackendConnectTimeoutMs, kBackendSendTimeoutMs, kBackendStreamReceiveTimeoutMs);

    std::wstring host = L"127.0.0.1";
    std::wstring path_w(path.begin(), path.end());

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), static_cast<INTERNET_PORT>(port), 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        HttpStreamResult failed;
        failed.error_message = "WinHttpConnect failed";
        return failed;
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", path_w.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        HttpStreamResult failed;
        failed.error_message = "WinHttpOpenRequest failed";
        return failed;
    }
    WinHttpSetTimeouts(hRequest, kBackendResolveTimeoutMs, kBackendConnectTimeoutMs, kBackendSendTimeoutMs, kBackendStreamReceiveTimeoutMs);

    // Send raw UTF-8 bytes
    std::wstring headers = L"Content-Type: application/json\r\n";
    BOOL result = WinHttpSendRequest(hRequest, headers.c_str(), static_cast<DWORD>(-1L),
        (LPVOID)json_body.c_str(), static_cast<DWORD>(json_body.size()),
        static_cast<DWORD>(json_body.size()), 0);

    if (!result) {
        DWORD error = GetLastError();
        Logger::Get().Error("WinHttpSendRequest failed: " + std::to_string(error));
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        HttpStreamResult failed;
        failed.error_message = "WinHttpSendRequest failed: " + std::to_string(error);
        return failed;
    }

    result = WinHttpReceiveResponse(hRequest, nullptr);
    if (!result) {
        DWORD error = GetLastError();
        Logger::Get().Error("WinHttpReceiveResponse failed: " + std::to_string(error));
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        HttpStreamResult failed;
        failed.error_message = "WinHttpReceiveResponse failed: " + std::to_string(error);
        return failed;
    }

    HttpStreamResult stream_result;
    DWORD statusCode = 0;
    DWORD statusCodeSize = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeSize, WINHTTP_NO_HEADER_INDEX);
    stream_result.http_status = static_cast<int>(statusCode);
    DWORD bytesAvailable = 0;
    DWORD bytesRead = 0;
    char buffer[65536];
    std::string leftover;
    bool saw_done = false;
    auto started_at = std::chrono::steady_clock::now();

    while (true) {
        if (std::chrono::steady_clock::now() - started_at > kBackendStreamTotalTimeout) {
            stream_result.error_message = "llama-server stream exceeded total timeout";
            break;
        }

        if (!WinHttpQueryDataAvailable(hRequest, &bytesAvailable)) {
            DWORD error = GetLastError();
            if (error == ERROR_WINHTTP_TIMEOUT) {
                if (on_heartbeat) on_heartbeat();
                continue;
            }
            stream_result.error_message = "WinHttpQueryDataAvailable failed: " + std::to_string(error);
            break;
        }
        if (bytesAvailable == 0) break;

        if (bytesAvailable > sizeof(buffer)) bytesAvailable = sizeof(buffer);
        if (WinHttpReadData(hRequest, buffer, bytesAvailable, &bytesRead)) {
            if (bytesRead == 0) break;
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

                if (line == "[DONE]") {
                    saw_done = true;
                    break;
                }
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
            if (saw_done) break;
        } else {
            DWORD error = GetLastError();
            if (error == ERROR_WINHTTP_TIMEOUT) {
                if (on_heartbeat) on_heartbeat();
                continue;
            }
            stream_result.error_message = "WinHttpReadData failed: " + std::to_string(error);
            break;
        }
    }

    if (stream_result.error_message.empty() && stream_result.http_status == 200 && !saw_done) {
        stream_result.error_message = "llama-server stream ended before [DONE]";
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
                        if (tc["function"].contains("arguments")) {
                            const auto& arguments = tc["function"]["arguments"];
                            tool_call.function_arguments = arguments.is_string() ? arguments.get<std::string>() : arguments.dump();
                        }
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

    return result;
}

InferenceResult LlamaEngine::PredictStream(const std::vector<ChatMessage>& messages,
                                            const InferenceParams& params,
                                            TokenCallback on_token,
                                            StreamHeartbeatCallback on_heartbeat) {
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

    HttpStreamResult stream_result = HttpPostStream("/v1/chat/completions", body.dump(), on_token, on_heartbeat);

    auto end = std::chrono::high_resolution_clock::now();
    float duration = std::chrono::duration<float, std::milli>(end - start).count();

    InferenceResult result;
    result.http_status = stream_result.http_status;
    if (!stream_result.error_message.empty() || stream_result.http_status == 0 || stream_result.http_status >= 400) {
        result.error_message = !stream_result.error_message.empty()
            ? stream_result.error_message
            : "llama-server stream returned HTTP " + std::to_string(stream_result.http_status);
    }
    result.text = stream_result.content_text;
    result.reasoning_text = stream_result.reasoning_text;
    result.duration_ms = duration;
    result.completion_tokens = static_cast<int>((stream_result.content_text.size() + stream_result.reasoning_text.size()) / 4);
    result.total_tokens = result.completion_tokens;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.total_requests++;
        if (result.HasError()) {
            stats_.failed_requests++;
        } else {
            stats_.successful_requests++;
            stats_.avg_latency_ms = (stats_.avg_latency_ms * (stats_.successful_requests - 1) + duration) / stats_.successful_requests;
            stats_.tokens_generated += result.completion_tokens;
        }
        stats_.max_latency_ms = std::max(stats_.max_latency_ms, duration);
        stats_.min_latency_ms = std::min(stats_.min_latency_ms, duration);
    }

    if (result.HasError()) {
        Logger::Get().Error("Stream inference failed: " + result.error_message);
    } else {
        Logger::Get().Info("Stream inference complete: " + std::to_string(result.completion_tokens) + " tokens in " +
                           std::to_string(duration) + "ms");
    }

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

void LlamaEngine::AbortActiveRequest(const std::string& reason) {
    std::string model_path;
    int gpu_layers = -1;
    int context_size = 100000;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        model_path = model_path_;
        gpu_layers = gpu_layers_;
        context_size = context_size_;
    }

    if (model_path.empty()) {
        Logger::Get().Warn("Cannot abort active request because no model path is loaded: " + reason);
        return;
    }

    Logger::Get().Warn("Aborting active llama-server request by restarting backend: " + reason);
    auto& server_mgr = LlamaServerManager::Get();
    server_mgr.Stop();
    if (!server_mgr.Start(model_path, gpu_layers, context_size, server_mgr.GetPort())) {
        Logger::Get().Error("Failed to restart llama-server while aborting active request");
        return;
    }
    if (!server_mgr.WaitForReady(120)) {
        Logger::Get().Error("llama-server did not become ready after active request abort");
        return;
    }
    Logger::Get().Info("llama-server restarted after active request abort");
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
    info.name = "AMD GPU (Vulkan)";
    info.is_discrete = true;
    return info;
}

} // namespace inferdeck::core
