std::atomic<bool> g_gpu_lost_restart{false};
constexpr int gpu_lost_exit_code = 76;

std::atomic<long long> g_gpu_backoff_until{0};

long long unix_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

fs::path gpu_lost_stamp_path() { return executable_dir() / "gpu-lost-restarts.txt"; }

std::vector<long long> read_gpu_lost_stamps() {
    std::vector<long long> stamps;
    std::ifstream in(gpu_lost_stamp_path());
    for (long long value; in >> value;) stamps.push_back(value);
    return stamps;
}

void record_gpu_lost_restart() {
    auto stamps = read_gpu_lost_stamps();
    const auto now = unix_seconds();
    std::erase_if(stamps, [now](long long stamp) { return stamp > now + 5; });
    stamps.push_back(now);
    if (stamps.size() > 10) stamps.erase(stamps.begin(), stamps.end() - 10);
    std::ofstream out(gpu_lost_stamp_path(), std::ios::trunc);
    for (const auto stamp : stamps) out << stamp << std::endl;
    if (!out) foundation::LOG_WARN("gpu_lost_stamp_write_failed", "path={}", gpu_lost_stamp_path().string());
}

bool gpu_backoff_active(const model::BackendCoordinator& coordinator) {
    if (coordinator.get_loaded_model()) g_gpu_backoff_until.store(0);
    return g_gpu_backoff_until.load() > unix_seconds();
}

void configure_gpu_lost_backoff(inferdeck::gateway::GatewayDeps& deps) {
    static bool first_boot = true;
    if (const auto until = std::exchange(first_boot, false)
            ? inferdeck::gateway::gpu_backoff_until(read_gpu_lost_stamps(), unix_seconds())
            : std::nullopt) {
        g_gpu_backoff_until.store(*until);
        foundation::LOG_WARN("gpu_lost_backoff", "until_unix={}", *until);
    }
    deps.auto_load_blocked = [&coordinator = deps.coordinator] { return gpu_backoff_active(coordinator); };
}

nlohmann::json build_health_json(const model::BackendCoordinator& coordinator, bool db_healthy) {
    const auto unhealthy = coordinator.unhealthy_models();
    const auto degraded = coordinator.degraded_models();
    const bool backoff = gpu_backoff_active(coordinator);
    const bool ok = unhealthy.empty() && !backoff && degraded.empty();
    nlohmann::json body = {{"ok", ok},
                           {"status", ok ? "ok" : "degraded"},
                           {"unhealthy_models", unhealthy},
                           {"degraded_models", degraded},
                           {"db_healthy", db_healthy},
                           {"version", inferdeck::app::build_version},
                           {"build_revision", inferdeck::app::build_revision},
                           {"build_dirty", inferdeck::app::build_dirty}};
    if (!degraded.empty()) body["reason"] = "gpu_unavailable";
    if (backoff) body["reason"] = "gpu_lost_backoff";
    return body;
}

void install_gpu_lost_handler() {
    inferdeck::provider::set_fatal_device_handler([](const std::string& runtime, const std::string& message) {
        if (g_gpu_lost_restart.exchange(true)) return;
        foundation::LOG_FATAL("gpu_device_lost_restart", "runtime={} error={}", runtime, message);
        record_gpu_lost_restart();
        g_stop.store(true);
        if (auto* coordinator = g_coordinator.load()) coordinator->request_shutdown();
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
                    return coordinator.get_loaded_model().has_value() || coordinator.swap_in_progress() ||
                           gpu_backoff_active(coordinator);
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

