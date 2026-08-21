    server.Get(R"(^/api/inferdeck/v1/config$)", wrap([deps](const httplib::Request& req,
                                               httplib::Response& resp) {
        (void)req;
        if (!deps.config_repository) {
            write_error(resp, 503, "config_unavailable", "configuration path is unavailable");
            return;
        }
        const auto snapshot = deps.config_repository->snapshot();
        if (!snapshot) {
            write_error(resp, 500, "config_read_failed", snapshot.error().message);
            return;
        }
        const std::string& desired = snapshot->has_active ? snapshot->active : snapshot->base;
        write_json(resp, 200, {
            {"yaml", mask_config_secrets(snapshot->base)},
            {"revision", snapshot->base_revision},
            {"activeYaml", mask_config_secrets(desired)},
            {"activeRevision", snapshot->active_revision},
            {"runningRevision", deps.running_config_revision},
            {"hasActiveProfile", snapshot->has_active},
            {"usingActiveProfile", deps.using_active_config},
            {"fallbackReason", deps.config_fallback_reason},
            {"restartRequired", deps.running_config_revision != snapshot->active_revision},
            {"secretSentinel", "__INFERDECK_SECRET__"},
        });
    }));
    server.Put(R"(^/api/inferdeck/v1/config$)", wrap([deps](const httplib::Request& req,
                                               httplib::Response& resp) {
        if (maintenance_mode_active(deps.gw)) {
            write_error(resp, 503, "maintenance_mode",
                        "configuration cannot change during a measured benchmark");
            return;
        }
        if (!deps.config_repository) {
            write_error(resp, 503, "config_unavailable", "configuration repository is unavailable");
            return;
        }
        nlohmann::json body;
        try {
            body = nlohmann::json::parse(req.body);
        } catch (const std::exception& error) {
            write_error(resp, 400, "invalid_json", error.what());
            return;
        }
        if (!body.contains("yaml") || !body["yaml"].is_string() ||
            !body.contains("revision") || !body["revision"].is_string()) {
            write_error(resp, 400, "invalid_config_update", "yaml and revision are required");
            return;
        }
        std::string submitted = body["yaml"].get<std::string>();
        if (submitted.empty() || submitted.size() > 2 * 1024 * 1024) {
            write_error(resp, 400, "invalid_config_update", "configuration size is invalid");
            return;
        }
        const auto current = deps.config_repository->snapshot();
        if (!current) {
            write_error(resp, 500, "config_read_failed", current.error().message);
            return;
        }
        auto written = deps.config_repository->write_base(
            body["revision"].get<std::string>(), submitted);
        if (!written) {
            const int status = written.error().code == foundation::ErrorCode::AlreadyExists
                ? 409 : written.error().code == foundation::ErrorCode::InvalidArgument ? 400 : 500;
            write_error(resp, status,
                        status == 409 ? "config_conflict" :
                        status == 400 ? "invalid_configuration" : "config_write_failed",
                        written.error().message);
            return;
        }
        write_json(resp, 200, {
            {"ok", true},
            {"revision", written->revision},
            {"restartRequired", true},
        });
    }));

    server.Put(R"(^/api/inferdeck/v1/config/active$)", wrap([deps](const httplib::Request& req,
                                                      httplib::Response& resp) {
        if (maintenance_mode_active(deps.gw)) {
            write_error(resp, 503, "maintenance_mode",
                        "configuration cannot change during a measured benchmark");
            return;
        }
        if (!deps.config_repository) {
            write_error(resp, 503, "config_unavailable", "configuration paths are unavailable");
            return;
        }
        nlohmann::json body;
        try {
            body = nlohmann::json::parse(req.body);
        } catch (const std::exception& error) {
            write_error(resp, 400, "invalid_json", error.what());
            return;
        }
        if (!body.contains("yaml") || !body["yaml"].is_string() ||
            !body.contains("revision") || !body["revision"].is_string()) {
            write_error(resp, 400, "invalid_config_update", "yaml and revision are required");
            return;
        }
        std::string submitted = body["yaml"].get<std::string>();
        if (submitted.empty() || submitted.size() > 2 * 1024 * 1024) {
            write_error(resp, 400, "invalid_config_update", "configuration size is invalid");
            return;
        }
        const auto current = deps.config_repository->snapshot();
        if (!current) {
            write_error(resp, 500, "config_read_failed", current.error().message);
            return;
        }
        auto written = deps.config_repository->write_active(
            body["revision"].get<std::string>(), submitted);
        if (!written) {
            const int status = written.error().code == foundation::ErrorCode::AlreadyExists
                ? 409 : written.error().code == foundation::ErrorCode::InvalidArgument ? 400 :
                  written.error().code == foundation::ErrorCode::Unavailable ? 503 : 500;
            write_error(resp, status,
                        status == 409 ? "config_conflict" :
                        status == 400 ? "invalid_configuration" :
                        status == 503 ? "config_reload_failed" : "config_write_failed",
                        written.error().message);
            return;
        }
        write_json(resp, 200, {
            {"ok", true},
            {"activeRevision", written->revision},
            {"hasActiveProfile", true},
            {"restartRequired", false},
            {"applyScheduled", true},
        });
    }));

    server.Delete(R"(^/api/inferdeck/v1/config/active$)", wrap([deps](const httplib::Request& req,
                                                         httplib::Response& resp) {
        if (maintenance_mode_active(deps.gw)) {
            write_error(resp, 503, "maintenance_mode",
                        "configuration cannot change during a measured benchmark");
            return;
        }
        if (!deps.config_repository) {
            write_error(resp, 503, "config_unavailable", "configuration repository is unavailable");
            return;
        }
        if (!req.has_header("If-Match") || req.get_header_value("If-Match").empty()) {
            write_error(resp, 400, "invalid_config_reset", "If-Match active revision is required");
            return;
        }
        auto reset = deps.config_repository->reset_active(req.get_header_value("If-Match"));
        if (!reset) {
            const int status = reset.error().code == foundation::ErrorCode::AlreadyExists
                ? 409 : reset.error().code == foundation::ErrorCode::Unavailable ? 503 : 500;
            write_error(resp, status,
                        status == 409 ? "config_conflict" :
                        status == 503 ? "config_reload_failed" : "config_reset_failed",
                        reset.error().message);
            return;
        }
        write_json(resp, 200, {
            {"ok", true},
            {"removed", reset->changed},
            {"activeRevision", reset->revision},
            {"hasActiveProfile", false},
            {"restartRequired", false},
            {"applyScheduled", reset->changed},
        });
    }));
