    server.Get(R"(^/api/inferdeck/v1/pricing$)", wrap([deps](const httplib::Request& req,
                                                httplib::Response& resp) {
        (void)req;
        nlohmann::json pricing = nlohmann::json::array();
        std::ifstream file(deps.pricing_file);
        if (file.is_open()) {
            try {
                pricing = nlohmann::json::parse(file);
                if (!pricing.is_array()) pricing = nlohmann::json::array();
            } catch (const std::exception& error) {
                LOG_WARN("pricing_file_invalid", "path={} error={}", deps.pricing_file, error.what());
                pricing = nlohmann::json::array();
            }
        } else {
            LOG_WARN("pricing_file_unreadable", "path={} file_defaults_unavailable=true", deps.pricing_file);
        }
        for (const auto& name : deps.gw.coordinator.registry().list()) {
            const auto info = deps.gw.coordinator.registry().get_info_result(name);
            if (!info || (!info->prompt_price_per_million &&
                          !info->cached_prompt_price_per_million &&
                          !info->completion_price_per_million)) continue;
            auto existing = std::find_if(pricing.begin(), pricing.end(), [&](const nlohmann::json& entry) {
                return entry.value("model_name", "") == name;
            });
            nlohmann::json value = existing == pricing.end()
                ? nlohmann::json{
                    {"model_name", name},
                    {"currency", "USD"},
                    {"prompt_price_per_million",
                     info->prompt_price_per_million.value_or(0.0)},
                    {"cached_prompt_price_per_million",
                     info->cached_prompt_price_per_million.value_or(
                         info->prompt_price_per_million.value_or(0.0))},
                    {"completion_price_per_million",
                     info->completion_price_per_million.value_or(0.0)}}
                : *existing;
            if (info->prompt_price_per_million) {
                value["prompt_price_per_million"] =
                    *info->prompt_price_per_million;
            }
            if (info->cached_prompt_price_per_million) {
                value["cached_prompt_price_per_million"] =
                    *info->cached_prompt_price_per_million;
            }
            if (info->completion_price_per_million) {
                value["completion_price_per_million"] =
                    *info->completion_price_per_million;
            }
            value["source"] = "model_settings";
            if (existing == pricing.end()) pricing.push_back(std::move(value));
            else *existing = std::move(value);
        }
        for (const auto& alias : deps.gw.coordinator.registry().aliases()) {
            auto target = std::find_if(pricing.begin(), pricing.end(), [&](const nlohmann::json& entry) {
                return entry.value("model_name", "") == alias.target;
            });
            if (target == pricing.end()) continue;
            nlohmann::json value = *target;
            value["model_name"] = alias.name;
            value["source"] = "model_alias";
            auto existing = std::find_if(pricing.begin(), pricing.end(), [&](const nlohmann::json& entry) {
                return entry.value("model_name", "") == alias.name;
            });
            if (existing == pricing.end()) pricing.push_back(std::move(value));
            else *existing = std::move(value);
        }
        resp.set_content(pricing.dump(), "application/json");
    }));

    server.Get(R"(^/api/inferdeck/v1/logs$)", wrap([deps](const httplib::Request& req,
                                             httplib::Response& resp) {
        std::size_t limit = 250;
        if (req.has_param("limit")) {
            try { limit = std::clamp<std::size_t>(std::stoul(req.get_param_value("limit")), 1, 1000); } catch (...) {}
        }
        const auto lines = bounded_log_tail(
            deps.log_file.empty() ? "logs/gateway.log" : deps.log_file,
            limit);
        nlohmann::json logs = nlohmann::json::array();
        for (const auto& item : lines) {
            logs.push_back({{"message", item}});
        }
        resp.set_content(nlohmann::json{{"logs", logs}}.dump(), "application/json");
    }));

    server.Get(R"(^/api/inferdeck/v1/events/stream$)", wrap([deps](const httplib::Request& req,
                                                 httplib::Response& resp) {
        (void)req;
        if (!deps.gw.events) {
            resp.status = 503;
            return;
        }
        auto lease = std::make_shared<DashboardStreamLease>();
        if (!lease->acquire()) {
            write_error(resp, 503, "too_many_event_streams",
                        "dashboard event stream limit reached");
            return;
        }
        auto sub = deps.gw.events->subscribe(64);
        LOG_INFO("sse_subscribe", "subscribers={}", deps.gw.events->subscriber_count());
        resp.set_header("Cache-Control", "no-cache");
        resp.set_header("X-Accel-Buffering", "no");
        resp.set_chunked_content_provider(
            "text/event-stream",
            [sub, lease](std::size_t, httplib::DataSink& sink) -> bool {
                if (sub->closed()) return false;
                auto ev = sub->wait_for(std::chrono::milliseconds{2000});
                if (!sink.is_writable()) return false;
                std::string out;
                if (!ev) {
                    out = ": \n\n";
                } else {
                    out = "event: " + ev->name + "\ndata: " + ev->data + "\n\n";
                }
                return sink.write(out.data(), out.size());
            },
            [sub, lease, deps](bool) {
                sub->close();
                lease->release();
                LOG_INFO("sse_unsubscribe", "subscribers={}",
                         deps.gw.events ? deps.gw.events->subscriber_count() : 0);
            });
    }));
