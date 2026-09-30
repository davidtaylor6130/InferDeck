    server.Get(R"(^/api/inferdeck/v1/api-settings$)",
               wrap([deps](const httplib::Request& req,
                           httplib::Response& resp) {
        (void)req;
        if (!deps.config_repository) {
            write_error(resp, 503, "config_unavailable",
                        "configuration repository is unavailable");
            return;
        }
        const auto snapshot = deps.config_repository->snapshot();
        if (!snapshot) {
            write_error(resp, 500, "config_read_failed",
                        snapshot.error().message);
            return;
        }
        try {
            const std::string& desired =
                snapshot->has_active ? snapshot->active : snapshot->base;
            const auto root = YAML::Load(desired);
            const bool authentication_required =
                root["auth"] && root["auth"]["required"] &&
                root["auth"]["required"].as<bool>();
            write_json(resp, 200, {
                {"allowPublicTraffic", !authentication_required},
                {"runningAllowPublicTraffic",
                 deps.gw.public_data_plane_access},
                {"publicPriority", public_request_priority},
                {"activeRevision", snapshot->active_revision},
                {"restartRequired",
                 deps.running_config_revision !=
                     snapshot->active_revision},
            });
        } catch (const std::exception& error) {
            write_error(resp, 500, "config_read_failed", error.what());
        }
    }));

    server.Put(R"(^/api/inferdeck/v1/api-settings$)",
               wrap([deps](const httplib::Request& req,
                           httplib::Response& resp) {
        if (maintenance_mode_active(deps.gw)) {
            write_error(resp, 503, "maintenance_mode",
                        "API settings cannot change during maintenance work");
            return;
        }
        if (!deps.config_repository) {
            write_error(resp, 503, "config_unavailable",
                        "configuration repository is unavailable");
            return;
        }
        const auto body = nlohmann::json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.is_object() ||
            body.size() != 2 ||
            !body.contains("allowPublicTraffic") ||
            !body["allowPublicTraffic"].is_boolean() ||
            !body.contains("revision") ||
            !body["revision"].is_string()) {
            write_error(
                resp, 400, "invalid_api_settings",
                "allowPublicTraffic and revision are required");
            return;
        }
        const bool allow_public_traffic =
            body["allowPublicTraffic"].get<bool>();
        const auto written = deps.config_repository->transact_active(
            body["revision"].get<std::string>(),
            [allow_public_traffic](
                const ConfigSnapshot& snapshot)
                -> foundation::Result<std::string> {
                const std::string& source =
                    snapshot.has_active ? snapshot.active : snapshot.base;
                return render_public_data_plane_access(
                    source, allow_public_traffic);
            },
            {});
        if (!written) {
            const int status =
                written.error().code ==
                    foundation::ErrorCode::AlreadyExists
                    ? 409
                    : written.error().code ==
                              foundation::ErrorCode::InvalidArgument ||
                          written.error().code ==
                              foundation::ErrorCode::ParseError
                    ? 400
                    : written.error().code ==
                              foundation::ErrorCode::Unavailable
                    ? 503
                    : 500;
            write_error(
                resp, status,
                status == 409 ? "config_conflict" :
                status == 400 ? "invalid_api_settings" :
                status == 503 ? "config_reload_failed" :
                                "config_write_failed",
                written.error().message);
            return;
        }
        write_json(resp, 200, {
            {"ok", true},
            {"allowPublicTraffic", allow_public_traffic},
            {"runningAllowPublicTraffic",
             deps.gw.public_data_plane_access},
            {"publicPriority", public_request_priority},
            {"activeRevision", written->revision},
            {"restartRequired", false},
            {"applyScheduled", written->changed},
        });
    }));
