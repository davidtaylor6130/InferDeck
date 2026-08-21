void handle_audio_transcriptions(const httplib::Request& req, httplib::Response& resp,
                                 const GatewayDeps& deps) {
    const auto observation = observe_request(
        req, resp, deps, "audio_transcription", false);
    if (!req.is_multipart_form_data() || !req.form.has_file("file") || !req.form.has_field("model")) {
        write_error(resp, 400, "invalid_transcription_request",
                    "multipart file and model fields are required",
                    !req.form.has_file("file") ? "file" : "model");
        return;
    }
    const auto& file = req.form.files.find("file")->second;
    if (file.content.empty() || file.content.size() > 25 * 1024 * 1024) {
        write_error(resp, 400, "invalid_audio",
                    "audio must be between 1 byte and 25 MB", "file");
        return;
    }
    if (req.form.get_file_count("file") != 1 ||
        req.form.get_field_count("model") != 1) {
        write_error(resp, 400, "invalid_transcription_request",
                    "file and model must each be supplied exactly once",
                    req.form.get_file_count("file") != 1 ? "file" : "model");
        return;
    }
    static const std::unordered_set<std::string> strict_fields{
        "model", "chunking_strategy", "include", "include[]", "keywords",
        "keywords[]", "known_speaker_names", "known_speaker_names[]",
        "known_speaker_references", "known_speaker_references[]", "language",
        "languages", "languages[]", "prompt", "response_format", "stream",
        "temperature", "timestamp_granularities", "timestamp_granularities[]",
    };
    if (deps.compatibility_profile != CompatibilityProfile::OpenAIDerivative) {
        for (const auto& [name, _] : req.form.fields) {
            if (!strict_fields.contains(name)) {
                write_error(resp, 400, "unsupported_parameter",
                            "unsupported transcription parameter: " + name,
                            name);
                return;
            }
        }
        for (const auto& [name, _] : req.form.files) {
            if (name != "file") {
                write_error(resp, 400, "unsupported_parameter",
                            "unsupported transcription file parameter: " + name,
                            name);
                return;
            }
        }
    }
    const std::string model_name = req.form.get_field("model");
    if (model_name.empty()) {
        write_error(resp, 400, "invalid_transcription_request",
                    "model must not be empty", "model");
        return;
    }
    const auto single_field = [&req, &resp](const std::string& name) {
        if (req.form.get_field_count(name) > 1) {
            write_error(resp, 400, "invalid_transcription_request",
                        name + " must be supplied at most once", name);
            return false;
        }
        return true;
    };
    for (const auto& name : {"language", "prompt", "response_format", "stream",
                             "temperature", "chunking_strategy"}) {
        if (!single_field(name)) return;
    }
    const std::string format = req.form.has_field("response_format") ? req.form.get_field("response_format") : "json";
    if (format != "json" && format != "text" && format != "verbose_json" &&
        format != "srt" && format != "vtt") {
        write_error(resp, 400, "unsupported_response_format",
                    "response_format must be json, text, verbose_json, srt, or vtt",
                    "response_format");
        return;
    }
    if (req.form.has_field("stream")) {
        const auto stream = req.form.get_field("stream");
        if (stream != "true" && stream != "false") {
            write_error(resp, 400, "invalid_transcription_request",
                        "stream must be true or false", "stream");
            return;
        }
        if (stream == "true") {
            write_error(resp, 400, "unsupported_parameter",
                        "streaming transcription is not supported by this native runtime",
                        "stream");
            return;
        }
    }
    for (const auto& name : {"chunking_strategy", "include", "include[]",
                             "keywords", "keywords[]", "known_speaker_names",
                             "known_speaker_names[]", "known_speaker_references",
                             "known_speaker_references[]", "languages",
                             "languages[]"}) {
        if (req.form.has_field(name)) {
            write_error(resp, 400, "unsupported_parameter",
                        std::string{name} +
                            " is not supported by this native transcription runtime",
                        name);
            return;
        }
    }
    std::vector<std::string> granularities =
        req.form.get_fields("timestamp_granularities[]");
    const auto plain_granularities =
        req.form.get_fields("timestamp_granularities");
    granularities.insert(granularities.end(), plain_granularities.begin(),
                         plain_granularities.end());
    if (!granularities.empty()) {
        if (format != "verbose_json") {
            write_error(resp, 400, "invalid_transcription_request",
                        "timestamp_granularities requires verbose_json",
                        "timestamp_granularities");
            return;
        }
        if (granularities.size() != 1 || granularities.front() != "segment") {
            write_error(resp, 400, "unsupported_parameter",
                        "only segment timestamp granularity is supported",
                        "timestamp_granularities");
            return;
        }
    }
    auto parameters = apply_transcription_parameters(
        model::TranscriptionRequest{}, req.form);
    if (!parameters) {
        write_error(resp, 400, "invalid_transcription_request",
                    parameters.error().message);
        return;
    }
    const auto resolved_model = resolve_model_name(deps, model_name);
    if (!resolved_model) {
        write_error(resp, 404, "model_not_found", resolved_model.error().message);
        return;
    }
    const std::string& runtime_model = resolved_model->resolved;
    VoiceSessionGuard voice_session(req, deps);
    auto job = begin_job(model_name, "audio_transcription");
    if (deps.compatibility_profile == CompatibilityProfile::OpenAIDerivative) {
        resp.set_header("X-InferDeck-Job-Id", std::to_string(job->id));
    }
    auto decode_permit = acquire_decode_permit(req, job);
    if (!decode_permit) {
        const int status = status_for(decode_permit.error().code);
        const int internal_status = internal_status_for(decode_permit.error().code);
        write_error(resp, status, "transcription_admission_failed",
                    decode_permit.error().message);
        record_media(deps, model_name, 0, internal_status, -1,
                     0.0, 0, observation);
        finish_job(job, internal_status == 499 ? "cancelled" : "failed");
        return;
    }
    auto slot = acquire_media_slot(req, deps, runtime_model, job);
    if (!slot) {
        const int status = status_for(slot.error().code);
        const int internal_status = internal_status_for(slot.error().code);
        write_error(resp, status, "transcription_admission_failed",
                    slot.error().message);
        record_media(deps, model_name, 0, internal_status, -1,
                     0.0, 0, observation);
        finish_job(job, internal_status == 499 ? "cancelled" : "failed");
        return;
    }
    SlotGuard guard{&deps.coordinator, runtime_model, *slot};
    auto decoded = decode_audio(file.content);
    decode_permit->release();
    if (!decoded) { write_error(resp, 400, "invalid_audio", decoded.error().message); record_media(deps, model_name, 0, 400, *slot, 0.0, 0, observation); finish_job(job, "failed"); return; }
    decoded->language = std::move(parameters->language);
    decoded->prompt = std::move(parameters->prompt);
    decoded->temperature = parameters->temperature;
    const double input_audio_seconds =
        static_cast<double>(decoded->pcm.size()) /
        static_cast<double>(decoded->sample_rate);
    auto result = deps.coordinator.transcribe(runtime_model, *slot, *decoded,
        [&req, &deps, &model_name, job](int progress) {
            update_job(job, progress);
            if (deps.events) deps.events->publish("progress", nlohmann::json{{"id", job->id}, {"model", model_name}, {"modality", "audio_transcription"}, {"progress", progress}}.dump());
            return !req.is_connection_closed() && !job->cancelled->load();
        });
    if (!result) {
        const bool cancelled = job->cancelled->load() ||
            result.error().code == foundation::ErrorCode::Cancelled;
        const int status = cancelled ? 408 : status_for(result.error().code);
        write_error(resp, status, "transcription_failed", result.error().message);
        record_media(deps, model_name, 0, cancelled ? 499 : status, *slot,
                     input_audio_seconds, 0, observation);
        finish_job(job, cancelled ? "cancelled" : "failed");
        return;
    }
    if (format == "text") {
        resp.status = 200;
        resp.set_content(result->text, "text/plain; charset=utf-8");
    } else if (format == "verbose_json") {
        write_json(resp, 200, verbose_transcription(*result, decoded->temperature));
    } else if (format == "srt") {
        resp.status = 200;
        resp.set_content(subtitles(*result, false), "application/x-subrip; charset=utf-8");
    } else if (format == "vtt") {
        resp.status = 200;
        resp.set_content(subtitles(*result, true), "text/vtt; charset=utf-8");
    } else {
        write_json(resp, 200, {{"text", result->text}});
    }
    record_media(deps, model_name, result->inference_ms, 200, *slot,
                 input_audio_seconds, 0, observation);
    finish_job(job, "completed");
    voice_session.refresh_and_keep();
}
