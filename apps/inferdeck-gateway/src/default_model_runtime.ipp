std::atomic<bool> g_gpu_lost_restart{false};
constexpr int gpu_lost_exit_code = 76;

void install_gpu_lost_handler() {
    inferdeck::provider::set_fatal_device_handler([](const std::string& runtime, const std::string& message) {
        foundation::LOG_FATAL("gpu_device_lost_restart", "runtime={} error={}", runtime, message);
        g_gpu_lost_restart.store(true);
        g_stop.store(true);
    });
}

std::thread start_default_model_loader(model::BackendCoordinator& coordinator,
                                       std::string default_model, int preload_delay_ms) {
    static bool first_boot = true;
    const int delay_ms = std::exchange(first_boot, false) ? preload_delay_ms : 0;
    return std::thread([&coordinator, default_model = std::move(default_model), delay_ms] {
        const auto aborted = [] { return g_stop.load() || g_reload.load(); };
        if (!inferdeck::gateway::wait_for_default_preload(
                std::chrono::milliseconds{delay_ms}, aborted,
                [&coordinator] {
                    return !coordinator.get_loaded_models().empty() || coordinator.swap_in_progress();
                })) {
            g_default_model_loading.store(false);
            return;
        }
        foundation::LOG_INFO("default_model_load_begin", "name={}", default_model);
        model::AcquireSlotOptions options;
        options.timeout = std::chrono::minutes{5};
        options.priority = -100;
        options.cancelled = aborted;
        options.prepare = [&coordinator, default_model, aborted] {
            return coordinator.swap_to_cancellable(default_model, std::chrono::minutes{5}, aborted);
        };
        auto slot = coordinator.acquire_slot(default_model, options);
        auto loaded = slot
            ? coordinator.release_slot(default_model, *slot)
            : foundation::Err<void>(slot.error().code, slot.error().message);
        if (loaded) {
            foundation::LOG_INFO("default_model_load_complete", "name={}", default_model);
        } else {
            foundation::LOG_ERROR("default_model_load_failed", "name={} error={}",
                                  default_model, loaded.error().message);
        }
        g_default_model_loading.store(false);
    });
}

