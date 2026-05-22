#include "llama_cpp/LlamaServerManager.hpp"
#include "core/Logger.hpp"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#endif

#include <chrono>
#include <thread>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <vector>

namespace inferdeck::core {

LlamaServerManager& LlamaServerManager::Get() {
    static LlamaServerManager instance;
    return instance;
}

LlamaServerManager::LlamaServerManager() = default;
LlamaServerManager::~LlamaServerManager() {
    Stop();
}

std::string LlamaServerManager::GetExecutablePath() const {
    return (std::filesystem::path(GetRuntimeDirectory()) / "llama-server.exe").string();
}

std::string LlamaServerManager::GetRuntimeDirectory() const {
    return (std::filesystem::current_path() / "runtime" / "llama-b9276-vulkan").string();
}

std::string LlamaServerManager::GetCurrentModelPath() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_model_path_;
}

uint32_t LlamaServerManager::GetPid() const {
    return pid_;
}

static bool ContainsForbiddenBackend(const std::filesystem::path& runtime_dir) {
    static const std::vector<std::string> forbidden = {"ggml-hip.dll", "ggml-cuda.dll", "ggml-sycl.dll"};
    for (const auto& name : forbidden) {
        if (std::filesystem::exists(runtime_dir / name)) {
            return true;
        }
    }
    return false;
}

static bool ValidateVulkanRuntime(const std::filesystem::path& runtime_dir) {
    return std::filesystem::exists(runtime_dir / "llama-server.exe") &&
           std::filesystem::exists(runtime_dir / "ggml-vulkan.dll") &&
           std::filesystem::exists(runtime_dir / "llama.dll") &&
           std::filesystem::exists(runtime_dir / "llama-common.dll") &&
           !ContainsForbiddenBackend(runtime_dir);
}

bool LlamaServerManager::DownloadIfNeeded() const {
    auto exe_path = GetExecutablePath();
    auto runtime_dir = std::filesystem::path(GetRuntimeDirectory());
    if (ValidateVulkanRuntime(runtime_dir)) {
        Logger::Get().Info("llama.cpp runtime: b9276 Windows x64 Vulkan");
        Logger::Get().Info("llama-server.exe: " + exe_path);
        Logger::Get().Info("Vulkan backend DLL: " + (runtime_dir / "ggml-vulkan.dll").string());
        return true;
    }
    Logger::Get().Error("Required b9276 Vulkan runtime is missing or polluted with HIP/CUDA/SYCL files: " + runtime_dir.string());
    return false;
}

bool LlamaServerManager::DownloadBinary() const {
    Logger::Get().Error("Automatic llama.cpp downloads are disabled. Install b9276 Windows x64 Vulkan under runtime/llama-b9276-vulkan.");
    return false;
}

bool LlamaServerManager::Start(const std::string& model_path, int gpu_layers, int context_size, int port) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (running_.load() && std::filesystem::equivalent(std::filesystem::path(current_model_path_), std::filesystem::path(model_path))) {
        Logger::Get().Info("llama-server already running requested model; reusing PID " + std::to_string(pid_));
        return true;
    }

    if (!DownloadIfNeeded()) {
        Logger::Get().Error("Failed to download llama-server binary");
        return false;
    }

    if (!std::filesystem::exists(model_path)) {
        Logger::Get().Error("Model file not found: " + model_path);
        return false;
    }

    port_ = port;

    if (!LaunchProcess(model_path, gpu_layers, context_size, port)) {
        Logger::Get().Error("Failed to launch llama-server");
        return false;
    }

    running_.store(true);
    current_model_path_ = model_path;
    Logger::Get().Info("llama-server started on port " + std::to_string(port));
    return true;
}

