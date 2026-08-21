namespace {

struct AcquiredChatSlot {
    int slot_id{-1};
    std::string reservation_key;
    std::optional<std::uint64_t> voice_session_token;
    double queue_duration_ms{};
    double swap_load_duration_ms{};
};

std::optional<AcquiredChatSlot> acquire_chat_slot(
    const httplib::Request& req, httplib::Response& resp,
    const GatewayDeps& deps, int priority,
    const std::string& requested_model, const std::string& model_name) {
    AcquiredChatSlot acquired;
    const auto acquisition_started = std::chrono::steady_clock::now();
    acquired.reservation_key = request_client_key(req);
    if (!acquired.reservation_key.empty()) {
        acquired.voice_session_token = deps.coordinator.hold_priority_session(
            acquired.reservation_key, model_name);
    }
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::minutes{5};
    const std::function<bool()> cancelled = [&req] {
        return req.is_connection_closed();
    };
    model::AcquireSlotOptions opts;
    opts.timeout = std::chrono::minutes{5};
    opts.block = true;
    opts.priority = std::clamp(priority, -100, 100);
    opts.reservation_key = acquired.reservation_key;
    if (acquired.voice_session_token) opts.priority = 100;
    opts.cancelled = cancelled;
    opts.prepare = [&deps, &model_name, deadline, cancelled, &acquired] {
        const auto started = std::chrono::steady_clock::now();
        auto loaded = ensure_model_loaded(deps, model_name, deadline, cancelled);
        acquired.swap_load_duration_ms += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        if (loaded.ok) return foundation::Ok();
        return foundation::Err<void>(loaded.error_code, loaded.message);
    };
    auto slot = deps.coordinator.acquire_slot(model_name, opts);
    if (slot) {
        acquired.slot_id = *slot;
        const auto total = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - acquisition_started).count();
        acquired.queue_duration_ms = std::max(
            0.0, total - acquired.swap_load_duration_ms);
        return acquired;
    }
    if (acquired.voice_session_token) {
        deps.coordinator.complete_priority_session_hold(
            acquired.reservation_key, *acquired.voice_session_token,
            std::chrono::milliseconds{deps.voice_session_grace_ms});
    }
    int status = 503;
    std::string code = "no_slots";
    if (slot.error().code == foundation::ErrorCode::Timeout) {
        code = "slot_timeout";
    } else if (slot.error().code == foundation::ErrorCode::Cancelled) {
        code = "cancelled";
    } else if (slot.error().code == foundation::ErrorCode::NotFound) {
        status = 404;
        code = "model_not_loaded";
    } else if (slot.error().code == foundation::ErrorCode::NotLoaded) {
        code = "model_not_loaded";
    }
    resp.set_header("Retry-After",
        slot.error().code == foundation::ErrorCode::NotLoaded
            ? deps.default_swap_timeout_s : "1");
    model::InferenceResult failed;
    record_request(deps, requested_model, failed, status, -1, 0.0, 0, model_name);
    write_error(resp, status, code, slot.error().message);
    return std::nullopt;
}

void handle_non_stream_chat(
    httplib::Response& resp, const GatewayDeps& deps,
    const std::string& requested_model, const std::string& model_name,
    const std::string& id, GenerationSession& session,
    const model::InferenceRequest& inference_request) {
    auto predicted = session.run(inference_request);
    if (!predicted) {
        const auto error = map_openai_error(predicted.error().code);
        LOG_ERROR("inference_failed",
                  "model={} slot_id={} status={} code={} error={}",
                  model_name, session.slot_id, error.status, error.code,
                  predicted.error().message);
        write_error(resp, error.status, error.code, predicted.error().message);
        session.finish_once(false, error.status, "inference_error");
        return;
    }
    const auto& result = *predicted;
    nlohmann::json message = {
        {"role", "assistant"},
        {"content", result.text},
    };
    if (deps.compatibility_profile == CompatibilityProfile::OpenAIDerivative &&
        !result.reasoning_text.empty()) {
        message["reasoning_content"] = result.reasoning_text;
    }
    if (!result.tool_calls.empty()) {
        message["tool_calls"] = nlohmann::json::array();
        for (const auto& call : result.tool_calls) {
            message["tool_calls"].push_back(tool_call_json(call));
        }
    }
    const std::string finish_reason = !result.tool_calls.empty()
        ? "tool_calls" : result.finish_reason;
    write_json(resp, 200, {
        {"id", id},
        {"object", "chat.completion"},
        {"created", std::time(nullptr)},
        {"model", requested_model},
        {"choices", nlohmann::json::array({{
            {"index", 0},
            {"message", message},
            {"finish_reason", finish_reason},
        }})},
        {"usage", {
            {"prompt_tokens", result.prompt_tokens},
            {"prompt_tokens_details", {
                {"cached_tokens", result.cached_prompt_tokens},
            }},
            {"completion_tokens", result.completion_tokens},
            {"total_tokens", result.prompt_tokens + result.completion_tokens},
        }},
    });
    session.finish_once(false, 200, "completed");
}

} // namespace

