void handle_audio_generations(const httplib::Request& req,
                              httplib::Response& resp,
                              const GatewayDeps& deps) {
    RequestObservation observation = observe_request(
        req, resp, deps, "audio_generation", false);
    observation.protocol_profile = "inferdeck";
    if (!require_json_media_type(req, resp)) return;

    const nlohmann::json body =
        nlohmann::json::parse(req.body, nullptr, false);
    if (body.is_discarded() || !body.is_object()) {
        write_error(resp, 400, "invalid_audio_generation",
                    "request body must be a JSON object");
        return;
    }
    static constexpr std::array<std::string_view, 7> supported_fields{
        "model", "prompt", "lyrics", "duration", "seed", "steps",
        "guidance_scale",
    };
    for (const auto& field : body.items()) {
        if (std::find(supported_fields.begin(), supported_fields.end(),
                      field.key()) == supported_fields.end()) {
            write_error(resp, 400, "unsupported_parameter",
                        "unsupported audio generation parameter: " +
                            field.key(),
                        field.key());
            return;
        }
    }
    if (!body.contains("model") || !body["model"].is_string()) {
        write_error(resp, 400, "invalid_audio_generation",
                    "model must be a string", "model");
        return;
    }
    if (!body.contains("prompt") || !body["prompt"].is_string()) {
        write_error(resp, 400, "invalid_audio_generation",
                    "prompt must be a string", "prompt");
        return;
    }
    if (body.contains("lyrics") && !body["lyrics"].is_string()) {
        write_error(resp, 400, "invalid_audio_generation",
                    "lyrics must be a string", "lyrics");
        return;
    }
    if (body.contains("duration") && !body["duration"].is_number()) {
        write_error(resp, 400, "invalid_audio_generation",
                    "duration must be a number", "duration");
        return;
    }
    if (body.contains("seed") && !body["seed"].is_number_integer()) {
        write_error(resp, 400, "invalid_audio_generation",
                    "seed must be an integer", "seed");
        return;
    }
    if (body.contains("steps") && !body["steps"].is_number_integer()) {
        write_error(resp, 400, "invalid_audio_generation",
                    "steps must be an integer", "steps");
        return;
    }
    if (body.contains("guidance_scale") &&
        !body["guidance_scale"].is_number()) {
        write_error(resp, 400, "invalid_audio_generation",
                    "guidance_scale must be a number", "guidance_scale");
        return;
    }

    model::AudioGenerationRequest request;
    std::string model_name;
    try {
        model_name = body["model"].get<std::string>();
        request.prompt = body["prompt"].get<std::string>();
        request.lyrics = body.value("lyrics", "");
        request.duration_seconds = body.value("duration", 30.0f);
        request.seed = body.value("seed", std::int64_t{-1});
        request.steps = body.value("steps", 0);
        request.guidance_scale = body.value("guidance_scale", 0.0f);
    } catch (...) {
        write_error(resp, 400, "invalid_audio_generation",
                    "audio generation parameters are out of range");
        return;
    }
    if (model_name.empty() || model_name.size() > 256 ||
        request.prompt.empty() || request.prompt.size() > 4096 ||
        request.lyrics.size() > 32768 ||
        !std::isfinite(request.duration_seconds) ||
        request.duration_seconds < 10.0f ||
        request.duration_seconds > 600.0f ||
        request.seed < -1 ||
        request.seed > static_cast<std::int64_t>(
            std::numeric_limits<std::uint32_t>::max()) ||
        request.steps < 0 || request.steps > 100 ||
        !std::isfinite(request.guidance_scale) ||
        request.guidance_scale < 0.0f ||
        request.guidance_scale > 50.0f) {
        write_error(
            resp, 400, "invalid_audio_generation",
            "model, prompt, lyrics, duration, seed, steps, or guidance_scale is invalid");
        return;
    }

    const foundation::Result<ResolvedModelName> resolved_model =
        resolve_model_name(deps, model_name);
    if (!resolved_model) {
        write_error(resp, 404, "model_not_found",
                    resolved_model.error().message);
        return;
    }
    const foundation::Result<model::ModelInfo> info =
        deps.coordinator.registry().get_info_result(
            resolved_model->resolved);
    if (!info || !info->supports("audio_generation")) {
        write_error(resp, 400, "unsupported_audio_model",
                    "model does not support audio generation", "model");
        return;
    }

    const std::shared_ptr<MediaJob> job =
        begin_job(
            model_name, "audio_generation", request.prompt,
            nlohmann::json{
                {"duration_seconds", request.duration_seconds},
                {"seed", request.seed},
                {"steps", request.steps},
                {"guidance_scale", request.guidance_scale},
                {"has_lyrics", !request.lyrics.empty()},
            });
    resp.set_header("X-InferDeck-Job-Id", std::to_string(job->id));
    const std::string& runtime_model = resolved_model->resolved;
    const foundation::Result<int> slot =
        acquire_media_slot(req, deps, runtime_model, job);
    if (!slot) {
        const int status = status_for(slot.error().code);
        const int internal_status =
            internal_status_for(slot.error().code);
        write_error(resp, status, "audio_generation_admission_failed",
                    slot.error().message);
        record_media(deps, model_name, 0.0f, internal_status, -1,
                     0.0, 0, observation);
        finish_job(
            job, internal_status == 499 ? "cancelled" : "failed",
            slot.error().message);
        return;
    }

    SlotGuard guard{&deps.coordinator, runtime_model, *slot};
    const foundation::Result<model::AudioGenerationResult> result =
        deps.coordinator.generate_audio(
            runtime_model, *slot, request,
            [&req, &deps, &model_name, job](int progress) {
                if (update_job(job, progress) && deps.events) {
                    deps.events->publish(
                        "progress",
                        nlohmann::json{
                            {"id", job->id},
                            {"model", model_name},
                            {"modality", "audio_generation"},
                            {"progress", progress},
                        }.dump());
                }
                return !req.is_connection_closed() &&
                    !job->cancelled->load();
            });
    if (!result) {
        const bool cancelled =
            job->cancelled->load() ||
            result.error().code == foundation::ErrorCode::Cancelled;
        const int status =
            cancelled ? 408 : status_for(result.error().code);
        const int internal_status = cancelled ? 499 : status;
        write_error(resp, status, "audio_generation_failed",
                    result.error().message);
        record_media(deps, model_name, 0.0f, internal_status, *slot,
                     0.0, 0, observation);
        finish_job(
            job, cancelled ? "cancelled" : "failed",
            result.error().message);
        return;
    }

    const std::vector<PendingMediaOutput> history_outputs{
        PendingMediaOutput{
            "audio/wav", ".wav", &result->wav_bytes},
    };
    const foundation::Result<void> stored =
        store_job_outputs(job, history_outputs);
    if (!stored) {
        foundation::LOG_WARN(
            "media_output_store_failed", "job_id={} error={}",
            job->id, stored.error().message);
    }
    resp.set_header("X-InferDeck-Seed", std::to_string(result->seed));
    resp.set_header("X-InferDeck-Audio-Duration-Seconds",
                    std::to_string(result->output_audio_seconds));
    resp.set_content(
        std::string(
            reinterpret_cast<const char*>(result->wav_bytes.data()),
            result->wav_bytes.size()),
        "audio/wav");
    resp.status = 200;
    RequestObservation completed_observation = observation;
    completed_observation.output_audio_seconds =
        result->output_audio_seconds;
    record_media(
        deps, model_name, result->duration_ms, 200, *slot, 0.0,
        utf8_character_count(request.prompt) +
            utf8_character_count(request.lyrics),
        completed_observation);
    finish_job(job, "completed");
}
