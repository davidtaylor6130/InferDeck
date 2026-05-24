#include "llama_cpp/LlamaServerManager.hpp"
#include "core/Logger.hpp"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>
#include <iphlpapi.h>
#include <tlhelp32.h>
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "iphlpapi.lib")
#endif

#include <chrono>
#include <thread>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <vector>
#include <cctype>

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

static bool PathsEquivalent(const std::string& left, const std::string& right) {
    if (left.empty() || right.empty()) return false;
    try {
        return std::filesystem::equivalent(std::filesystem::path(left), std::filesystem::path(right));
    } catch (...) {
        return left == right;
    }
}

#ifdef _WIN32
static std::string ToLowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

static std::string NormalizePathForCompare(const std::filesystem::path& path) {
    std::error_code ec;
    auto absolute = std::filesystem::weakly_canonical(path, ec);
    if (ec) absolute = std::filesystem::absolute(path, ec);
    auto text = ec ? path.string() : absolute.string();
    std::replace(text.begin(), text.end(), '/', '\\');
    return ToLowerCopy(text);
}

static bool QueryProcessImagePath(DWORD pid, std::string& out) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;

    std::wstring buffer(32768, L'\0');
    DWORD size = static_cast<DWORD>(buffer.size());
    bool ok = QueryFullProcessImageNameW(process, 0, buffer.data(), &size);
    CloseHandle(process);
    if (!ok || size == 0) return false;

    buffer.resize(size);
    out = std::filesystem::path(buffer).string();
    return true;
}

static bool IsProcessAlive(DWORD pid) {
    if (pid == 0) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    DWORD exit_code = 0;
    bool alive = GetExitCodeProcess(process, &exit_code) && exit_code == STILL_ACTIVE;
    CloseHandle(process);
    return alive;
}

static DWORD GetTcpPortOwner(int port) {
    DWORD size = 0;
    DWORD status = GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0);
    if (status != ERROR_INSUFFICIENT_BUFFER || size == 0) return 0;

    std::vector<unsigned char> buffer(size);
    auto* table = reinterpret_cast<PMIB_TCPTABLE_OWNER_PID>(buffer.data());
    status = GetExtendedTcpTable(table, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0);
    if (status != NO_ERROR) return 0;

    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const auto& row = table->table[i];
        const int local_port_low = ntohs(static_cast<u_short>(row.dwLocalPort & 0xFFFF));
        const int local_port_high = ntohs(static_cast<u_short>((row.dwLocalPort >> 16) & 0xFFFF));
        if (local_port_low == port || local_port_high == port) return row.dwOwningPid;
    }
    return 0;
}

static bool TerminateAndWait(DWORD pid, std::chrono::milliseconds timeout) {
    HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return !IsProcessAlive(pid);

    DWORD exit_code = 0;
    if (GetExitCodeProcess(process, &exit_code) && exit_code == STILL_ACTIVE) {
        TerminateProcess(process, 0);
        WaitForSingleObject(process, static_cast<DWORD>(timeout.count()));
    }
    bool stopped = !GetExitCodeProcess(process, &exit_code) || exit_code != STILL_ACTIVE;
    CloseHandle(process);
    return stopped;
}

