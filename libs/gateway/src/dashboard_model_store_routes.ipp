    server.Get(R"(^/api/inferdeck/v1/model-store/search$)", wrap([deps](const httplib::Request& req,
                                                            httplib::Response& resp) {
        if (!deps.model_store) {
            write_error(resp, 503, "model_store_unavailable", "model store is unavailable");
            return;
        }
        const std::string query = req.has_param("q") ? req.get_param_value("q") : "";
        const std::string runtime = req.has_param("runtime") ? req.get_param_value("runtime") : "";
        const std::string modality = req.has_param("modality") ? req.get_param_value("modality") : "";
        int limit = 20;
        if (req.has_param("limit")) {
            try {
                limit = std::clamp(std::stoi(req.get_param_value("limit")), 1, 100);
            } catch (...) {
                write_error(resp, 400, "invalid_model_search", "limit must be an integer");
                return;
            }
        }
        auto result = deps.model_store->search(query, runtime, modality, limit);
        if (!result) {
            write_error(resp, result.error().code == foundation::ErrorCode::InvalidArgument ? 400 : 502,
                        "model_search_failed", result.error().message);
            return;
        }
        write_json(resp, 200, {{"models", std::move(*result)}});
    }));
    server.Get(R"(^/api/inferdeck/v1/model-store/inspect$)", wrap([deps](const httplib::Request& req,
                                                             httplib::Response& resp) {
        if (!deps.model_store) {
            write_error(resp, 503, "model_store_unavailable", "model store is unavailable");
            return;
        }
        const std::string repo = req.has_param("repo") ? req.get_param_value("repo") : "";
        auto result = deps.model_store->inspect(repo);
        if (!result) {
            const int status = result.error().code == foundation::ErrorCode::InvalidArgument ? 400 :
                               result.error().code == foundation::ErrorCode::NotFound ? 404 : 502;
            write_error(resp, status, "model_inspect_failed", result.error().message);
            return;
        }
        write_json(resp, 200, std::move(*result));
    }));

    server.Get(R"(^/api/inferdeck/v1/model-store/downloads$)", wrap([deps](const httplib::Request&,
                                                               httplib::Response& resp) {
        if (!deps.model_store) {
            write_error(resp, 503, "model_store_unavailable", "model store is unavailable");
            return;
        }
        nlohmann::json downloads = nlohmann::json::array();
        for (const auto& download : deps.model_store->downloads()) downloads.push_back(to_json(download));
        write_json(resp, 200, {{"downloads", std::move(downloads)},
                               {"installed", deps.model_store->installed()},
                               {"library", deps.model_store->library()}});
    }));

    server.Post(R"(^/api/inferdeck/v1/model-store/downloads$)", wrap([deps](const httplib::Request& req,
                                                                httplib::Response& resp) {
        if (!deps.model_store) {
            write_error(resp, 503, "model_store_unavailable", "model store is unavailable");
            return;
        }
        try {
            const auto body = nlohmann::json::parse(req.body);
            auto result = deps.model_store->install(
                body.value("repo", ""), body.value("filename", ""),
                body.value("runtime", ""), body.value("modality", ""),
                body.value("modelName", ""));
            if (!result) {
                const int status = result.error().code == foundation::ErrorCode::AlreadyExists ? 409 : 400;
                write_error(resp, status, "model_install_failed", result.error().message);
                return;
            }
            write_json(resp, 202, {{"id", *result}, {"state", "queued"}});
        } catch (const std::exception& error) {
            write_error(resp, 400, "invalid_model_install", error.what());
        }
    }));

    server.Post(R"(^/api/inferdeck/v1/model-store/downloads/([0-9]+)/(cancel|resume)$)",
                wrap([deps](const httplib::Request& req, httplib::Response& resp) {
        if (!deps.model_store) {
            write_error(resp, 503, "model_store_unavailable", "model store is unavailable");
            return;
        }
        const auto id = static_cast<std::uint64_t>(std::stoull(req.matches[1].str()));
        auto result = req.matches[2].str() == "cancel" ? deps.model_store->cancel(id) :
                                                        deps.model_store->resume(id);
        if (!result) {
            write_error(resp, result.error().code == foundation::ErrorCode::NotFound ? 404 : 409,
                        "download_control_failed", result.error().message);
            return;
        }
        write_json(resp, 200, {{"ok", true}});
    }));

    server.Post(R"(^/api/inferdeck/v1/model-store/(remove|archive)$)", wrap([deps](const httplib::Request& req,
                                                             httplib::Response& resp) {
        if (!deps.model_store) {
            write_error(resp, 503, "model_store_unavailable", "model store is unavailable");
            return;
        }
        try {
            const auto body = nlohmann::json::parse(req.body);
            auto result = req.matches[1].str() == "archive"
                ? deps.model_store->archive(body.value("model", ""))
                : deps.model_store->remove(body.value("model", ""));
            if (!result) {
                const int status = result.error().code == foundation::ErrorCode::NotFound ? 404 : 409;
                write_error(resp, status, "model_retire_failed", result.error().message);
                return;
            }
            write_json(resp, 200, {{"ok", true}});
        } catch (const std::exception& error) {
            write_error(resp, 400, "invalid_model_retire", error.what());
        }
    }));

    server.Post(R"(^/api/inferdeck/v1/model-store/unregister$)", wrap([deps](const httplib::Request& req,
                                                                httplib::Response& resp) {
        if (!deps.config_repository) {
            write_error(resp, 503, "config_unavailable", "configuration repository is unavailable");
            return;
        }
        try {
            const auto body = nlohmann::json::parse(req.body);
            const std::string name = body.value("model", "");
            if (name.empty()) {
                write_error(resp, 400, "missing_model", "request body must include model");
                return;
            }
            if (!body.contains("revision") || !body["revision"].is_string()) {
                write_error(resp, 400, "invalid_model_unregister", "revision is required");
                return;
            }
            const auto info = deps.gw.coordinator.registry().get_info_result(name);
            if (!info) {
                write_error(resp, 404, "model_not_found", info.error().message);
                return;
            }
            bool unregistered = false;
            const auto written = deps.config_repository->transact_active(
                body["revision"].get<std::string>(),
                [&](const ConfigSnapshot& snapshot) -> foundation::Result<std::string> {
                    const auto& original = snapshot.has_active ? snapshot.active : snapshot.base;
                    const auto root = YAML::Load(original);
                    if (root["default_model"] &&
                        root["default_model"].as<std::string>() == name) {
                        return foundation::Err<std::string>(
                            foundation::ErrorCode::AlreadyExists,
                            "change the default model before removing " + name);
                    }
                    for (const auto& alias : deps.gw.coordinator.registry().aliases()) {
                        if (alias.target == name) return foundation::Err<std::string>(
                            foundation::ErrorCode::AlreadyExists,
                            "remove or retarget alias " + alias.name + " first");
                    }
                    auto updated = remove_model_registry_entry(original, name);
                    if (!updated) return updated;
                    const auto removed = deps.gw.coordinator.unregister(name);
                    if (!removed) return foundation::Err<std::string>(
                        removed.error().code, removed.error().message);
                    unregistered = true;
                    return updated;
                },
                [&] {
                    if (unregistered) deps.gw.coordinator.registry().register_model(*info);
                });
            if (!written) {
                const int status = written.error().code == foundation::ErrorCode::AlreadyExists
                    ? 409 : written.error().code == foundation::ErrorCode::NotFound ? 404 : 500;
                write_error(resp, status, "model_unregister_persist_failed",
                            written.error().message);
                return;
            }
            write_json(resp, 200, {
                {"ok", true},
                {"filesDeleted", false},
                {"restartRequired", false},
                {"revision", written->revision},
            });
        } catch (const std::exception& error) {
            write_error(resp, 400, "invalid_model_unregister", error.what());
        }
    }));
