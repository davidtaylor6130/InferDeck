#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <optional>

#include "ggml-backend.h"
#include "llama.h"
#include "llama_cpp_wrapper/llama_cpp_model.hpp"
#include "provider/sdk.hpp"

namespace {

using inferdeck::llama_wrapper::LlamaCppConfig;
using nlohmann::json;

LlamaCppConfig config_for(const inferdeck::model::ModelInfo& info, const json& gateway)
{
    LlamaCppConfig config;
    config.kv_unified = info.kv_unified;
    config.vram_safety_margin_mb = gateway.value("vram_safety_margin_mb", config.vram_safety_margin_mb);
    config.n_batch = info.n_batch.value_or(gateway.value("n_batch", config.n_batch));
    config.n_ubatch = info.n_ubatch.value_or(gateway.value("n_ubatch", config.n_ubatch));
    config.use_mmap = gateway.value("use_mmap", config.use_mmap);
    config.use_mlock = gateway.value("use_mlock", config.use_mlock);
    config.n_gpu_layers = info.n_gpu_layers;
    if (!config.n_gpu_layers && gateway.contains("n_gpu_layers"))
    {
        config.n_gpu_layers = gateway["n_gpu_layers"].get<int>();
    }
    const auto flash_attn = info.artifacts.find("flash_attn");
    config.flash_attn = flash_attn != info.artifacts.end() ? flash_attn->second
                                                         : gateway.value("flash_attn", config.flash_attn);
    config.kv_offload = gateway.value("kv_offload", config.kv_offload);
    config.op_offload = gateway.value("op_offload", config.op_offload);
    config.cache_type_k = info.cache_type_k.empty() ? gateway.value("cache_type_k", config.cache_type_k) : info.cache_type_k;
    config.cache_type_v = info.cache_type_v.empty() ? gateway.value("cache_type_v", config.cache_type_v) : info.cache_type_v;
    config.mtp_enabled = info.mtp_enabled;
    config.mtp_draft_tokens = info.mtp_draft_tokens;
    config.mtp_p_min = info.mtp_p_min;
    config.mtp_max_active_requests = info.mtp_max_active_requests;
    config.speculative_type = info.speculative_type;
    config.draft_model_path = info.draft_model_path;
    config.swa_full = gateway.value("swa_full", config.swa_full);
    config.truncate_prompt = gateway.value("truncate_prompt", config.truncate_prompt);
    config.reasoning_format = info.reasoning_format.empty() ? "auto" : info.reasoning_format;
    config.sampling = info.sampling;
    if (!info.chat_template_path.empty())
    {
        std::ifstream file(info.chat_template_path, std::ios::binary);
        config.chat_template.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
    return config;
}

std::optional<llama_ftype> quantization_ftype(std::string quantization)
{
    std::transform(quantization.begin(), quantization.end(), quantization.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    if (quantization == "q4_k_m") return LLAMA_FTYPE_MOSTLY_Q4_K_M;
    if (quantization == "q5_k_m") return LLAMA_FTYPE_MOSTLY_Q5_K_M;
    if (quantization == "q6_k") return LLAMA_FTYPE_MOSTLY_Q6_K;
    if (quantization == "q8_0") return LLAMA_FTYPE_MOSTLY_Q8_0;
    return std::nullopt;
}

const char* device_type_name(enum ggml_backend_dev_type type)
{
    switch (type)
    {
    case GGML_BACKEND_DEVICE_TYPE_CPU: return "cpu";
    case GGML_BACKEND_DEVICE_TYPE_GPU: return "gpu";
    case GGML_BACKEND_DEVICE_TYPE_IGPU: return "igpu";
    case GGML_BACKEND_DEVICE_TYPE_ACCEL: return "accel";
    default: return "unknown";
    }
}

inferdeck::foundation::Result<json> operation(const std::string& name, const json& request)
{
    using inferdeck::foundation::ErrorCode;
    if (name == "devices")
    {
        json devices = json::array();
        for (size_t index = 0; index < ggml_backend_dev_count(); ++index)
        {
            ggml_backend_dev_t device = ggml_backend_dev_get(index);
            if (!device) continue;
            const char* device_name = ggml_backend_dev_name(device);
            const char* description = ggml_backend_dev_description(device);
            devices.push_back({{"index", index}, {"name", device_name ? device_name : "unknown"},
                               {"description", description ? description : ""},
                               {"type", device_type_name(ggml_backend_dev_type(device))}});
        }
        return json{{"devices", std::move(devices)}};
    }
    if (name != "quantize")
    {
        return inferdeck::foundation::Err<json>(ErrorCode::Unavailable, "llama_cpp has no operation " + name);
    }
    const std::optional<llama_ftype> ftype = quantization_ftype(request.value("quantization", ""));
    if (!ftype)
    {
        return inferdeck::foundation::Err<json>(ErrorCode::InvalidArgument, "unsupported quantization type");
    }
    llama_model_quantize_params params = llama_model_quantize_default_params();
    params.ftype = *ftype;
    params.nthread = request.value("threads", 0);
    params.allow_requantize = false;
    params.keep_split = false;
    params.dry_run = false;
    const std::string source = request.value("source", "");
    const std::string destination = request.value("destination", "");
    if (llama_model_quantize(source.c_str(), destination.c_str(), &params) != 0)
    {
        return inferdeck::foundation::Err<json>(ErrorCode::InvalidArgument,
                                                "llama.cpp rejected the source model or quantization request");
    }
    return json::object();
}

inferdeck::provider::sdk::Definition define()
{
    return {inferdeck::provider::sdk::contract_manifest("llama_cpp", INFERDECK_PROVIDER_VERSION),
            [](const inferdeck::model::ModelInfo& info, const json& gateway)
            {
                return std::make_unique<inferdeck::llama_wrapper::LlamaCppModel>(info, config_for(info, gateway));
            },
            [](const json&) -> inferdeck::foundation::Result<void>
            {
                inferdeck::llama_wrapper::LlamaCppModel::init_backend();
                return inferdeck::foundation::Ok();
            },
            [] { inferdeck::llama_wrapper::LlamaCppModel::shutdown_backend(); },
            operation};
}

}

INFERDECK_PROVIDER_SDK(define)
