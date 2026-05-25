#include "llama_cpp/LlamaServerManager.hpp"
#include "core/Logger.hpp"

#include <chrono>
#include <filesystem>

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
    return "internal llama.cpp runtime";
}

std::string LlamaServerManager::GetRuntimeDirectory() const {
    return (std::filesystem::current_path() / "internal-llama-cpp-vulkan").string();
}

std::string LlamaServerManager::GetCurrentModelPath() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_model_path_;
}

uint32_t LlamaServerManager::GetPid() const {
    return 0;
}

bool LlamaServerManager::DownloadIfNeeded() const {
    Logger::Get().Info("llama.cpp runtime is linked in-process with Vulkan backend enabled");
    return true;
}

bool LlamaServerManager::DownloadBinary() const {
    Logger::Get().Info("No llama-server binary is required for the internal llama.cpp runtime");
    return true;
}

bool LlamaServerManager::Start(const std::string& model_path, int gpu_layers, int context_size, int port) {
    (void)gpu_layers;
    (void)context_size;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!std::filesystem::exists(model_path)) {
        Logger::Get().Error("Model file not found: " + model_path);
        return false;
    }
    port_ = port;
    current_model_path_ = model_path;
    running_.store(true);
    pid_ = 0;
    Logger::Get().Info("Internal llama.cpp runtime active for model: " + model_path);
    return true;
}

bool LlamaServerManager::LaunchProcess(const std::string& model_path, int gpu_layers, int context_size, int port) {
    return Start(model_path, gpu_layers, context_size, port);
}

void LlamaServerManager::Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_.load()) return;
    running_.store(false);
    current_model_path_.clear();
    Logger::Get().Info("Internal llama.cpp runtime marked stopped");
}

bool LlamaServerManager::KillProcess() {
    Stop();
    return true;
}

bool LlamaServerManager::IsRunning() const {
    return running_.load();
}

int LlamaServerManager::GetPort() const {
    return port_;
}

bool LlamaServerManager::WaitForReady(int timeout_seconds) const {
    (void)timeout_seconds;
    return running_.load();
}

bool LlamaServerManager::Restart(const std::string& new_model_path, int gpu_layers, int context_size) {
    return Start(new_model_path, gpu_layers, context_size, port_);
}

} // namespace inferdeck::core
