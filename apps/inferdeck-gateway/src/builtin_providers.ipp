/// <summary>
/// Describes every runtime compiled into the gateway. This is the only place
/// the gateway names a built-in provider type; everything downstream sees
/// <see cref="model::ProviderDescriptor"/> and the backend interfaces.
/// Plugin providers (docs/providers.md) are appended after these by the caller.
/// </summary>
std::vector<model::ProviderDescriptor> builtin_providers(const inferdeck::gateway::GatewayConfig& cfg)
{
    const model::RuntimeContractRegistry contracts = model::standard_runtime_contracts();
    std::vector<model::ProviderDescriptor> providers;

    model::ProviderDescriptor llama{*contracts.find("llama_cpp")};
    llama.initialize = [](const std::vector<model::ModelInfo>&) -> foundation::Result<void>
    {
        static std::once_flag initialized;
        std::call_once(initialized, []
        {
            llama_backend_init();
            if (const char* system_info = llama_print_system_info())
            {
                foundation::LOG_INFO("llama_system_info", "{}", system_info);
            }
        });
        return foundation::Ok();
    };
    llama.factory = [cfg](const model::ModelInfo& info) -> std::unique_ptr<model::IBackend>
    {
        return std::make_unique<inferdeck::llama_wrapper::LlamaCppModel>(info, make_llama_config(cfg, info));
    };
    providers.push_back(std::move(llama));

    model::ProviderDescriptor radiance{*contracts.find("vllm_radiance")};
#ifdef INFERDECK_VLLM_RADIANCE_ENABLE
    radiance.initialize = [](const std::vector<model::ModelInfo>& models) -> foundation::Result<void>
    {
        const auto configured = std::find_if(models.begin(), models.end(), [](const model::ModelInfo& info)
        {
            return info.runtime == "vllm_radiance";
        });
        if (configured == models.end())
        {
            return foundation::Ok();
        }
        const auto python_root = configured->artifacts.find("python_root");
        if (python_root == configured->artifacts.end())
        {
            return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                         "model " + configured->name + " has no python_root artifact");
        }
        return inferdeck::vllm_radiance_wrapper::initialize_python_runtime(python_root->second);
    };
    radiance.factory = [](const model::ModelInfo& info) -> std::unique_ptr<model::IBackend>
    {
        return std::make_unique<inferdeck::vllm_radiance_wrapper::VllmRadianceModel>(info);
    };
#endif
    providers.push_back(std::move(radiance));

    for (model::ProviderDescriptor& native : inferdeck::native_runtimes::providers())
    {
        providers.push_back(std::move(native));
    }
    return providers;
}

/// <summary>
/// Registers built-in providers, then plugin providers so a plugin can
/// replace a built-in runtime of the same name. Called once per gateway start.
/// </summary>
void install_gateway_providers(const inferdeck::gateway::GatewayConfig& cfg,
                               model::ModelRegistry& registry)
{
    for (const std::string& rejected : g_plugins.rejected)
    {
        foundation::LOG_ERROR("provider_plugin_rejected", "{}", rejected);
    }
    std::vector<model::ProviderDescriptor> providers = builtin_providers(cfg);
    providers.insert(providers.end(), g_plugins.providers.begin(), g_plugins.providers.end());
    model::install_providers(providers, registry, cfg.models);
}
