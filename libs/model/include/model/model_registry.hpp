#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "model/imodel.hpp"

namespace inferdeck::model {

class ModelRegistry {
public:
    using ModelFactory = std::function<std::unique_ptr<IModel>(const ModelInfo&)>;

    ModelRegistry() = default;
    ModelRegistry(const ModelRegistry&) = delete;
    ModelRegistry& operator=(const ModelRegistry&) = delete;
    ModelRegistry(ModelRegistry&& other) noexcept;
    ModelRegistry& operator=(ModelRegistry&& other) noexcept;

    void set_factory(ModelFactory factory);
    void register_model(ModelInfo info);
    void unregister_model(const std::string& name);

    [[nodiscard]] bool has(const std::string& name) const;
    [[nodiscard]] const ModelInfo& get_info(const std::string& name) const;
    [[nodiscard]] std::vector<std::string> list() const;
    [[nodiscard]] std::size_t size() const;

    [[nodiscard]] std::unique_ptr<IModel> create(const std::string& name) const;

    [[nodiscard]] foundation::Result<ModelInfo> get_info_result(const std::string& name) const;

private:
    mutable std::mutex mutex_;
    ModelFactory factory_;
    std::unordered_map<std::string, ModelInfo> entries_;
};

} // namespace inferdeck::model
