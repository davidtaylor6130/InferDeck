    server.Get(R"(^/api/inferdeck/v1/post-training/capabilities$)",
               wrap([](const httplib::Request&, httplib::Response& resp) {
        write_json(resp, 200, {
            {"inProcess", true},
            {"quantization", {
                {"available", true},
                {"types", {"Q4_K_M", "Q5_K_M", "Q6_K", "Q8_0"}},
                {"managedSourcesOnly", true},
                {"requantization", false},
                {"maxConcurrentJobs", 1},
                {"cancellable", false},
                {"computeResource", "cpu"},
                {"blocksNewBackgroundLeases", true}
            }},
            {"fineTuning", {
                {"available", false},
                {"reason", "the vendored backend has no supported adapter-training and save API"}
            }}
        });
    }));

    server.Get(R"(^/api/inferdeck/v1/post-training/quantizations$)",
               wrap([deps](const httplib::Request&, httplib::Response& resp) {
        if (!deps.model_store) {
            write_error(resp, 503, "post_training_unavailable",
                        "model store is unavailable");
            return;
        }
        nlohmann::json jobs = nlohmann::json::array();
        for (const auto& job : deps.model_store->quantizations()) {
            jobs.push_back(to_json(job));
        }
        write_json(resp, 200, {{"jobs", std::move(jobs)}});
    }));

    server.Post(R"(^/api/inferdeck/v1/post-training/quantizations$)",
                wrap([deps](const httplib::Request& req,
                            httplib::Response& resp) {
        if (!deps.model_store) {
            write_error(resp, 503, "post_training_unavailable",
                        "model store is unavailable");
            return;
        }
        try {
            const auto body = nlohmann::json::parse(req.body);
            if (!body.is_object()) {
                write_error(resp, 400, "invalid_quantization_request",
                            "request body must be a JSON object");
                return;
            }
            for (const auto& [name, _] : body.items()) {
                if (name != "sourceModel" && name != "outputModel" &&
                    name != "quantization" && name != "threads") {
                    write_error(resp, 400, "invalid_quantization_request",
                                "unknown request field: " + name);
                    return;
                }
            }
            auto result = deps.model_store->quantize(
                body.value("sourceModel", ""),
                body.value("outputModel", ""),
                body.value("quantization", ""),
                body.value("threads", 0));
            if (!result) {
                const int status =
                    result.error().code == foundation::ErrorCode::NotFound ? 404 :
                    result.error().code == foundation::ErrorCode::AlreadyExists ? 409 :
                    result.error().code == foundation::ErrorCode::Unavailable ? 409 : 400;
                write_error(resp, status, "quantization_not_started",
                            result.error().message);
                return;
            }
            write_json(resp, 202, {
                {"id", *result}, {"state", "queued"}, {"cancellable", false}
            });
        } catch (const nlohmann::json::exception& error) {
            write_error(resp, 400, "invalid_quantization_request", error.what());
        }
    }));