static std::vector<DWORD> FindRuntimeLlamaProcesses(const std::string& expected_exe_path) {
    std::vector<DWORD> pids;
    const auto expected = NormalizePathForCompare(expected_exe_path);

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return pids;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            std::string exe_name = std::filesystem::path(entry.szExeFile).string();
            if (ToLowerCopy(exe_name) != "llama-server.exe") continue;

            std::string image_path;
            if (!QueryProcessImagePath(entry.th32ProcessID, image_path)) continue;
            if (NormalizePathForCompare(image_path) == expected) {
                pids.push_back(entry.th32ProcessID);
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return pids;
}

static bool CleanupStaleRuntimeProcesses(const std::string& expected_exe_path, int port, DWORD tracked_pid) {
    bool ok = true;
    const auto runtime_pids = FindRuntimeLlamaProcesses(expected_exe_path);
    const DWORD port_owner = GetTcpPortOwner(port);

    if (port_owner != 0 && port_owner != tracked_pid &&
        std::find(runtime_pids.begin(), runtime_pids.end(), port_owner) == runtime_pids.end()) {
        Logger::Get().Error("llama-server port " + std::to_string(port) +
            " is owned by unknown PID " + std::to_string(port_owner) + "; refusing to steal it.");
        return false;
    }

    for (DWORD pid : runtime_pids) {
        if (pid == tracked_pid && IsProcessAlive(pid)) continue;
        Logger::Get().Warn("Stopping stale InferDeck llama-server process PID " + std::to_string(pid));
        if (!TerminateAndWait(pid, std::chrono::seconds(15))) {
            Logger::Get().Error("Failed to stop stale llama-server process PID " + std::to_string(pid));
            ok = false;
        }
    }

    const DWORD owner_after_cleanup = GetTcpPortOwner(port);
    if (owner_after_cleanup != 0 && owner_after_cleanup != tracked_pid) {
        Logger::Get().Error("llama-server port " + std::to_string(port) +
            " is still owned by PID " + std::to_string(owner_after_cleanup) + " after cleanup.");
        return false;
    }
    return ok;
}
#endif

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

    if (running_.load()) {
#ifdef _WIN32
        const DWORD port_owner = GetTcpPortOwner(port);
        if (!IsProcessAlive(pid_) || port_owner != pid_) {
            Logger::Get().Warn("Tracked llama-server PID is not healthy or does not own port " + std::to_string(port) + "; cleaning up before start.");
            running_.store(false);
            if (process_handle_) {
                CloseHandle(process_handle_);
                process_handle_ = nullptr;
            }
            pid_ = 0;
        } else
#endif
        if (PathsEquivalent(current_model_path_, model_path)) {
            Logger::Get().Info("llama-server already running requested model; reusing PID " + std::to_string(pid_));
            return true;
        } else {
            KillProcess();
            running_.store(false);
        }
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

#ifdef _WIN32
    if (!CleanupStaleRuntimeProcesses(GetExecutablePath(), port_, pid_)) {
        return false;
    }
#endif

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
    cmd += " --fit on";
    cmd += " --fit-target 512";
    cmd += " --parallel 1";
    cmd += " --kv-unified";
    cmd += " --timeout 1800";
    cmd += " --reasoning-format none";

    Logger::Get().Info("Launching llama-server command: " + cmd);
    Logger::Get().Info("Selected model path: " + model_path);
    Logger::Get().Info("Resolved Vulkan runtime directory: " + GetRuntimeDirectory());

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    std::filesystem::create_directories(std::filesystem::current_path() / "logs");
    std::wstring stdout_path = (std::filesystem::current_path() / "logs" / "llama-server.out.log").wstring();
    std::wstring stderr_path = (std::filesystem::current_path() / "logs" / "llama-server.err.log").wstring();
    HANDLE stdout_handle = CreateFileW(stdout_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE stderr_handle = CreateFileW(stderr_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (stdout_handle != INVALID_HANDLE_VALUE && stderr_handle != INVALID_HANDLE_VALUE) {
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdOutput = stdout_handle;
        si.hStdError = stderr_handle;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    }

    PROCESS_INFORMATION pi = {};

    std::wstring cmd_w(cmd.begin(), cmd.end());
    std::wstring exe_dir_w = std::filesystem::path(exe_path).parent_path().wstring();

    BOOL created = CreateProcessW(
        nullptr,
        &cmd_w[0],
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        exe_dir_w.c_str(),
        &si,
        &pi
    );
    if (stdout_handle != INVALID_HANDLE_VALUE) CloseHandle(stdout_handle);
    if (stderr_handle != INVALID_HANDLE_VALUE) CloseHandle(stderr_handle);

    if (!created) {
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
        DWORD exitCode = 0;
        GetExitCodeProcess(process_handle_, &exitCode);
        if (exitCode == STILL_ACTIVE) {
            TerminateProcess(process_handle_, 0);
            WaitForSingleObject(process_handle_, 15000);
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
            HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), static_cast<INTERNET_PORT>(port_), 0);
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

                        const DWORD port_owner = GetTcpPortOwner(port_);
                        if (statusCode == 200 && port_owner == pid_) {
                            Logger::Get().Info("llama-server is ready!");
                            return true;
                        }
                        if (statusCode == 200 && port_owner != pid_) {
                            Logger::Get().Warn("llama-server health check responded, but port " + std::to_string(port_) +
                                " is owned by PID " + std::to_string(port_owner) +
                                " instead of tracked PID " + std::to_string(pid_));
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
        if (PathsEquivalent(current_model_path_, new_model_path)) {
            Logger::Get().Info("Requested model already loaded; skipping restart for: " + new_model_path);
            return true;
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

#ifdef _WIN32
    if (!CleanupStaleRuntimeProcesses(GetExecutablePath(), port_, 0)) {
        return false;
    }
#endif

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