std::optional<AcquiredGenerationSlot> acquire_generation_slot(
    const httplib::Request& req, httplib::Response& resp,
    const GatewayDeps& deps, int priority,
    const std::string& requested_model, const std::string& resolved_model) {
    auto acquired = acquire_chat_slot(req, resp, deps, priority,
                                      requested_model, resolved_model);
    if (!acquired) return std::nullopt;
    return AcquiredGenerationSlot{
        acquired->slot_id,
        std::move(acquired->reservation_key),
        std::move(acquired->voice_session_token),
        acquired->queue_duration_ms,
        acquired->swap_load_duration_ms,
    };
}

void handle_chat_completions(const httplib::Request& req, httplib::Response& resp,
                             const GatewayDeps& deps) {
    if (!require_json_media_type(req, resp)) return;
    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req.body);
    } catch (const std::exception& e) {
        write_error(resp, 400, "invalid_json", e.what());
        return;
    }
    if (!body.contains("model") || !body["model"].is_string()) {
        write_error(resp, 400, "missing_model",
                    "request body must include 'model'", "model");
        return;
    }
    const bool derivative =
        deps.compatibility_profile == CompatibilityProfile::OpenAIDerivative;
    if (!derivative) {
        static constexpr std::array<std::string_view, 15> supported_fields{
            "model", "messages", "max_tokens", "max_completion_tokens",
            "temperature", "top_p", "seed", "stream", "stream_options",
            "tools", "tool_choice", "parallel_tool_calls", "reasoning_effort",
            "response_format", "stop",
        };
        for (const auto& field : body.items()) {
            if (std::find(supported_fields.begin(), supported_fields.end(),
                          field.key()) == supported_fields.end()) {
                write_error(resp, 400, "unsupported_parameter",
                            "unsupported Chat Completions parameter: " +
                                field.key(),
                            field.key());
                return;
            }
        }
        if (body.contains("messages") && body["messages"].is_array()) {
            for (const auto& message : body["messages"]) {
                if (message.is_object() && message.contains("reasoning_content")) {
                    write_error(resp, 400, "unsupported_parameter",
                                "unsupported Chat Completions message parameter: reasoning_content",
                                "messages");
                    return;
                }
            }
        }
    }
    if (body.contains("stream") && !body["stream"].is_boolean()) {
        write_error(resp, 400, "invalid_request_error",
                    "stream must be a boolean", "stream");
        return;
    }
    const bool stream = body.value("stream", false);
    if (body.contains("stream_options") &&
        !body["stream_options"].is_object()) {
        write_error(resp, 400, "invalid_request_error",
                    "stream_options must be an object", "stream_options");
        return;
    }
    if (!stream && body.contains("stream_options")) {
        write_error(resp, 400, "invalid_request_error",
                    "stream_options requires stream to be true",
                    "stream_options");
        return;
    }
    if (body.contains("stream_options")) {
        static constexpr std::array<std::string_view, 1>
            stream_option_fields{"include_usage"};
        for (const auto& field : body["stream_options"].items()) {
            if (std::find(stream_option_fields.begin(),
                          stream_option_fields.end(), field.key()) ==
                stream_option_fields.end()) {
                write_error(
                    resp, 400, "unsupported_parameter",
                    "unsupported stream_options parameter: " + field.key(),
                    "stream_options");
                return;
            }
        }
        if (body["stream_options"].contains("include_usage") &&
            !body["stream_options"]["include_usage"].is_boolean()) {
            write_error(resp, 400, "invalid_request_error",
                        "stream_options.include_usage must be a boolean",
                        "stream_options");
            return;
        }
    }
    const bool include_stream_usage =
        body.contains("stream_options") &&
        body["stream_options"].value("include_usage", false);
    auto inference_request =
        parse_openai_chat_request(body, derivative);
    if (!inference_request) {
        write_error(resp, 400, "invalid_request_error",
                    inference_request.error().message,
                    inference_request.error().field.empty()
                        ? nlohmann::json(nullptr)
                        : nlohmann::json(inference_request.error().field));
        return;
    }
    std::string requested_model = body["model"].get<std::string>();
    const auto resolved_model = resolve_model_name(deps, requested_model);
    if (!resolved_model) {
        write_error(resp, 404, "model_not_found", resolved_model.error().message);
        return;
    }
    const std::string& model_name = resolved_model->resolved;
    if (maintenance_blocks_model(deps, model_name)) {
        write_error(resp, 503, "maintenance_mode",
                    "measured model optimization is using the same compute resource");
        return;
    }
    const auto model_info = deps.coordinator.registry().get_info_result(model_name);
    if (!model_info) {
        write_error(resp, 404, "model_not_found", model_info.error().message);
        return;
    }
    if (!model_info->has_vision && chat_uses_vision(body)) {
        write_error(resp, 400, "unsupported_capability",
                    "model does not support image input: " + model_name);
        return;
    }
    auto reasoning_effort = normalize_reasoning_request(body, *model_info);
    if (!reasoning_effort) {
        write_error(resp, 400, "invalid_request_error",
                    reasoning_effort.error().message);
        return;
    }
    inference_request->reasoning_effort = *reasoning_effort;

    const int priority = derivative && body.contains("priority") &&
            body["priority"].is_number_integer()
        ? body["priority"].get<int>() : 0;
    auto acquired = acquire_generation_slot(
        req, resp, deps, priority, requested_model, model_name);
    if (!acquired) return;
    const int slot_id = acquired->slot_id;
    const auto& reservation_key = acquired->reservation_key;
    const auto& voice_session_token = acquired->voice_session_token;

    const std::string id = make_id();
    const std::string stream_model = requested_model;
    const auto stream_created = static_cast<std::int64_t>(std::time(nullptr));

    auto observation = observe_request(req, resp, deps, "text", stream);
    observation.queue_duration_ms = acquired->queue_duration_ms;
    observation.swap_load_duration_ms = acquired->swap_load_duration_ms;
    auto state = std::make_shared<GenerationSession>(
        deps.coordinator, deps.metrics, deps.stats_db, deps.events,
        slot_id, requested_model, model_name, reservation_key,
        voice_session_token.value_or(0), deps.voice_session_grace_ms,
        std::move(observation));

    if (!stream) {
        handle_non_stream_chat(resp, deps, requested_model, model_name,
                               id, *state, *inference_request);
        return;
    }

    resp.set_header("Content-Type", "text/event-stream");
    resp.set_header("Cache-Control", "no-cache");
    resp.set_header("Connection", "keep-alive");
    state->start(std::move(*inference_request));

    resp.set_chunked_content_provider(
        "text/event-stream",
        [id, stream_model, stream_created, state, include_stream_usage,
         derivative](
            std::size_t, httplib::DataSink& sink) mutable {
            try {
            std::unique_lock<std::mutex> lk(state->mtx);

            while (!state->cv.wait_for(lk, std::chrono::seconds{2}, [&] {
                return !state->delta_queue.empty() || state->inference_done || state->aborted.load();
            })) {
                lk.unlock();
                if (!sink.write(": \n\n", 4)) {
                    LOG_WARN("stream_abort", "model={} slot_id={} reason=heartbeat_write_failed",
                             state->model_name, state->slot_id);
                    state->finish_once(true, 499, "heartbeat_write_failed");
                    return false;
                }
                lk.lock();
            }

            if (state->aborted.load() && !state->inference_done && state->delta_queue.empty()) {
                lk.unlock();
                LOG_WARN("stream_abort", "model={} slot_id={} reason=aborted",
                         state->model_name, state->slot_id);
                state->finish_once(true, 499, "aborted");
                return false;
            }

            if (!state->delta_queue.empty()) {
                std::deque<model::InferenceDelta> deltas;
                while (!state->delta_queue.empty()) {
                    state->pending_bytes -= delta_size(state->delta_queue.front());
                    deltas.push_back(std::move(state->delta_queue.front()));
                    state->delta_queue.pop_front();
                }
                lk.unlock();
                state->cv.notify_all();

                if (!sink.is_writable()) {
                    LOG_WARN("stream_abort", "model={} slot_id={} reason=sink_not_writable",
                             state->model_name, state->slot_id);
                    state->finish_once(true, 499, "sink_not_writable");
                    return false;
                }

                for (const auto& delta : deltas) {
                    auto json_delta = delta_json(state->utf8.on_delta(delta), derivative);
                    if (json_delta.empty()) continue;
                    std::string out = serialize_chat_stream_delta(
                        id, stream_model, stream_created, json_delta,
                        include_stream_usage, derivative);
                    if (!sink.write(out.data(), out.size())) {
                        LOG_WARN("stream_abort", "model={} slot_id={} reason=chunk_write_failed",
                                 state->model_name, state->slot_id);
                        state->finish_once(true, 499, "chunk_write_failed");
                        return false;
                    }
                }
                return true;
            }

            const bool inference_error = state->inference_error;
            const auto error_code = state->error_code;
            const std::string error_msg = state->error_msg;
            const auto final_result = state->final_result;
            lk.unlock();

            auto trailing_delta = delta_json(state->utf8.finish(), derivative);
            if (!trailing_delta.empty()) {
                std::string out = serialize_chat_stream_delta(
                    id, stream_model, stream_created, trailing_delta,
                    include_stream_usage, derivative);
                if (!sink.write(out.data(), out.size())) {
                    LOG_WARN("stream_abort", "model={} slot_id={} reason=trailing_chunk_write_failed",
                             state->model_name, state->slot_id);
                    state->finish_once(true, 499, "trailing_chunk_write_failed");
                    return false;
                }
            }

            if (inference_error) {
                const auto ec = map_openai_error(error_code);
                LOG_ERROR("inference_failed",
                          "model={} slot_id={} status={} code={} error={}",
                          state->model_name, state->slot_id, ec.status, ec.code, error_msg);
                auto error = make_error_json(ec.status, ec.code, error_msg);
                error["error"]["type"] = ec.type;
                std::string err = "data: " + dump_json(error) +
                    "\n\ndata: [DONE]\n\n";
                if (!sink.write(err.data(), err.size())) {
                    LOG_WARN("stream_abort", "model={} slot_id={} reason=error_write_failed",
                             state->model_name, state->slot_id);
                    state->finish_once(true, 499, "error_write_failed");
                    return false;
                }
                state->finish_once(false, ec.status, "inference_error");
            } else {
                const bool has_tool_calls = final_result && !final_result->tool_calls.empty();
                const std::string finish_reason = has_tool_calls ? "tool_calls" :
                    (final_result ? final_result->finish_reason : "stop");
                std::string done = serialize_chat_stream_terminal(
                    id, stream_model, stream_created, finish_reason,
                    final_result ? final_result.get() : nullptr,
                    include_stream_usage);
                if (!sink.write(done.data(), done.size())) {
                    LOG_WARN("stream_abort", "model={} slot_id={} reason=done_write_failed",
                             state->model_name, state->slot_id);
                    state->finish_once(true, 499, "done_write_failed");
                    return false;
                }
                state->finish_once(false, 200, "completed");
            }
            sink.done();
            return false;
            } catch (const std::exception& e) {
                LOG_ERROR("stream_provider_exception", "model={} slot_id={} what={}",
                          state->model_name, state->slot_id, e.what());
                state->finish_once(true, 500, "provider_exception");
                return false;
            } catch (...) {
                LOG_ERROR("stream_provider_unknown_exception", "model={} slot_id={}",
                          state->model_name, state->slot_id);
                state->finish_once(true, 500, "provider_unknown_exception");
                return false;
            }
        },
        [state](bool success) {
            if (!success) {
                LOG_WARN("stream_abort", "model={} slot_id={} reason=resource_releaser",
                         state->model_name, state->slot_id);
                state->finish_once(true, 499, "resource_releaser");
            } else {
                state->finish_once(false, 200, "resource_releaser_success");
            }
        });
}
