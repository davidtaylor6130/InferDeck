void handle_responses(const httplib::Request& req, httplib::Response& resp,
                      const GatewayDeps& deps) {
    if (!require_json_media_type(req, resp)) return;
    nlohmann::json request;
    try {
        request = nlohmann::json::parse(req.body);
    } catch (const std::exception&) {
        write_error(resp, 400, "invalid_json", "invalid JSON");
        return;
    }
    const bool derivative =
        deps.compatibility_profile == CompatibilityProfile::OpenAIDerivative;
    auto parsed = parse_openai_responses_request(request, derivative);
    if (!parsed) {
        const std::string& message = parsed.error().message;
        const std::string code =
            message.starts_with("unsupported Responses parameter:")
                ? "unsupported_parameter" : "invalid_request_error";
        write_error(resp, 400, code, message,
                    parsed.error().field.empty()
                        ? nlohmann::json(nullptr)
                        : nlohmann::json(parsed.error().field));
        return;
    }
    const std::string& requested_model = parsed->requested_model;
    const auto resolved_model = resolve_model_name(deps, requested_model);
    if (!resolved_model) {
        write_error(resp, 404, "model_not_found", resolved_model.error().message);
        return;
    }
    const std::string& model_name = resolved_model->resolved;
    auto info = deps.coordinator.registry().get_info_result(model_name);
    if (!info) {
        write_error(resp, 404, "model_not_found", info.error().message);
        return;
    }
    if (!info->supports("responses")) {
        write_error(resp, 400, "unsupported_capability",
                    "model does not support Responses API: " + model_name);
        return;
    }
    if (parsed->capability_field) {
        write_error(
            resp, 400, "unsupported_capability",
            "model '" + model_name + "' does not support " +
                parsed->capability.value_or("the requested capability"),
            *parsed->capability_field);
        return;
    }
    if (!info->has_vision && request.contains("input") &&
        response_input_uses_vision(request["input"])) {
        write_error(resp, 400, "unsupported_capability",
                    "model does not support image input: " + model_name);
        return;
    }
    std::string cache_reservation_key;
    for (const auto field : {"prompt_cache_key", "user"}) {
        if (request.contains(field) && request[field].is_string() &&
            !request[field].get_ref<const std::string&>().empty()) {
            cache_reservation_key =
                "openai-cache:" + request[field].get<std::string>();
            break;
        }
    }
    const bool stream = parsed->stream;
    if (!stream) {
        auto acquired = acquire_generation_slot(
            req, resp, deps, parsed->priority, requested_model, model_name,
            cache_reservation_key);
        if (!acquired) return;
        auto observation = observe_request(req, resp, deps, "text", false);
        observation.queue_duration_ms = acquired->queue_duration_ms;
        observation.swap_load_duration_ms = acquired->swap_load_duration_ms;
        GenerationSession session(
            deps.coordinator, deps.metrics, deps.stats_db, deps.events,
            acquired->slot_id, requested_model, model_name,
            acquired->reservation_key,
            acquired->voice_session_token.value_or(0),
            deps.voice_session_grace_ms, std::move(observation));
        auto result = session.run(parsed->generation);
        if (!result) {
            const auto error = map_openai_error(result.error().code);
            write_error(resp, error.status, error.code, result.error().message);
            session.finish_once(false, error.status, "inference_error");
            return;
        }
        write_json(resp, 200, result_to_response(
            *result, request, make_id("resp_"), requested_model,
            static_cast<std::int64_t>(std::time(nullptr))));
        session.finish_once(false, 200, "completed");
        return;
    }
    auto acquired = acquire_generation_slot(
        req, resp, deps, parsed->priority, requested_model, model_name,
        cache_reservation_key);
    if (!acquired) return;
    auto observation = observe_request(req, resp, deps, "text", true);
    observation.queue_duration_ms = acquired->queue_duration_ms;
    observation.swap_load_duration_ms = acquired->swap_load_duration_ms;
    auto session = std::make_shared<GenerationSession>(
        deps.coordinator, deps.metrics, deps.stats_db, deps.events,
        acquired->slot_id, requested_model, model_name,
        acquired->reservation_key,
        acquired->voice_session_token.value_or(0),
        deps.voice_session_grace_ms, std::move(observation));
    auto state = std::make_shared<ResponsesStreamState>();
    state->session = session;
    state->request = request;
    state->include_obfuscation =
        !request.contains("stream_options") ||
        request["stream_options"].is_null() ||
        request["stream_options"].value("include_obfuscation", true);
    state->response_id = make_id("resp_");
    state->model = requested_model;
    state->created_at = static_cast<std::int64_t>(std::time(nullptr));
    session->start(std::move(parsed->generation));

    resp.status = 200;
    resp.set_header("Cache-Control", "no-cache");
    resp.set_header("Connection", "keep-alive");
    resp.set_chunked_content_provider(
        "text/event-stream",
        [state](std::size_t, httplib::DataSink& sink) {
            try {
                if (!start_response_stream(*state, sink)) {
                    state->session->finish_once(true, 499, "start_write_failed");
                    return false;
                }
                std::unique_lock lock(state->session->mtx);
                while (!state->session->cv.wait_for(
                    lock, std::chrono::seconds{2}, [&] {
                        return !state->session->delta_queue.empty() ||
                               state->session->inference_done ||
                               state->session->aborted.load();
                    })) {
                    lock.unlock();
                    if (!sink.write(": \n\n", 4)) {
                        state->session->finish_once(
                            true, 499, "heartbeat_write_failed");
                        return false;
                    }
                    lock.lock();
                }
                if (state->session->aborted.load() &&
                    !state->session->inference_done &&
                    state->session->delta_queue.empty()) {
                    lock.unlock();
                    state->session->finish_once(true, 499, "aborted");
                    return false;
                }
                if (!state->session->delta_queue.empty()) {
                    std::deque<model::InferenceDelta> deltas;
                    deltas.swap(state->session->delta_queue);
                    state->session->pending_bytes = 0;
                    lock.unlock();
                    state->session->cv.notify_all();
                    if (!sink.is_writable()) {
                        state->session->finish_once(
                            true, 499, "sink_not_writable");
                        return false;
                    }
                    for (const auto& delta : deltas) {
                        if (!apply_generation_delta(*state, sink, delta)) {
                            state->session->finish_once(
                                true, 499, "chunk_write_failed");
                            return false;
                        }
                    }
                    return true;
                }
                const bool inference_error = state->session->inference_error;
                const auto error_code = state->session->error_code;
                const auto error_message = state->session->error_msg;
                const auto result = state->session->final_result;
                lock.unlock();

                const auto trailing = state->session->utf8.finish();
                if (!apply_sanitized_generation_delta(*state, sink, trailing)) {
                    state->session->finish_once(
                        true, 499, "trailing_chunk_write_failed");
                    return false;
                }
                if (inference_error) {
                    const auto error = map_openai_error(error_code);
                    if (!fail_response_stream(*state, sink, {
                        {"code", error.code}, {"message", error_message},
                    })) {
                        state->session->finish_once(
                            true, 499, "error_write_failed");
                        return false;
                    }
                    state->session->finish_once(
                        false, error.status, "inference_error");
                } else {
                    if (result) {
                        state->usage = {
                            {"prompt_tokens", result->prompt_tokens},
                            {"prompt_tokens_details", {
                                {"cached_tokens", result->cached_prompt_tokens},
                            }},
                            {"completion_tokens", result->completion_tokens},
                        };
                    }
                    if (!finish_response_stream(*state, sink)) {
                        state->session->finish_once(
                            true, 499, "done_write_failed");
                        return false;
                    }
                    state->session->finish_once(false, 200, "completed");
                }
                sink.done();
                return false;
            } catch (...) {
                state->session->finish_once(
                    true, 500, "provider_exception");
                return false;
            }
        },
        [state](bool success) {
            state->session->finish_once(
                !success, success ? 200 : 499,
                success ? "resource_releaser_success" : "resource_releaser");
        });
}
