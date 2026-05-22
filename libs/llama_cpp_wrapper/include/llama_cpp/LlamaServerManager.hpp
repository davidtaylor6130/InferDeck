#pragma once

#include <string>
#include <memory>
#include <atomic>
#include <mutex>
#include <cstdint>

namespace inferdeck::core {

class LlamaServerManager {
public:
    static LlamaServerManager& Get();

    bool Start(const std::string& model_path, int gpu_layers = -1, int context_size = 100000, int port = 18080);
    void Stop();
    bool IsRunning() const;
    int GetPort() const;
    bool WaitForReady(int timeout_seconds = 120) const;
    bool Restart(const std::string& new_model_path, int gpu_layers = -1, int context_size = 100000);

    std::string GetExecutablePath() const;
    std::string GetRuntimeDirectory() const;
    std::string GetCurrentModelPath() const;
    uint32_t GetPid() const;

private:
    LlamaServerManager();
    ~LlamaServerManager();
    LlamaServerManager(const LlamaServerManager&) = delete;
    LlamaServerManager& operator=(const LlamaServerManager&) = delete;

    bool DownloadIfNeeded() const;
    bool DownloadBinary() const;
    bool LaunchProcess(const std::string& model_path, int gpu_layers, int context_size, int port);
    bool KillProcess();

    void* process_handle_ = nullptr;
    std::atomic<bool> running_{false};
    int port_ = 18080;
    uint32_t pid_ = 0;
    std::string current_model_path_;
    mutable std::mutex mutex_;
};

} // namespace inferdeck::core
