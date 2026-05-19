/// @file BackendRegistry.cpp
/// @brief BackendRegistry singleton implementation.

#include "backends/BackendRegistry.hpp"
#include <spdlog/spdlog.h>

namespace inferdeck::backends {

BackendRegistry& BackendRegistry::Instance() {
    static BackendRegistry instance;
    return instance;
}

BackendRegistry::BackendRegistry() = default;

std::shared_ptr<ITextBackend> BackendRegistry::GetTextBackend(const std::string& name) {
    return Get<ITextBackend>(name);
}

std::shared_ptr<ISttBackend> BackendRegistry::GetSttBackend(const std::string& name) {
    return Get<ISttBackend>(name);
}

std::shared_ptr<ITtsBackend> BackendRegistry::GetTtsBackend(const std::string& name) {
    return Get<ITtsBackend>(name);
}

std::shared_ptr<IImageBackend> BackendRegistry::GetImageBackend(const std::string& name) {
    return Get<IImageBackend>(name);
}

std::shared_ptr<IEmbeddingBackend> BackendRegistry::GetEmbeddingBackend(const std::string& name) {
    return Get<IEmbeddingBackend>(name);
}

std::shared_ptr<ITrainingBackend> BackendRegistry::GetTrainingBackend(const std::string& name) {
    return Get<ITrainingBackend>(name);
}

bool BackendRegistry::Has(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return backends_.find(name) != backends_.end();
}

bool BackendRegistry::InitializeAll() {
    std::lock_guard<std::mutex> lock(mutex_);

    // Sort by priority descending (highest priority first)
    std::vector<std::pair<std::string, std::shared_ptr<ITextBackend>>> sorted;
    for (const auto& [name, backend] : backends_) {
        sorted.push_back({name, backend});
    }
    std::sort(sorted.begin(), sorted.end(),
              [this](const auto& a, const auto& b) {
                  auto pa = priorities_.count(a.first) ? priorities_.at(a.first) : VRamPriority::TEXT_GENERATION;
                  auto pb = priorities_.count(b.first) ? priorities_.at(b.first) : VRamPriority::TEXT_GENERATION;
                  return static_cast<int>(pa) > static_cast<int>(pb);
              });

    for (const auto& [name, backend] : sorted) {
        spdlog::info("BackendRegistry: initializing '{}'...", name);
        if (!backend->Initialize()) {
            spdlog::error("BackendRegistry: failed to initialize '{}'", name);
            return false;
        }
        spdlog::info("BackendRegistry: '{}' initialized successfully", name);
    }
    return true;
}

void BackendRegistry::ShutdownAll() {
    std::lock_guard<std::mutex> lock(mutex_);

    // Shutdown in reverse priority order (lowest first)
    std::vector<std::pair<std::string, std::shared_ptr<ITextBackend>>> sorted;
    for (const auto& [name, backend] : backends_) {
        sorted.push_back({name, backend});
    }
    std::sort(sorted.begin(), sorted.end(),
              [this](const auto& a, const auto& b) {
                  auto pa = priorities_.count(a.first) ? priorities_.at(a.first) : VRamPriority::TEXT_GENERATION;
                  auto pb = priorities_.count(b.first) ? priorities_.at(b.first) : VRamPriority::TEXT_GENERATION;
                  return static_cast<int>(pa) < static_cast<int>(pb);
              });

    for (const auto& [name, backend] : sorted) {
        spdlog::info("BackendRegistry: shutting down '{}'...", name);
        backend->Shutdown();
        spdlog::info("BackendRegistry: '{}' shut down", name);
    }
}

std::vector<std::string> BackendRegistry::GetBackendNames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> names;
    names.reserve(backends_.size());
    for (const auto& [name, _] : backends_) {
        names.push_back(name);
    }
    return names;
}

nlohmann::json BackendRegistry::GetRegistryInfo() const {
    std::lock_guard<std::mutex> lock(mutex_);
    nlohmann::json info;
    info["backend_count"] = static_cast<int>(backends_.size());
    info["backends"] = nlohmann::json::array();

    for (const auto& [name, backend] : backends_) {
        nlohmann::json b;
        b["name"] = name;
        b["status"] = static_cast<int>(backend->GetStatus());
        b["ready"] = backend->IsReady();
        b["priority"] = priorities_.count(name) ?
            static_cast<int>(priorities_.at(name)) : static_cast<int>(VRamPriority::TEXT_GENERATION);
        info["backends"].push_back(b);
    }
    return info;
}

nlohmann::json BackendRegistry::GetBackendStatusSummary() const {
    std::lock_guard<std::mutex> lock(mutex_);
    nlohmann::json summary;
    summary["total"] = static_cast<int>(backends_.size());
    summary["ready"] = 0;
    summary["busy"] = 0;
    summary["error"] = 0;
    summary["uninitialized"] = 0;

    for (const auto& [name, backend] : backends_) {
        switch (backend->GetStatus()) {
            case BackendStatus::READY: summary["ready"] = summary["ready"].get<int>() + 1; break;
            case BackendStatus::BUSY: summary["busy"] = summary["busy"].get<int>() + 1; break;
            case BackendStatus::ERROR: summary["error"] = summary["error"].get<int>() + 1; break;
            default: summary["uninitialized"] = summary["uninitialized"].get<int>() + 1; break;
        }
    }
    return summary;
}

} // namespace inferdeck::backends
