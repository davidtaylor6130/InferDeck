    server.Get(R"(^/api/inferdeck/v1/model-aliases$)", wrap([deps](const httplib::Request&,
                                                       httplib::Response& resp) {
        nlohmann::json aliases = nlohmann::json::array();
        for (const auto& alias : deps.gw.coordinator.registry().aliases()) {
            aliases.push_back(alias_json(alias));
        }
        if (!deps.config_repository) {
            write_error(resp, 503, "config_unavailable", "configuration repository is unavailable");
            return;
        }
        const auto snapshot = deps.config_repository->snapshot();
        if (!snapshot) {
            write_error(resp, 500, "config_read_failed", snapshot.error().message);
            return;
        }
        write_json(resp, 200, {
            {"aliases", std::move(aliases)},
            {"revision", snapshot->active_revision},
        });
    }));
    server.Put(R"(^/api/inferdeck/v1/model-aliases/([A-Za-z0-9_.-]+)$)",
               wrap([deps](const httplib::Request& req, httplib::Response& resp) {
        if (!deps.config_repository) {
            write_error(resp, 503, "config_unavailable", "configuration repository is unavailable");
            return;
        }
        const std::string name = req.matches[1].str();
        if (name.empty() || name.size() > 128) {
            write_error(resp, 400, "invalid_model_alias", "alias name is invalid");
            return;
        }
        try {
            const auto body = nlohmann::json::parse(req.body);
            if (!body.contains("revision") || !body["revision"].is_string()) {
                write_error(resp, 400, "invalid_model_alias", "revision is required");
                return;
            }
            model::ModelAlias requested;
            requested.name = name;
            requested.target = body.value("target", "");
            requested.required_context_size = body.value("requiredContextSize", 0);
            if (body.contains("requiredCapabilities")) {
                requested.required_capabilities =
                    body["requiredCapabilities"].get<std::vector<std::string>>();
            }
            std::optional<model::ModelAlias> previous;
            for (const auto& alias : deps.gw.coordinator.registry().aliases()) {
                if (alias.name == name) previous = alias;
            }
            std::optional<model::ModelAlias> committed;
            const auto persisted = deps.config_repository->transact_active(
                body["revision"].get<std::string>(),
                [&](const ConfigSnapshot& snapshot) -> foundation::Result<std::string> {
                    const auto applied = deps.gw.coordinator.registry().set_alias(requested);
                    if (!applied) return foundation::Err<std::string>(
                        applied.error().code, applied.error().message);
                    committed = *applied;
                    return render_aliases(
                        snapshot.has_active ? snapshot.active : snapshot.base,
                        deps.gw.coordinator.registry().aliases());
                },
                [&] {
                    if (previous) (void)deps.gw.coordinator.registry().set_alias(*previous);
                    else (void)deps.gw.coordinator.registry().remove_alias(name);
                });
            if (!persisted) {
                const int status = persisted.error().code == foundation::ErrorCode::AlreadyExists
                    ? 409 : persisted.error().code == foundation::ErrorCode::InvalidArgument ? 400 : 500;
                write_error(resp, status,
                            status == 409 ? "config_conflict" : "model_alias_persist_failed",
                            persisted.error().message);
                return;
            }
            auto response = alias_json(*committed);
            response["revision"] = persisted->revision;
            write_json(resp, previous ? 200 : 201, response);
        } catch (const std::exception& error) {
            write_error(resp, 400, "invalid_model_alias", error.what());
        }
    }));

    server.Delete(R"(^/api/inferdeck/v1/model-aliases/([A-Za-z0-9_.-]+)$)",
                  wrap([deps](const httplib::Request& req, httplib::Response& resp) {
        if (!deps.config_repository) {
            write_error(resp, 503, "config_unavailable", "configuration repository is unavailable");
            return;
        }
        if (!req.has_header("If-Match") || req.get_header_value("If-Match").empty()) {
            write_error(resp, 400, "invalid_model_alias", "If-Match revision is required");
            return;
        }
        const std::string name = req.matches[1].str();
        std::optional<model::ModelAlias> previous;
        for (const auto& alias : deps.gw.coordinator.registry().aliases()) {
            if (alias.name == name) previous = alias;
        }
        if (!previous) {
            write_error(resp, 404, "model_alias_not_found", "model alias not found: " + name);
            return;
        }
        const auto persisted = deps.config_repository->transact_active(
            req.get_header_value("If-Match"),
            [&](const ConfigSnapshot& snapshot) -> foundation::Result<std::string> {
                const auto removed = deps.gw.coordinator.registry().remove_alias(name);
                if (!removed) return foundation::Err<std::string>(
                    removed.error().code, removed.error().message);
                return render_aliases(
                    snapshot.has_active ? snapshot.active : snapshot.base,
                    deps.gw.coordinator.registry().aliases());
            },
            [&] { (void)deps.gw.coordinator.registry().set_alias(*previous); });
        if (!persisted) {
            const int status = persisted.error().code == foundation::ErrorCode::AlreadyExists
                ? 409 : 500;
            write_error(resp, status,
                        status == 409 ? "config_conflict" : "model_alias_persist_failed",
                        persisted.error().message);
            return;
        }
        write_json(resp, 200, {{"ok", true}, {"revision", persisted->revision}});
    }));
