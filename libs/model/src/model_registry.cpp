#include "model/model_registry.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <utility>

namespace inferdeck::model {

ModelRegistry::ModelRegistry(ModelRegistry&& other) noexcept
    : factories_(std::move(other.factories_)),
      entries_(std::move(other.entries_)),
      aliases_(std::move(other.aliases_)) {}

ModelRegistry& ModelRegistry::operator=(ModelRegistry&& other) noexcept {
    if (this != &other) {
        factories_ = std::move(other.factories_);
        entries_ = std::move(other.entries_);
        aliases_ = std::move(other.aliases_);
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
    if (aliases_.contains(info.name)) {
        throw std::invalid_argument("ModelRegistry::register_model: name conflicts with alias: " + info.name);
    }
    entries_[info.name] = std::move(info);
}

void ModelRegistry::unregister_model(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [alias_name, alias] : aliases_) {
        if (alias.target == name) {
            throw std::invalid_argument("model is targeted by alias: " + alias_name);
        }
    }
    entries_.erase(name);
}

bool ModelRegistry::has(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.find(name) != entries_.end();
}

ModelInfo ModelRegistry::get_info(const std::string& name) const {
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

foundation::Result<ModelAlias> ModelRegistry::set_alias(ModelAlias alias) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (alias.name.empty() || alias.target.empty()) {
        return foundation::Err<ModelAlias>(foundation::ErrorCode::InvalidArgument,
                                           "alias name and target are required");
    }
    if (alias.name.size() > 128 ||
        !std::all_of(alias.name.begin(), alias.name.end(), [](unsigned char character) {
            return std::isalnum(character) || character == '-' ||
                   character == '_' || character == '.';
        }) || alias.required_context_size < 0 ||
        std::any_of(alias.required_capabilities.begin(),
                    alias.required_capabilities.end(),
                    [](const auto& capability) { return capability.empty(); })) {
        return foundation::Err<ModelAlias>(foundation::ErrorCode::InvalidArgument,
                                           "alias contract is invalid");
    }
    if (alias.name == alias.target || entries_.contains(alias.name)) {
        return foundation::Err<ModelAlias>(foundation::ErrorCode::AlreadyExists,
                                           "alias conflicts with a concrete model ID");
    }
    const auto target = entries_.find(alias.target);
    if (target == entries_.end()) {
        const auto reason = aliases_.contains(alias.target)
            ? "aliases cannot target other aliases"
            : "alias target is not a registered concrete model";
        return foundation::Err<ModelAlias>(foundation::ErrorCode::InvalidArgument, reason);
    }
    const auto existing = aliases_.find(alias.name);
    if (existing == aliases_.end()) {
        if (alias.required_context_size <= 0) {
            alias.required_context_size = target->second.context_size;
        }
        if (alias.required_capabilities.empty()) {
            alias.required_capabilities = target->second.capabilities;
        }
    } else {
        alias.required_context_size = existing->second.required_context_size;
        alias.required_capabilities = existing->second.required_capabilities;
    }
    std::vector<std::string> mismatches;
    if (target->second.context_size < alias.required_context_size) {
        mismatches.push_back("context_size requires at least " +
                             std::to_string(alias.required_context_size));
    }
    for (const auto& capability : alias.required_capabilities) {
        if (!target->second.supports(capability)) {
            mismatches.push_back("missing capability " + capability);
        }
    }
    if (!mismatches.empty()) {
        std::string message = "incompatible alias target: ";
        for (std::size_t i = 0; i < mismatches.size(); ++i) {
            if (i) message += ", ";
            message += mismatches[i];
        }
        return foundation::Err<ModelAlias>(foundation::ErrorCode::InvalidArgument,
                                           std::move(message));
    }
    aliases_[alias.name] = alias;
    return foundation::Ok(std::move(alias));
}

foundation::Result<void> ModelRegistry::remove_alias(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!aliases_.erase(name)) {
        return foundation::Err<void>(foundation::ErrorCode::NotFound,
                                     "model alias not found: " + name);
    }
    return foundation::Ok();
}

foundation::Result<std::string> ModelRegistry::resolve(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (entries_.contains(name)) return foundation::Ok(name);
    const auto alias = aliases_.find(name);
    if (alias != aliases_.end() && entries_.contains(alias->second.target)) {
        return foundation::Ok(alias->second.target);
    }
    return foundation::Err<std::string>(foundation::ErrorCode::NotFound,
                                        "model not registered: " + name);
}

std::vector<ModelAlias> ModelRegistry::aliases() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ModelAlias> result;
    result.reserve(aliases_.size());
    for (const auto& [_, alias] : aliases_) result.push_back(alias);
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.name < right.name;
    });
    return result;
}

bool ModelRegistry::has_alias(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return aliases_.contains(name);
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
