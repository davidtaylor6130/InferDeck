#include "model/model_registry.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace inferdeck::model {

ModelRegistry::ModelRegistry(ModelRegistry&& other) noexcept
    : factories_(std::move(other.factories_)),
      entries_(std::move(other.entries_)) {}

ModelRegistry& ModelRegistry::operator=(ModelRegistry&& other) noexcept {
    if (this != &other) {
        factories_ = std::move(other.factories_);
        entries_ = std::move(other.entries_);
    }
    return *this;
}

void ModelRegistry::set_factory(ModelFactory factory) {
    register_factory("llama_cpp", std::move(factory));
}

void ModelRegistry::register_factory(std::string runtime, BackendFactory factory) {
    if (runtime.empty() || !factory) {
        throw std::invalid_argument("ModelRegistry::register_factory: runtime or factory is empty");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    factories_[std::move(runtime)] = std::move(factory);
}

bool ModelRegistry::has_factory(const std::string& runtime) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return factories_.contains(runtime);
}

void ModelRegistry::register_model(ModelInfo info) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (info.name.empty()) {
        throw std::invalid_argument("ModelRegistry::register_model: name is empty");
    }
    entries_[info.name] = std::move(info);
}

void ModelRegistry::unregister_model(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(name);
}

bool ModelRegistry::has(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.find(name) != entries_.end();
}

const ModelInfo& ModelRegistry::get_info(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(name);
    if (it == entries_.end()) {
        throw std::out_of_range("ModelRegistry::get_info: not found: " + name);
    }
    return it->second;
}

foundation::Result<ModelInfo> ModelRegistry::get_info_result(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(name);
    if (it == entries_.end()) {
        return foundation::Err<ModelInfo>(foundation::ErrorCode::NotFound,
                                          "model not registered: " + name);
    }
    return foundation::Ok(it->second);
}

std::vector<std::string> ModelRegistry::list() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    out.reserve(entries_.size());
    for (const auto& [k, _] : entries_) out.push_back(k);
    std::sort(out.begin(), out.end());
    return out;
}

std::size_t ModelRegistry::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

std::unique_ptr<IBackend> ModelRegistry::create(const std::string& name) const {
    auto result = create_result(name);
    return result ? std::move(result.value()) : nullptr;
}

foundation::Result<std::unique_ptr<IBackend>> ModelRegistry::create_result(
    const std::string& name) const {
    ModelInfo info;
    BackendFactory factory;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto entry = entries_.find(name);
        if (entry == entries_.end()) {
            return foundation::Err<std::unique_ptr<IBackend>>(
                foundation::ErrorCode::NotFound, "model not registered: " + name);
        }
        info = entry->second;
        const auto selected = factories_.find(info.runtime);
        if (selected == factories_.end()) {
            return foundation::Err<std::unique_ptr<IBackend>>(
                foundation::ErrorCode::Unavailable,
                "runtime not registered: " + info.runtime);
        }
        factory = selected->second;
    }
    auto backend = factory(info);
    if (!backend) {
        return foundation::Err<std::unique_ptr<IBackend>>(
            foundation::ErrorCode::Internal,
            "runtime factory returned no backend: " + info.runtime);
    }
    return foundation::Ok(std::move(backend));
}

} // namespace inferdeck::model
