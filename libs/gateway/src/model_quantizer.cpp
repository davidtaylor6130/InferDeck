#include "gateway/model_store.hpp"

#include <nlohmann/json.hpp>

#include "model/provider.hpp"

namespace inferdeck::gateway {

namespace {

std::string path_to_utf8(const std::filesystem::path& path) {
    const std::u8string value = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

class ProviderModelQuantizer final : public IModelQuantizer {
public:
    foundation::Result<void> quantize(
        const std::filesystem::path& source,
        const std::filesystem::path& destination,
        const std::string& quantization,
        int threads) override {
        const nlohmann::json request = {
            {"source", path_to_utf8(source)}, {"destination", path_to_utf8(destination)},
            {"quantization", quantization}, {"threads", threads}};
        const auto result = model::invoke_provider_operation("llama_cpp", "quantize", request.dump());
        if (!result) {
            return foundation::Err<void>(result.error().code, result.error().message);
        }
        return foundation::Ok();
    }
};

}

std::unique_ptr<IModelQuantizer> make_native_model_quantizer() {
    return std::make_unique<ProviderModelQuantizer>();
}

}
