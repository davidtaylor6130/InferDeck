void handle_image_generations(const httplib::Request& req, httplib::Response& resp,
                              const GatewayDeps& deps) {
    const auto observation = observe_request(
        req, resp, deps, "image_generation", false);
    if (!require_json_media_type(req, resp)) return;
    nlohmann::json body;
    try { body = nlohmann::json::parse(req.body); }
    catch (const std::exception& error) { write_error(resp, 400, "invalid_json", error.what()); return; }
    const bool derivative =
        deps.compatibility_profile == CompatibilityProfile::OpenAIDerivative;
    if (!derivative) {
        static constexpr std::array<std::string_view, 14> supported_fields{
            "model", "prompt", "background", "moderation", "n",
            "output_compression", "output_format", "partial_images",
            "quality", "response_format", "size", "stream", "style", "user",
        };
        if (!body.is_object()) {
            write_error(resp, 400, "invalid_request_error",
                        "request body must be an object");
            return;
        }
        for (const auto& field : body.items()) {
            if (std::find(supported_fields.begin(), supported_fields.end(),
                          field.key()) == supported_fields.end()) {
                write_error(resp, 400, "unsupported_parameter",
                            "unsupported Images parameter: " +
                                field.key(),
                            field.key());
                return;
            }
        }
        const auto compatible_default =
            [&body, &resp](std::string_view field, std::string_view value) {
            if (!body.contains(field) || body[field].is_null()) return true;
            if (!body[field].is_string() ||
                body[field].get<std::string>() != value) {
                write_error(resp, 400, "unsupported_parameter",
                            std::string(field) + " is not supported by the native image runtime",
                            std::string(field));
                return false;
            }
            return true;
        };
        if (!compatible_default("background", "auto") ||
            !compatible_default("moderation", "auto") ||
            !compatible_default("output_format", "png") ||
            !compatible_default("quality", "auto") ||
            !compatible_default("response_format", "b64_json")) {
            return;
        }
        if (body.contains("style") && !body["style"].is_null()) {
            write_error(resp, 400, "unsupported_parameter",
                        "style is not supported by the native image runtime",
                        "style");
            return;
        }
        if (body.contains("output_compression") &&
            !body["output_compression"].is_null()) {
            write_error(resp, 400, "unsupported_parameter",
                        "output_compression is not supported for PNG output",
                        "output_compression");
            return;
        }
        if (body.contains("stream") && !body["stream"].is_null() &&
            (!body["stream"].is_boolean() || body["stream"].get<bool>())) {
            write_error(resp, 400, "unsupported_parameter",
                        "streaming image generation is not supported",
                        "stream");
            return;
        }
        if (body.contains("partial_images") &&
            !body["partial_images"].is_null() &&
            (!body["partial_images"].is_number_integer() ||
             body["partial_images"].get<int>() != 0)) {
            write_error(resp, 400, "unsupported_parameter",
                        "partial_images requires unsupported image streaming",
                        "partial_images");
            return;
        }
        if (body.contains("user") &&
            (!body["user"].is_string() ||
             body["user"].get_ref<const std::string&>().size() > 64)) {
            write_error(resp, 400, "invalid_user",
                        "user must be a string of at most 64 characters",
                        "user");
            return;
        }
    }
    if (!body.is_object()) {
        write_error(resp, 400, "invalid_image_request",
                    "request body must be an object");
        return;
    }
    if (!body.contains("prompt") || !body["prompt"].is_string()) {
        write_error(resp, 400, "invalid_image_request",
                    "prompt must be a string", "prompt");
        return;
    }
    if (body.contains("model") && !body["model"].is_null() &&
        !body["model"].is_string()) {
        write_error(resp, 400, "invalid_image_request",
                    "model must be a string", "model");
        return;
    }
    if (body.contains("n") && !body["n"].is_null() &&
        !body["n"].is_number_integer()) {
        write_error(resp, 400, "invalid_image_request",
                    "n must be an integer", "n");
        return;
    }
    if (body.contains("size") && !body["size"].is_null() &&
        !body["size"].is_string()) {
        write_error(resp, 400, "invalid_image_request",
                    "size must be a string", "size");
        return;
    }
    const std::string model_name =
        body.contains("model") && !body["model"].is_null()
            ? body["model"].get<std::string>() : deps.default_model;
    model::ImageGenerationRequest request;
    request.prompt = body["prompt"].get<std::string>();
    request.negative_prompt = body.value("negative_prompt", "");
    request.count = body.contains("n") && !body["n"].is_null()
        ? body["n"].get<int>() : 1;
    request.seed = body.value("seed", std::int64_t{-1});
    request.steps = body.value("steps", 20);
    request.guidance_scale = body.value("guidance_scale", 7.0f);
    const std::string size =
        body.contains("size") && !body["size"].is_null()
            ? body["size"].get<std::string>() : "1024x1024";
    char trailing = '\0';
    if (std::sscanf(size.c_str(), "%dx%d%c", &request.width, &request.height, &trailing) != 2 ||
        model_name.empty() || request.prompt.empty() || request.prompt.size() > 32000 ||
        request.negative_prompt.size() > 32768 || request.count < 1 || request.count > 4 ||
        request.width < 256 || request.height < 256 || request.width > 2048 || request.height > 2048 ||
        request.width % 64 != 0 || request.height % 64 != 0 || request.steps < 1 || request.steps > 200 ||
        !std::isfinite(request.guidance_scale) || request.guidance_scale < 0.0f || request.guidance_scale > 50.0f) {
        write_error(resp, 400, "invalid_image_request", "invalid model, prompt, size, count, seed, steps, or guidance scale");
        return;
    }
    const auto resolved_model = resolve_model_name(deps, model_name);
    if (!resolved_model) {
        write_error(resp, 404, "model_not_found", resolved_model.error().message);
        return;
    }
    auto job = begin_job(model_name, "image");
    if (derivative) {
        resp.set_header("X-InferDeck-Job-Id", std::to_string(job->id));
    }
    const std::string& runtime_model = resolved_model->resolved;
    auto slot = acquire_media_slot(req, deps, runtime_model, job);
    if (!slot) {
        const int status = status_for(slot.error().code);
        const int internal_status = internal_status_for(slot.error().code);
        write_error(resp, status, "image_admission_failed", slot.error().message);
        record_media(deps, model_name, 0, internal_status, -1,
                     0.0, 0, observation);
        finish_job(job, internal_status == 499 ? "cancelled" : "failed");
        return;
    }
    SlotGuard guard{&deps.coordinator, runtime_model, *slot};
    auto result = deps.coordinator.generate_images(runtime_model, *slot, request,
        [&req, &deps, &model_name, job](int progress) {
            update_job(job, progress);
            if (deps.events) deps.events->publish("progress", nlohmann::json{{"id", job->id}, {"model", model_name}, {"modality", "image"}, {"progress", progress}}.dump());
            return !req.is_connection_closed() && !job->cancelled->load();
        });
    if (!result) {
        const bool cancelled = job->cancelled->load() ||
            result.error().code == foundation::ErrorCode::Cancelled;
        const int status = cancelled ? 408 : status_for(result.error().code);
        const int internal_status = cancelled ? 499 : status;
        write_error(resp, status, "image_generation_failed", result.error().message);
        record_media(deps, model_name, 0, internal_status, *slot,
                     0.0, 0, observation);
        finish_job(job, cancelled ? "cancelled" : "failed");
        return;
    }
    nlohmann::json data = nlohmann::json::array();
    for (const auto& image : result->png_images) data.push_back({{"b64_json", base64(image)}});
    write_json(resp, 200, {{"created", std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::system_clock::now().time_since_epoch()).count()},
                           {"output_format", "png"},
                           {"data", std::move(data)}});
    auto completed_observation = observation;
    completed_observation.output_image_count =
        static_cast<int>(result->png_images.size());
    record_media(deps, model_name, result->duration_ms, 200, *slot,
                 0.0, 0, completed_observation);
    finish_job(job, "completed");
}