bool LlamaServerManager::LaunchProcess(const std::string& model_path, int gpu_layers, int context_size, int port) {
#ifdef _WIN32
    auto exe_path = GetExecutablePath();

    std::string cmd = "\"" + exe_path + "\"";
    cmd += " --model \"" + model_path + "\"";
    cmd += " --ctx-size " + std::to_string(context_size);
    cmd += " --gpu-layers " + (gpu_layers < 0 ? std::string("all") : std::to_string(gpu_layers));
    cmd += " --host 127.0.0.1";
    cmd += " --port " + std::to_string(port);
    cmd += " --n-predict -1";
    cmd += " --flash-attn on";
    cmd += " --cache-type-k q8_0";
    cmd += " --cache-type-v q8_0";
    cmd += " --main-gpu 0";
    cmd += " --split-mode none";
    cmd += " --no-mmap";
    cmd += " --cache-ram 0";
    cmd += " --fit";
    cmd += " --fit-target 512";
    cmd += " --parallel 1";
    cmd += " --kv-unified";

    Logger::Get().Info("Launching llama-server command: " + cmd);
    Logger::Get().Info("Selected model path: " + model_path);
    Logger::Get().Info("Resolved Vulkan runtime directory: " + GetRuntimeDirectory());

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {};

    std::wstring cmd_w(cmd.begin(), cmd.end());
    std::wstring exe_dir_w = std::filesystem::path(exe_path).parent_path().wstring();

    if (!CreateProcessW(
        nullptr,
        &cmd_w[0],
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        exe_dir_w.c_str(),
        &si,
        &pi
    )) {
        DWORD err = GetLastError();
        Logger::Get().Error("CreateProcess failed, error: " + std::to_string(err));
        return false;
    }

    process_handle_ = pi.hProcess;
    pid_ = pi.dwProcessId;
    CloseHandle(pi.hThread);

    Logger::Get().Info("llama-server process started, PID: " + std::to_string(pid_));
    return true;
#else
    return false;
#endif
}

void LlamaServerManager::Stop() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!running_.load()) return;

    KillProcess();
    running_.store(false);
    Logger::Get().Info("llama-server stopped");
}

bool LlamaServerManager::KillProcess() {
#ifdef _WIN32
    if (process_handle_) {
        // Try graceful shutdown first
        DWORD exitCode = 0;
        GetExitCodeProcess(process_handle_, &exitCode);
        if (exitCode == STILL_ACTIVE) {
            TerminateProcess(process_handle_, 0);
        }
        CloseHandle(process_handle_);
        process_handle_ = nullptr;
        pid_ = 0;
        return true;
    }
#endif
    return false;
}

bool LlamaServerManager::IsRunning() const {
    return running_.load();
}

int LlamaServerManager::GetPort() const {
    return port_;
}

bool LlamaServerManager::WaitForReady(int timeout_seconds) const {
    Logger::Get().Info("Waiting for llama-server to be ready (timeout: " + std::to_string(timeout_seconds) + "s)...");

    auto start = std::chrono::steady_clock::now();
    auto timeout = std::chrono::seconds(timeout_seconds);

#ifdef _WIN32
    while (std::chrono::steady_clock::now() - start < timeout) {
        HINTERNET hSession = WinHttpOpen(L"InferDeck/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (hSession) {
            std::wstring host = L"127.0.0.1";
            std::wstring path = L"/health";
            HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), port_, 0);
            if (hConnect) {
                HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(),
                    nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
                if (hRequest) {
                    if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                        WinHttpReceiveResponse(hRequest, nullptr)) {
                        DWORD statusCode = 0;
                        DWORD size = sizeof(statusCode);
                        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &size, WINHTTP_NO_HEADER_INDEX);
                        WinHttpCloseHandle(hRequest);
                        WinHttpCloseHandle(hConnect);
                        WinHttpCloseHandle(hSession);

                        if (statusCode == 200) {
                            Logger::Get().Info("llama-server is ready!");
                            return true;
                        }
                    } else {
                        WinHttpCloseHandle(hRequest);
                    }
                }
                WinHttpCloseHandle(hConnect);
            }
            WinHttpCloseHandle(hSession);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
#else
    (void)timeout;
#endif

    Logger::Get().Error("llama-server did not become ready within timeout");
    return false;
}

bool LlamaServerManager::Restart(const std::string& new_model_path, int gpu_layers, int context_size) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (running_.load()) {
        if (!current_model_path_.empty()) {
            try {
                if (std::filesystem::equivalent(std::filesystem::path(current_model_path_), std::filesystem::path(new_model_path))) {
                    Logger::Get().Info("Requested model already loaded; skipping restart for: " + new_model_path);
                    return true;
                }
            } catch (...) {
                if (current_model_path_ == new_model_path) {
                    Logger::Get().Info("Requested model already loaded; skipping restart for: " + new_model_path);
                    return true;
                }
            }
        }
        KillProcess();
        running_.store(false);
    }

    if (!DownloadIfNeeded()) {
        Logger::Get().Error("Failed to download llama-server binary");
        return false;
    }

    if (!std::filesystem::exists(new_model_path)) {
        Logger::Get().Error("Model file not found: " + new_model_path);
        return false;
    }

    if (!LaunchProcess(new_model_path, gpu_layers, context_size, port_)) {
        Logger::Get().Error("Failed to launch llama-server");
        return false;
    }

    running_.store(true);
    current_model_path_ = new_model_path;
    Logger::Get().Info("llama-server started on port " + std::to_string(port_));
    return true;
}

} // namespace inferdeck::core
