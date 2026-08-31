void handle_audio_speech(const httplib::Request& req, httplib::Response& resp,
                         const GatewayDeps& deps) {
    if (!require_json_media_type(req, resp)) return;
    nlohmann::json body;
    try { body = nlohmann::json::parse(req.body); }
    catch (const std::exception& error) { write_error(resp, 400, "invalid_json", error.what()); return; }
    const bool derivative =
        deps.compatibility_profile == CompatibilityProfile::OpenAIDerivative;
    if (!derivative && body.is_object()) {
        static const std::unordered_set<std::string> fields{
            "model", "input", "voice", "instructions", "response_format",
            "speed", "stream_format",
        };
        for (const auto& field : body.items()) {
            if (!fields.contains(field.key())) {
                write_error(resp, 400, "unsupported_parameter",
                            "unsupported speech parameter: " + field.key(),
                            field.key());
                return;
            }
        }
    }
    if (!body.is_object()) {
        write_error(resp, 400, "invalid_speech_request",
                    "request body must be an object");
        return;
    }
    if (!body.contains("model") || !body["model"].is_string() ||
        body["model"].get_ref<const std::string&>().empty()) {
        write_error(resp, 400, "invalid_speech_request",
                    "model must be a non-empty string", "model");
        return;
    }
    if (!body.contains("input") || !body["input"].is_string() ||
        body["input"].get_ref<const std::string&>().empty()) {
        write_error(resp, 400, "invalid_speech_request",
                    "input must be a non-empty string", "input");
        return;
    }
    if (!body.contains("voice")) {
        write_error(resp, 400, "invalid_speech_request",
                    "voice is required", "voice");
        return;
    }
    const std::string model_name = body["model"].get<std::string>();
    model::SpeechRequest request;
    bool needs_instruction_capability = false;
    request.input = body["input"].get<std::string>();
    if (body["voice"].is_string()) {
        request.voice = body["voice"].get<std::string>();
    } else if (body["voice"].is_object() && body["voice"].size() == 1 &&
               body["voice"].contains("id") &&
               body["voice"]["id"].is_string()) {
        request.voice = body["voice"]["id"].get<std::string>();
    }
    if (request.voice.empty()) {
        write_error(resp, 400, "invalid_speech_request",
                    "voice must be a non-empty string or voice ID object",
                    "voice");
        return;
    }
    if (body.contains("speed")) {
        if (!body["speed"].is_number()) {
            write_error(resp, 400, "invalid_speech_request",
                        "speed must be a number between 0.25 and 4.0",
                        "speed");
            return;
        }
        const double speed = body["speed"].get<double>();
        if (!std::isfinite(speed) || speed < 0.25 || speed > 4.0) {
            write_error(resp, 400, "invalid_speech_request",
                        "speed must be a number between 0.25 and 4.0",
                        "speed");
            return;
        }
        request.speed = static_cast<float>(speed);
    }
    if (body.contains("instructions")) {
        if (!body["instructions"].is_string()) {
            write_error(resp, 400, "invalid_speech_request",
                        "instructions must be a string", "instructions");
            return;
        }
        const auto instructions = body["instructions"].get<std::string>();
        if (utf8_character_count(instructions) > 4096) {
            write_error(resp, 400, "invalid_speech_request",
                        "instructions must contain at most 4096 characters",
                        "instructions");
            return;
        }
        needs_instruction_capability = !instructions.empty();
    }
    if (utf8_character_count(request.input) > 4096) {
        write_error(resp, 400, "invalid_speech_request",
                    "input must contain at most 4096 characters", "input");
        return;
    }
    if (body.contains("response_format") && !body["response_format"].is_string()) {
        write_error(resp, 400, "invalid_speech_request",
                    "response_format must be a string", "response_format");
        return;
    }
    request.format = body.value("response_format", std::string{"mp3"});
    static const std::array formats{"mp3", "opus", "aac", "flac", "wav", "pcm"};
    if (std::find(formats.begin(), formats.end(), request.format) == formats.end()) {
        write_error(resp, 400, "unsupported_response_format",
                    "response_format must be mp3, opus, aac, flac, wav, or pcm",
                    "response_format");
        return;
    }
    if (body.contains("stream_format") && !body["stream_format"].is_string()) {
        write_error(resp, 400, "invalid_speech_request",
                    "stream_format must be a string", "stream_format");
        return;
    }
    const std::string stream_format = body.value("stream_format", std::string{"audio"});
    if (stream_format != "audio" && stream_format != "sse") {
        write_error(resp, 400, "unsupported_stream_format",
                    "stream_format must be audio or sse", "stream_format");
        return;
    }
    const auto resolved_model = resolve_model_name(deps, model_name);
    if (!resolved_model) {
        write_error(resp, 404, "model_not_found", resolved_model.error().message);
        return;
    }
    const auto info = deps.coordinator.registry().get_info_result(resolved_model->resolved);
    if (!info || !info->supports("audio_speech")) {
        write_error(resp, 400, "unsupported_capability",
                    "model does not support audio speech generation");
        return;
    }
    if (needs_instruction_capability) {
        write_error(resp, 400, "unsupported_capability",
                    "model '" + resolved_model->resolved +
                        "' does not support speech instructions",
                    "instructions");
        return;
    }
    if (stream_format == "sse") {
        write_error(resp, 400, "unsupported_capability",
                    "model '" + resolved_model->resolved +
                        "' does not support SSE speech events",
                    "stream_format");
        return;
    }
    const bool wav_runtime =
        info && (info->runtime == "sherpa_onnx" ||
                 info->runtime == "windows_sapi");
    const auto input_characters = utf8_character_count(request.input);
    const auto observation = observe_request(
        req, resp, deps, "audio_speech", request.format != "wav");
    if (wav_runtime && request.format != "wav" && request.format != "pcm") {
        write_error(resp, 400, "unsupported_capability",
                    "model '" + resolved_model->resolved +
                        "' supports wav and pcm speech output only",
                    "response_format");
        return;
    }
    const std::string& runtime_model = resolved_model->resolved;
    const auto validation = deps.coordinator.validate_speech_request(
        runtime_model, request);
    if (!validation) {
        const int status = status_for(validation.error().code);
        write_error(resp, status,
                    status == 400 ? "invalid_speech_request"
                                  : "speech_validation_failed",
                    validation.error().message);
        return;
    }
    const auto validation_deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds{30};
    const std::function<bool()> cancelled = [&req] {
        return req.is_connection_closed();
    };
    const auto prepared = ensure_model_loaded(
        deps, runtime_model, validation_deadline, cancelled);
    if (!prepared.ok) {
        write_error(resp, prepared.status, "speech_admission_failed",
                    prepared.message);
        return;
    }
    VoiceSessionGuard voice_session(req, deps);
    auto job = begin_job(model_name, "audio_speech");
    if (deps.compatibility_profile == CompatibilityProfile::OpenAIDerivative) {
        resp.set_header("X-InferDeck-Job-Id", std::to_string(job->id));
    }
    auto slot = acquire_media_slot(req, deps, runtime_model, job);
    if (!slot) {
        const int status = status_for(slot.error().code);
        const int internal_status = internal_status_for(slot.error().code);
        write_error(resp, status, "speech_admission_failed", slot.error().message);
        record_media(deps, model_name, 0, internal_status, -1,
                     0.0, 0, observation);
        finish_job(job, internal_status == 499 ? "cancelled" : "failed");
        return;
    }
    SlotGuard guard{&deps.coordinator, runtime_model, *slot};
    if (request.format == "wav") {
        try {
            auto result = deps.coordinator.synthesize(
                runtime_model, *slot, request,
                [&req, job](const std::byte*, std::size_t) {
                    return !req.is_connection_closed() &&
                           !job->cancelled->load();
                });
            if (!result) {
                const bool was_cancelled = job->cancelled->load() ||
                    result.error().code == foundation::ErrorCode::Cancelled;
                const int status = was_cancelled
                    ? 408
                    : status_for(result.error().code);
                write_error(resp, status, "speech_generation_failed",
                            result.error().message);
                record_media(deps, model_name, 0,
                             was_cancelled ? 499 : status, *slot,
                             0.0, 0, observation);
                finish_job(job, was_cancelled ? "cancelled" : "failed");
                return;
            }
            if (result->bytes.empty()) {
                write_error(resp, 500, "speech_generation_failed",
                            "speech runtime returned no audio");
                record_media(deps, model_name, result->duration_ms, 500, *slot,
                             0.0, 0, observation);
                finish_job(job, "failed");
                return;
            }
            resp.set_content(
                std::string(
                    reinterpret_cast<const char*>(result->bytes.data()),
                    result->bytes.size()),
                result->content_type.empty() ? "audio/wav"
                                             : result->content_type);
            auto completed_observation = observation;
            completed_observation.output_audio_seconds =
                result->output_audio_seconds;
            record_media(deps, model_name, result->duration_ms, 200, *slot,
                         0.0, input_characters, completed_observation);
            finish_job(job, "completed");
            return;
        } catch (const std::exception& error) {
            write_error(resp, 500, "speech_generation_failed", error.what());
            record_media(deps, model_name, 0, 500, *slot,
                         0.0, 0, observation);
            finish_job(job, "failed");
            return;
        }
    }
    auto state = std::make_shared<SpeechStreamState>(deps);
    state->coordinator = &deps.coordinator;
    state->model = runtime_model;
    state->requested_model = model_name;
    state->slot = *slot;
    state->job = job;
    state->input_characters = input_characters;
    state->observation = observation;
    state->session_key = voice_session.key();
    state->session_token = voice_session.token();
    try {
        state->worker = std::thread([state, request] {
            try {
                auto result = state->coordinator->synthesize(
                    state->model, state->slot, request,
                    [state](const std::byte* data, std::size_t size) {
                        try {
                            if (state->aborted.load() || state->job->cancelled->load()) return false;
                            if (size > 0) {
                                if (!data) {
                                    state->failed.store(true);
                                    return false;
                                }
                                std::unique_lock lock(state->mutex);
                                auto has_capacity = [state, size] {
                                    const bool byte_capacity =
                                        state->chunks.empty() ||
                                        (size <= state->max_pending_bytes &&
                                         state->pending_bytes <=
                                             state->max_pending_bytes - size);
                                    return state->chunks.size() <
                                               state->max_pending_chunks &&
                                           byte_capacity;
                                };
                                while (!state->aborted.load() &&
                                       !state->job->cancelled->load() &&
                                       !has_capacity()) {
                                    state->cv.wait_for(
                                        lock, std::chrono::milliseconds{100});
                                }
                                if (state->aborted.load() ||
                                    state->job->cancelled->load()) {
                                    return false;
                                }
                                state->chunks.emplace_back(reinterpret_cast<const char*>(data), size);
                                state->pending_bytes += size;
                                state->streamed_bytes = true;
                                state->cv.notify_one();
                            }
                            return !state->aborted.load() && !state->job->cancelled->load();
                        } catch (...) {
                            state->failed.store(true);
                            return false;
                        }
                    });
                if (!result) {
                    state->failed.store(!state->job->cancelled->load());
                    if (state->job->cancelled->load()) state->aborted.store(true);
                } else {
                    state->duration_ms = result->duration_ms;
                    state->output_audio_seconds = result->output_audio_seconds;
                    std::lock_guard lock(state->mutex);
                    if (!state->streamed_bytes && !result->bytes.empty()) {
                        state->chunks.emplace_back(
                            reinterpret_cast<const char*>(result->bytes.data()),
                            result->bytes.size());
                        state->pending_bytes += result->bytes.size();
                    }
                }
            } catch (...) {
                state->failed.store(true);
            }
            state->finished.store(true);
            state->cv.notify_all();
        });
    } catch (const std::exception& error) {
        write_error(resp, 500, "speech_generation_failed", error.what());
        record_media(deps, model_name, 0, 500, *slot,
                     0.0, 0, observation);
        finish_job(job, "failed");
        return;
    }
    guard.disarm();
    voice_session.keep();
    const std::string content_type = request.format == "wav" ? "audio/wav" :
                                     request.format == "pcm" ? "audio/pcm" :
                                     request.format == "opus" ? "audio/ogg" :
                                     request.format == "aac" ? "audio/aac" :
                                     request.format == "flac" ? "audio/flac" : "audio/mpeg";
    resp.set_chunked_content_provider(
        content_type,
        [state](std::size_t, httplib::DataSink& sink) {
            std::unique_lock lock(state->mutex);
            state->cv.wait_for(lock, std::chrono::seconds(2), [state] {
                return !state->chunks.empty() || state->finished.load() || state->aborted.load();
            });
            if (!state->chunks.empty()) {
                std::string chunk = std::move(state->chunks.front());
                state->pending_bytes -= chunk.size();
                state->chunks.pop_front();
                lock.unlock();
                state->cv.notify_all();
                if (!sink.write(chunk.data(), chunk.size())) {
                    state->finish(499);
                    return false;
                }
                return true;
            }
            if (state->finished.load()) {
                const bool failed = state->failed.load();
                const bool aborted = state->aborted.load();
                lock.unlock();
                state->finish(aborted ? 499 : failed ? 500 : 200);
                sink.done();
                return true;
            }
            return true;
        },
        [state](bool success) {
            state->finish(!success || state->aborted.load()
                              ? 499
                              : state->failed.load() ? 500 : 200);
        });
}
