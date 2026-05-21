#pragma once

#include <string>
#include <memory>
#include <atomic>
#include <mutex>

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
    bool DownloadIfNeeded() const;

private:
    LlamaServerManager();
    ~LlamaServerManager();
    LlamaServerManager(const LlamaServerManager&) = delete;
    LlamaServerManager& operator=(const LlamaServerManager&) = delete;

    bool DownloadBinary() const;
    bool LaunchProcess(const std::string& model_path, int gpu_layers, int context_size, int port);
    bool KillProcess();

    void* process_handle_ = nullptr;
    std::atomic<bool> running_{false};
    int port_ = 18080;
    mutable std::mutex mutex_;
};

} // namespace inferdeck::core
