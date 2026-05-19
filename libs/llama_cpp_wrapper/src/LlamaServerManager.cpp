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
    return (std::filesystem::current_path() / "llama-server.exe").string();
}

bool LlamaServerManager::DownloadIfNeeded() const {
    auto exe_path = GetExecutablePath();
    if (std::filesystem::exists(exe_path)) {
        Logger::Get().Info("llama-server.exe already exists at: " + exe_path);
        return true;
    }
    Logger::Get().Info("llama-server.exe not found, downloading...");
    return DownloadBinary();
}

bool LlamaServerManager::DownloadBinary() const {
#ifdef _WIN32
    auto exe_path = GetExecutablePath();
    auto exe_dir = std::filesystem::path(exe_path).parent_path();
    std::filesystem::create_directories(exe_dir);

    // Latest stable release: b9222 - AMD GPU uses HIP/Radeon build
    const std::string url = "https://github.com/ggml-org/llama.cpp/releases/download/b9222/llama-b9222-bin-win-hip-radeon-x64.zip";
    const std::string zip_path = (exe_dir / "llama-hip-radeon.zip").string();

    Logger::Get().Info("Downloading llama.cpp AMD HIP/Radeon binary from GitHub releases...");

    // Use WinHTTP to download
    HINTERNET hSession = WinHttpOpen(L"InferDeck/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        Logger::Get().Error("WinHttpOpen failed");
        return false;
    }

    HINTERNET hConnect = WinHttpConnect(hSession, L"github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        Logger::Get().Error("WinHttpConnect failed");
        return false;
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET",
        L"/ggml-org/llama.cpp/releases/download/b9222/llama-b9222-bin-win-hip-radeon-x64.zip",
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        Logger::Get().Error("WinHttpOpenRequest failed");
        return false;
    }

    BOOL bResults = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (!bResults) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        Logger::Get().Error("WinHttpSendRequest failed");
        return false;
    }

    bResults = WinHttpReceiveResponse(hRequest, nullptr);
    if (!bResults) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        Logger::Get().Error("WinHttpReceiveResponse failed");
        return false;
    }

    DWORD statusCode = 0;
    DWORD statusCodeSize = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeSize, WINHTTP_NO_HEADER_INDEX);

    if (statusCode != 200) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        Logger::Get().Error("Download failed with HTTP status: " + std::to_string(statusCode));
        return false;
    }

    // Download in chunks
    std::ofstream out(zip_path, std::ios::binary);
    if (!out) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        Logger::Get().Error("Cannot open file for writing: " + zip_path);
        return false;
    }

    DWORD bytesAvailable = 0;
    DWORD bytesRead = 0;
    char buffer[65536];
    int64_t totalDownloaded = 0;

    while (WinHttpQueryDataAvailable(hRequest, &bytesAvailable) && bytesAvailable > 0) {
        if (bytesAvailable > sizeof(buffer)) bytesAvailable = sizeof(buffer);
        if (WinHttpReadData(hRequest, buffer, bytesAvailable, &bytesRead)) {
            out.write(buffer, bytesRead);
            totalDownloaded += bytesRead;
            if (totalDownloaded % (10 * 1024 * 1024) == 0) {
                Logger::Get().Info("Downloaded: " + std::to_string(totalDownloaded / (1024 * 1024)) + " MB");
            }
        } else {
            break;
        }
    }
    out.close();

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    Logger::Get().Info("Download complete: " + std::to_string(totalDownloaded / (1024 * 1024)) + " MB");

    // Extract zip using PowerShell
    std::wstring extract_cmd = L"powershell -Command \"Expand-Archive -Path '" + std::filesystem::path(zip_path).wstring() + L"' -DestinationPath '" + exe_dir.wstring() + L"' -Force\"";
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    if (!CreateProcessW(nullptr, &extract_cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        Logger::Get().Error("Failed to extract zip file");
        return false;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    // Clean up zip
    std::filesystem::remove(zip_path);

    // Check if llama-server.exe exists (might be in a subdirectory)
    auto server_exe = std::filesystem::path(exe_path);
    if (!std::filesystem::exists(server_exe)) {
        // Search for it in subdirectories
        for (const auto& entry : std::filesystem::recursive_directory_iterator(exe_dir)) {
            if (entry.is_regular_file() && entry.path().filename() == "llama-server.exe") {
                std::filesystem::copy_file(entry.path(), server_exe, std::filesystem::copy_options::overwrite_existing);
                break;
            }
        }
    }

    if (!std::filesystem::exists(server_exe)) {
        Logger::Get().Error("llama-server.exe not found after extraction");
        return false;
    }

    Logger::Get().Info("llama-server.exe extracted to: " + exe_path);
    return true;
#else
    Logger::Get().Error("Download not implemented on this platform");
    return false;
#endif
}

bool LlamaServerManager::Start(const std::string& model_path, int gpu_layers, int context_size, int port) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (running_.load()) {
        Logger::Get().Warn("llama-server already running");
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
    Logger::Get().Info("llama-server started on port " + std::to_string(port));
    return true;
}

bool LlamaServerManager::LaunchProcess(const std::string& model_path, int gpu_layers, int context_size, int port) {
#ifdef _WIN32
    auto exe_path = GetExecutablePath();

    std::string cmd = "\"" + exe_path + "\"";
    cmd += " --model \"" + model_path + "\"";
    cmd += " --ctx-size " + std::to_string(context_size);
    cmd += " --gpu-layers " + std::to_string(gpu_layers);
    cmd += " --host 127.0.0.1";
    cmd += " --port " + std::to_string(port);
    cmd += " --n-predict -1";

    Logger::Get().Info("Launching: " + cmd);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {};

    // Convert to wide string for CreateProcessW
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
    CloseHandle(pi.hThread);

    Logger::Get().Info("llama-server process started, PID: " + std::to_string(pi.dwProcessId));
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
        KillProcess();
        running_.store(false);
    }

    return Start(new_model_path, gpu_layers, context_size, port_);
}

} // namespace inferdeck::core
