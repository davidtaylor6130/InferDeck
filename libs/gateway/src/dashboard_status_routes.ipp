    server.Get(R"(^/api/inferdeck/v1/status$)", wrap([deps](const httplib::Request& req,
                                               httplib::Response& resp) {
        (void)req;
        resp.set_content(build_dashboard_status(deps).dump(), "application/json");
    }));
    server.Get(R"(^/api/inferdeck/v1/models$)", wrap([deps](const httplib::Request& req,
                                               httplib::Response& resp) {
        (void)req;
        write_json(resp, 200, build_dashboard_models(deps.gw.coordinator));
    }));

    server.Get(R"(^/api/inferdeck/v1/usage/daily$)",
               wrap([deps](const httplib::Request& req,
                           httplib::Response& resp) {
        (void)req;
        write_json(resp, 200, {
            {"dailyTokenUsage",
             usage_bucket_json(deps.gw.stats_db->daily_usage(0))},
            {"dailyTokenUsageAllTime", true},
        });
    }));

    server.Get(R"(^/api/inferdeck/v1/jobs$)", wrap([deps](const httplib::Request& req,
                                             httplib::Response& resp) {
        int limit = 100;
        if (req.has_param("limit")) {
            try { limit = std::clamp(std::stoi(req.get_param_value("limit")), 1, 500); } catch (...) {}
        }
        const auto protocol_profile = req.has_param("protocol_profile")
            ? req.get_param_value("protocol_profile") : std::string{};
        const auto endpoint = req.has_param("endpoint")
            ? req.get_param_value("endpoint") : std::string{};
        resp.set_content(build_dashboard_jobs(
            *deps.gw.stats_db, limit, protocol_profile, endpoint).dump(),
            "application/json");
    }));

    server.Post(R"(^/api/inferdeck/v1/models/load$)", wrap([deps](const httplib::Request& req,
                                                     httplib::Response& resp) {
        auto body = req.body.empty() ? nlohmann::json::object() : nlohmann::json::parse(req.body);
        const std::string model_name = body.value("model", body.value("name", ""));
        if (model_name.empty()) {
            write_error(resp, 400, "missing_model", "request body must include model");
            return;
        }
        auto started = start_swap_async(deps.gw, model_name);
        write_json(resp, started.status, started.body);
    }));

    server.Post(R"(^/api/inferdeck/v1/models/unload$)", wrap([deps](const httplib::Request& req,
                                                       httplib::Response& resp) {
        const auto body = req.body.empty() ? nlohmann::json::object() : nlohmann::json::parse(req.body);
        const auto current = deps.gw.coordinator.get_loaded_model();
        const std::string requested_model = body.value("model", current.value_or(""));
        const auto resolved = requested_model.empty()
            ? foundation::Ok(std::string{})
            : deps.gw.coordinator.registry().resolve(requested_model);
        if (!resolved) {
            write_error(resp, 404, "model_not_found", resolved.error().message);
            return;
        }
        const std::string& model_name = *resolved;
        if (!model_name.empty() && maintenance_blocks_model(deps.gw, model_name)) {
            write_error(resp, 503, "maintenance_mode",
                        "measured model optimization is using the same compute resource");
            return;
        }
        auto result = model_name.empty() ? foundation::Ok() : deps.gw.coordinator.unload(model_name);
        if (!result) {
            write_error(resp, 500, "unload_failed", result.error().message);
            return;
        }
        if (deps.gw.events && !model_name.empty()) {
            deps.gw.events->publish("model", nlohmann::json{
                {"state", "unloaded"},
                {"from", model_name},
                {"to", ""},
                {"durationMs", 0.0},
                {"error", ""},
            }.dump());
        }
        write_json(resp, 200, {{"ok", true}, {"status", "stopped"},
                               {"model", requested_model},
                               {"resolvedModel", model_name}});
    }));
