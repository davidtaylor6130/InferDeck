/// <summary>Provider status served by GET /api/inferdeck/v1/providers; rebuilt on every gateway start.</summary>
nlohmann::json g_provider_report = nlohmann::json::object();

/// <summary>
/// Installs the provider DLLs loaded at process start. The gateway links no
/// runtime: every runtime, for every modality, is a *.provider.dll (see
/// docs/providers.md). Standard runtimes without a DLL are reported as
/// "not_installed". Called once per gateway start.
/// </summary>
void install_gateway_providers(const inferdeck::gateway::GatewayConfig& cfg,
                               model::ModelRegistry& registry)
{
    for (const std::string& rejected : g_plugins.rejected)
    {
        foundation::LOG_ERROR("provider_plugin_rejected", "{}", rejected);
    }
    nlohmann::json gateway = {
        {"n_batch", cfg.n_batch}, {"n_ubatch", cfg.n_ubatch}, {"use_mmap", cfg.use_mmap},
        {"use_mlock", cfg.use_mlock}, {"flash_attn", cfg.flash_attn}, {"kv_offload", cfg.kv_offload},
        {"op_offload", cfg.op_offload}, {"cache_type_k", cfg.cache_type_k}, {"cache_type_v", cfg.cache_type_v},
        {"swa_full", cfg.swa_full}, {"truncate_prompt", cfg.truncate_prompt},
        {"vram_safety_margin_mb", cfg.vram_safety_margin_mb}};
    if (cfg.n_gpu_layers)
    {
        gateway["n_gpu_layers"] = *cfg.n_gpu_layers;
    }
    inferdeck::provider::set_plugin_host_settings(gateway.dump());
    const std::vector<model::ProviderStatus> statuses =
        model::install_providers(g_plugins.providers, registry, cfg.models);
    g_runtime_factories.clear();
    nlohmann::json listed = nlohmann::json::array();
    const auto list = [&listed](const model::ProviderStatus& status)
    {
        nlohmann::json modalities = nlohmann::json::object();
        for (const auto& [modality, capabilities] : status.contract.modality_capabilities)
        {
            std::vector<std::string> sorted(capabilities.begin(), capabilities.end());
            std::sort(sorted.begin(), sorted.end());
            modalities[modality] = std::move(sorted);
        }
        listed.push_back({{"runtime", status.runtime}, {"origin", status.origin},
                          {"version", status.version}, {"state", status.state},
                          {"error", status.error}, {"modalities", std::move(modalities)}});
    };
    std::set<std::string> installed;
    for (std::size_t index = 0; index < statuses.size(); ++index)
    {
        list(statuses[index]);
        installed.insert(statuses[index].runtime);
        if (statuses[index].state == "active")
        {
            g_runtime_factories[statuses[index].runtime] = g_plugins.providers[index].factory;
        }
    }
    for (const std::string runtime : {"llama_cpp", "vllm_radiance", "jev_style", "stable_diffusion_cpp",
                                      "ace_step_cpp", "ltx_video_cpp", "whisper_cpp", "sherpa_onnx",
                                      "windows_sapi"})
    {
        const model::RuntimeContract* contract = model::runtime_contracts().find(runtime);
        if (!installed.contains(runtime) && contract)
        {
            list({runtime, "", "", "not_installed", runtime + ".provider.dll is not in the providers folder",
                  *contract});
        }
    }
    g_provider_report = {{"abi_version", 2}, {"abi_versions", {1, 2}}, {"providers", std::move(listed)},
                         {"rejected_plugins", g_plugins.rejected}};
}
