    server.Post(R"(^/api/inferdeck/v1/optimize/profile$)",
                wrap([deps](const httplib::Request& req,
                            httplib::Response& resp) {
        if (deps.gw.coordinator.active_request_count() > 0 ||
            deps.gw.coordinator.queued_request_count() > 0) {
            write_error(resp, 409, "optimization_busy",
                        "profile analysis waits until active and queued requests are zero");
            return;
        }
        if (deps.gw.swap_tracker && deps.gw.swap_tracker->snapshot().swapping) {
            write_error(resp, 409, "optimization_busy",
                        "profile analysis waits until the current model swap finishes");
            return;
        }
        const auto gpu = deps.gpu.latest();
        if (!gpu.available || gpu.vram_total_mb <= 0.0) {
            write_error(resp, 503, "optimization_hardware_unavailable",
                        "GPU VRAM telemetry is required for profile analysis");
            return;
        }
        if (gpu.utilization_pct > 20.0) {
            write_error(resp, 409, "optimization_busy",
                        "GPU utilization is above the 20 percent safety threshold");
            return;
        }
        try {
            const auto body = nlohmann::json::parse(req.body);
            const std::string model_name = body.value("model", "");
            auto registered =
                deps.gw.coordinator.registry().get_info_result(model_name);
            if (!registered) {
                write_error(resp, 404, "optimization_model_not_found",
                            registered.error().message);
                return;
            }
            const auto& info = *registered;
            if (info.runtime != "llama_cpp") {
                write_error(resp, 400, "optimization_runtime_unsupported",
                            "profile analysis currently supports llama.cpp LLM models");
                return;
            }

            optimize::ProfileInput input;
            input.model = model_name;
            input.total_vram_mb = gpu.vram_total_mb;
            input.model_file_mb =
                artifact_size_mb(info.gguf_path) +
                (info.has_vision ? artifact_size_mb(info.mmproj_path) : 0.0);
            input.configured_vram_mb = info.vram_required_mb;
            input.context_per_slot =
                body.value("contextPerSlot", info.context_size);
            input.slots = body.value("slots", info.n_slots);
            input.min_slots = body.value("minSlots", info.min_slots);
            input.n_batch = body.value("nBatch", 512);
            input.n_ubatch = body.value("nUbatch", input.n_batch);
            input.cache_type_k =
                body.value("cacheTypeK", std::string{"q8_0"});
            input.cache_type_v =
                body.value("cacheTypeV", std::string{"q8_0"});
            input.flash_attention =
                body.value("flashAttention", std::string{"auto"});

            if (deps.gw.stats_db) {
                for (const auto& usage : deps.gw.stats_db->model_usage()) {
                    if (usage.model != model_name ||
                        usage.total_duration_ms <= 0.0) {
                        continue;
                    }
                    input.observed_tokens_per_second =
                        static_cast<double>(usage.completion_tokens) /
                        (usage.total_duration_ms / 1000.0);
                    break;
                }
            }

            const auto result = optimize::recommend_profile(input);
            nlohmann::json candidates = nlohmann::json::array();
            for (const auto& candidate : result.candidates) {
                candidates.push_back(profile_candidate_json(candidate));
            }
            write_json(resp, 200, {
                {"model", model_name},
                {"mode", "profile_estimate"},
                {"measured", result.measured},
                {"observedTokensPerSecond",
                 input.observed_tokens_per_second},
                {"modelFileMb", input.model_file_mb},
                {"totalVramMb", input.total_vram_mb},
                {"weights", {
                    {"quality", result.quality_weight},
                    {"speed", result.speed_weight},
                    {"parallelism", result.parallelism_weight},
                    {"headroom", result.headroom_weight},
                }},
                {"recommended",
                 profile_candidate_json(result.recommended)},
                {"candidates", std::move(candidates)},
                {"notes", result.notes},
            });
        } catch (const std::invalid_argument& error) {
            write_error(resp, 400, "invalid_optimization_profile",
                        error.what());
        } catch (const std::exception& error) {
            write_error(resp, 422, "optimization_failed", error.what());
        }
    }));

    server.Post(R"(^/api/inferdeck/v1/optimize/benchmark$)",
                wrap([deps](const httplib::Request& req,
                            httplib::Response& resp) {
        if (!deps.profile_benchmark) {
            write_error(resp, 503, "benchmark_unavailable",
                        "measured profile benchmark is unavailable");
            return;
        }
        const auto gpu = deps.gpu.latest();
        if (!gpu.available || gpu.vram_total_mb <= 0.0) {
            write_error(resp, 503, "optimization_hardware_unavailable",
                        "GPU VRAM telemetry is required for measured benchmarking");
            return;
        }
        if (gpu.utilization_pct > 20.0) {
            write_error(resp, 409, "optimization_busy",
                        "GPU utilization is above the 20 percent safety threshold");
            return;
        }
        try {
            const auto body = nlohmann::json::parse(req.body);
            const std::string model_name = body.value("model", "");
            auto registered =
                deps.gw.coordinator.registry().get_info_result(model_name);
            if (!registered) {
                write_error(resp, 404, "optimization_model_not_found",
                            registered.error().message);
                return;
            }
            const auto& info = *registered;
            if (info.runtime != "llama_cpp") {
                write_error(resp, 400, "optimization_runtime_unsupported",
                            "measured benchmarking currently supports llama.cpp LLM models");
                return;
            }
            optimize::ProfileInput input;
            input.model = model_name;
            input.total_vram_mb = gpu.vram_total_mb;
            input.model_file_mb =
                artifact_size_mb(info.gguf_path) +
                (info.has_vision ? artifact_size_mb(info.mmproj_path) : 0.0);
            input.configured_vram_mb = info.vram_required_mb;
            input.context_per_slot =
                body.value("contextPerSlot", info.context_size);
            input.slots = body.value("slots", info.n_slots);
            input.min_slots = body.value("minSlots", info.min_slots);
            input.n_batch = body.value("nBatch", 512);
            input.n_ubatch = body.value("nUbatch", input.n_batch);
            input.cache_type_k =
                body.value("cacheTypeK", std::string{"q8_0"});
            input.cache_type_v =
                body.value("cacheTypeV", std::string{"q8_0"});
            const auto started = deps.profile_benchmark->start(
                info, input, body.value("candidateLimit", 3));
            if (!started) {
                const int status =
                    started.error().code == foundation::ErrorCode::InvalidArgument
                        ? 400 : 409;
                write_error(resp, status, "benchmark_not_started",
                            started.error().message);
                return;
            }
            write_json(resp, 202,
                       profile_benchmark_snapshot_json(*started));
        } catch (const nlohmann::json::exception& error) {
            write_error(resp, 400, "invalid_benchmark_request", error.what());
        } catch (const std::exception& error) {
            write_error(resp, 422, "benchmark_not_started", error.what());
        }
    }));

    server.Get(R"(^/api/inferdeck/v1/optimize/benchmark$)",
               wrap([deps](const httplib::Request&,
                           httplib::Response& resp) {
        if (!deps.profile_benchmark) {
            write_error(resp, 503, "benchmark_unavailable",
                        "measured profile benchmark is unavailable");
            return;
        }
        write_json(resp, 200, profile_benchmark_snapshot_json(
            deps.profile_benchmark->snapshot()));
    }));

    server.Get(R"(^/api/inferdeck/v1/optimize/schedule$)",
               wrap([deps](const httplib::Request&, httplib::Response& resp) {
        if (!deps.profile_benchmark_scheduler) {
            write_error(resp, 503, "optimization_schedule_unavailable",
                        "scheduled optimization is unavailable");
            return;
        }
        nlohmann::json schedules = nlohmann::json::array();
        for (const auto& status : deps.profile_benchmark_scheduler->statuses()) {
            schedules.push_back({
                {"model", status.model},
                {"enabled", status.enabled},
                {"windowStart", status.window_start},
                {"windowEnd", status.window_end},
                {"nextRunUnixMs", status.next_run_unix_ms},
                {"lastStartedUnixMs", status.last_started_unix_ms},
                {"lastFinishedUnixMs", status.last_finished_unix_ms},
                {"lastOutcome", status.last_outcome},
                {"lastMessage", status.last_message},
            });
        }
        write_json(resp, 200, {
            {"timezone", deps.profile_benchmark_scheduler->timezone_name()},
            {"schedules", std::move(schedules)},
        });
    }));

    server.Post(R"(^/api/inferdeck/v1/optimize/benchmark/cancel$)",
                wrap([deps](const httplib::Request&,
                            httplib::Response& resp) {
        if (!deps.profile_benchmark) {
            write_error(resp, 503, "benchmark_unavailable",
                        "measured profile benchmark is unavailable");
            return;
        }
        const auto cancelled = deps.profile_benchmark->cancel();
        if (!cancelled) {
            write_error(resp, 409, "benchmark_not_running",
                        cancelled.error().message);
            return;
        }
        write_json(resp, 202, profile_benchmark_snapshot_json(
            deps.profile_benchmark->snapshot()));
    }));
