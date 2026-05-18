/// @file BackendRegistry.hpp
/// @brief Singleton managing all loaded backends with lifecycle control.

#pragma once

#include <string>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <vector>
#include <nlohmann/json.hpp>
#include "backends/ITextBackend.hpp"
#include "backends/ISttBackend.hpp"
#include "backends/ITtsBackend.hpp"
#include "backends/IImageBackend.hpp"
#include "backends/IEmbeddingBackend.hpp"
#include "backends/ITrainingBackend.hpp"
#include "backends/GpuResourceManager.hpp"

namespace inferdeck::backends {

class BackendRegistry {
public:
    static BackendRegistry& Instance();

    BackendRegistry(const BackendRegistry&) = delete;
    BackendRegistry& operator=(const BackendRegistry&) = delete;

    /// Register a backend instance.
    template<typename T>
    bool Register(const std::string& name, std::unique_ptr<T> backend,
                  VRamPriority priority = VRamPriority::TEXT_GENERATION);

    /// Get a backend by name (typed).
    template<typename T>
    std::shared_ptr<T> Get(const std::string& name);

    std::shared_ptr<ITextBackend> GetTextBackend(const std::string& name);
    std::shared_ptr<ISttBackend> GetSttBackend(const std::string& name);
    std::shared_ptr<ITtsBackend> GetTtsBackend(const std::string& name);
    std::shared_ptr<IImageBackend> GetImageBackend(const std::string& name);
    std::shared_ptr<IEmbeddingBackend> GetEmbeddingBackend(const std::string& name);
    std::shared_ptr<ITrainingBackend> GetTrainingBackend(const std::string& name);

    bool Has(const std::string& name) const;
    bool InitializeAll();
    void ShutdownAll();
    std::vector<std::string> GetBackendNames() const;
    nlohmann::json GetRegistryInfo() const;
    nlohmann::json GetBackendStatusSummary() const;

private:
    BackendRegistry();
    ~BackendRegistry() = default;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<ITextBackend>> backends_;
    std::unordered_map<std::string, VRamPriority> priorities_;
};

template<typename T>
bool BackendRegistry::Register(const std::string& name, std::unique_ptr<T> backend,
                               VRamPriority priority) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (backends_.find(name) != backends_.end()) {
        return false;
    }
    backends_[name] = std::move(backend);
    priorities_[name] = priority;
    return true;
}

template<typename T>
std::shared_ptr<T> BackendRegistry::Get(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = backends_.find(name);
    if (it == backends_.end()) {
        return nullptr;
    }
    auto typed = std::dynamic_pointer_cast<T>(it->second);
    return typed;
}

} // namespace inferdeck::backends
