// The llama.cpp runtime (text + embeddings, vision, reasoning, MTP) as a v2
// provider DLL. Built only where ggml is static, so the DLL carries a private
// llama.cpp/ggml that upgrades independently of the gateway's. See
// docs/providers.md, "Upgrading a runtime as a DLL".
#include <fstream>
#include <iterator>

#include "llama.h"
#include "llama_cpp_wrapper/llama_cpp_model.hpp"
#include "provider/sdk.hpp"

namespace {

using inferdeck::llama_wrapper::LlamaCppConfig;
using nlohmann::json;

/// <summary>Mirrors the gateway's make_llama_config: model settings first, then gateway defaults.</summary>
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
    config.flash_attn = gateway.value("flash_attn", config.flash_attn);
    config.kv_offload = gateway.value("kv_offload", config.kv_offload);
    config.op_offload = gateway.value("op_offload", config.op_offload);
    config.cache_type_k = info.cache_type_k.empty() ? gateway.value("cache_type_k", config.cache_type_k) : info.cache_type_k;
    config.cache_type_v = info.cache_type_v.empty() ? gateway.value("cache_type_v", config.cache_type_v) : info.cache_type_v;
    config.mtp_enabled = info.mtp_enabled;
    config.mtp_draft_tokens = info.mtp_draft_tokens;
    config.mtp_p_min = info.mtp_p_min;
    config.mtp_max_active_requests = info.mtp_max_active_requests;
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
            [] { inferdeck::llama_wrapper::LlamaCppModel::shutdown_backend(); }};
}

}

INFERDECK_PROVIDER_SDK(define)
