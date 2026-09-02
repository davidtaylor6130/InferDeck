#include "gateway/model_store.hpp"

#include <llama.h>

#include <algorithm>
#include <cctype>
#include <optional>

namespace inferdeck::gateway {

namespace {

std::optional<llama_ftype> quantization_ftype(std::string quantization) {
    std::transform(quantization.begin(), quantization.end(),
                   quantization.begin(), [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    if (quantization == "q4_k_m") return LLAMA_FTYPE_MOSTLY_Q4_K_M;
    if (quantization == "q5_k_m") return LLAMA_FTYPE_MOSTLY_Q5_K_M;
    if (quantization == "q6_k") return LLAMA_FTYPE_MOSTLY_Q6_K;
    if (quantization == "q8_0") return LLAMA_FTYPE_MOSTLY_Q8_0;
    return std::nullopt;
}

std::string path_to_utf8(const std::filesystem::path& path) {
    const std::u8string value = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

class NativeModelQuantizer final : public IModelQuantizer {
public:
    foundation::Result<void> quantize(
        const std::filesystem::path& source,
        const std::filesystem::path& destination,
        const std::string& quantization,
        int threads) override {
        const auto ftype = quantization_ftype(quantization);
        if (!ftype) {
            return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                         "unsupported quantization type");
        }
        llama_model_quantize_params params = llama_model_quantize_default_params();
        params.ftype = *ftype;
        params.nthread = threads;
        params.allow_requantize = false;
        params.keep_split = false;
        params.dry_run = false;
        const std::string source_utf8 = path_to_utf8(source);
        const std::string destination_utf8 = path_to_utf8(destination);
        if (llama_model_quantize(source_utf8.c_str(),
                                 destination_utf8.c_str(), &params) != 0) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "llama.cpp rejected the source model or quantization request");
        }
        return foundation::Ok();
    }
};

}

std::unique_ptr<IModelQuantizer> make_native_model_quantizer() {
    return std::make_unique<NativeModelQuantizer>();
}

}
