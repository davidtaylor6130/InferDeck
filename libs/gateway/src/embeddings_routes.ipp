void handle_embeddings(const httplib::Request& req, httplib::Response& resp,
                       const GatewayDeps& deps) {
    if (!require_json_media_type(req, resp)) return;
    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req.body);
    } catch (const std::exception& error) {
        write_error(resp, 400, "invalid_json", error.what());
        return;
    }
    if (!body.is_object()) {
        write_error(resp, 400, "invalid_request_error",
                    "request body must be an object");
        return;
    }
    if (deps.compatibility_profile == CompatibilityProfile::StrictOpenAI) {
        static const std::unordered_set<std::string> supported{
            "model", "input", "encoding_format", "dimensions", "user",
        };
        for (const auto& field : body.items()) {
            if (!supported.contains(field.key())) {
                write_error(resp, 400, "unsupported_parameter",
                            "unsupported Embeddings parameter: " + field.key(),
                            field.key());
                return;
            }
        }
    }
    if (!body.contains("model") || !body["model"].is_string() ||
        body["model"].get<std::string>().empty()) {
        write_error(resp, 400, "missing_model",
                    "request body must include 'model'", "model");
        return;
    }
    const std::string requested_model = body["model"].get<std::string>();

    model::EmbeddingRequest embedding_request;
    const auto parse_token_array =
        [&resp](const nlohmann::json& value)
            -> std::optional<model::EmbeddingTokenInput> {
        if (!value.is_array() || value.empty()) {
            write_error(resp, 400, "invalid_input",
                        "token input must be a non-empty array", "input");
            return std::nullopt;
        }
        model::EmbeddingTokenInput token_input;
        token_input.tokens.reserve(value.size());
        for (const auto& token : value) {
            if (!(token.is_number_integer() || token.is_number_unsigned())) {
                write_error(resp, 400, "invalid_input",
                            "every token ID must be an integer", "input");
                return std::nullopt;
            }
            const bool out_of_range = token.is_number_unsigned()
                ? token.get<std::uint64_t>() >
                    static_cast<std::uint64_t>(
                        std::numeric_limits<std::int32_t>::max())
                : token.get<std::int64_t>() < 0 ||
                    token.get<std::int64_t>() >
                        std::numeric_limits<std::int32_t>::max();
            if (out_of_range) {
                write_error(resp, 400, "invalid_input",
                            "token IDs must be non-negative 32-bit integers",
                            "input");
                return std::nullopt;
            }
            token_input.tokens.push_back(token.get<std::int32_t>());
        }
        return token_input;
    };
    if (!body.contains("input")) {
        write_error(resp, 400, "invalid_input",
                    "request body must include 'input'", "input");
        return;
    }
    const auto& input = body["input"];
    if (input.is_string()) {
        embedding_request.inputs.push_back(
            model::EmbeddingTextInput{input.get<std::string>()});
    } else if (input.is_array()) {
        if (input.empty() || input.size() > 256) {
            write_error(resp, 400, "invalid_input",
                        "input must contain 1 to 256 items", "input");
            return;
        }
        if (input.front().is_string()) {
            for (const auto& item : input) {
                if (!item.is_string()) {
                    write_error(resp, 400, "invalid_input",
                                "input arrays cannot mix strings and token IDs",
                                "input");
                    return;
                }
                embedding_request.inputs.push_back(
                    model::EmbeddingTextInput{item.get<std::string>()});
            }
        } else if (input.front().is_array()) {
            for (const auto& item : input) {
                if (!item.is_array()) {
                    write_error(resp, 400, "invalid_input",
                                "input arrays cannot mix token IDs and token arrays",
                                "input");
                    return;
                }
                auto parsed = parse_token_array(item);
                if (!parsed) return;
                embedding_request.inputs.push_back(std::move(*parsed));
            }
        } else {
            auto parsed = parse_token_array(input);
            if (!parsed) return;
            embedding_request.inputs.push_back(std::move(*parsed));
        }
    } else {
        write_error(resp, 400, "invalid_input",
                    "input must be a string, string array, token array, or token matrix",
                    "input");
        return;
    }
    std::size_t total_size = 0;
    for (const auto& item : embedding_request.inputs) {
        const bool valid = std::visit([&total_size](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            const std::size_t size = [&] {
                if constexpr (std::is_same_v<T, model::EmbeddingTextInput>) {
                    return value.text.size();
                } else {
                    return value.tokens.size();
                }
            }();
            total_size += size;
            return size > 0 && size <= 1024 * 1024 &&
                   total_size <= 4 * 1024 * 1024;
        }, item);
        if (!valid) {
            write_error(resp, 400, "invalid_input",
                        "embedding input is empty or too large", "input");
            return;
        }
    }
    if (body.contains("dimensions")) {
        if (!body["dimensions"].is_number_integer() ||
            body["dimensions"].get<int>() <= 0 ||
            body["dimensions"].get<int>() > 65536) {
            write_error(resp, 400, "invalid_dimensions",
                        "dimensions must be a positive integer", "dimensions");
            return;
        }
        embedding_request.dimensions = body["dimensions"].get<int>();
    }
    if (body.contains("user") &&
        (!body["user"].is_string() ||
         body["user"].get<std::string>().size() > 64)) {
        write_error(resp, 400, "invalid_user",
                    "user must be a string of at most 64 characters", "user");
        return;
    }
    const std::string encoding = body.value("encoding_format", "float");
    if (encoding != "float" && encoding != "base64") {
        write_error(resp, 400, "unsupported_encoding",
                    "encoding_format must be float or base64",
                    "encoding_format");
        return;
    }

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
    if (!info->supports("embeddings")) {
        write_error(resp, 400, "unsupported_capability",
                    "model does not support embeddings: " + model_name);
        return;
    }
    const int priority = body.contains("priority") && body["priority"].is_number_integer()
        ? body["priority"].get<int>() : 0;
    auto slot = acquire_for_request(req, deps, model_name, priority);
    if (!slot) {
        const int status = slot.error().code == foundation::ErrorCode::NotFound ? 404 : 503;
        write_error(resp, status, "embedding_unavailable", slot.error().message);
        return;
    }
    struct SlotGuard {
        model::BackendCoordinator& coordinator;
        std::string model;
        int slot;
        ~SlotGuard() { (void)coordinator.release_slot(model, slot); }
    } guard{deps.coordinator, model_name, *slot};

    auto result = deps.coordinator.embed(model_name, *slot, embedding_request,
        [&req] { return req.is_connection_closed(); });
    if (!result) {
        const int status = result.error().code == foundation::ErrorCode::InvalidArgument ? 400
            : result.error().code == foundation::ErrorCode::Cancelled ? 499 : 500;
        record_request(deps, requested_model, model::InferenceResult{}, status, *slot,
                       0.0, 0, model_name,
                       observe_request(req, resp, deps, "embedding", false));
        write_error(resp, status, "embedding_failed", result.error().message);
        return;
    }
    nlohmann::json data = nlohmann::json::array();
    for (std::size_t index = 0; index < result->embeddings.size(); ++index) {
        data.push_back({
            {"object", "embedding"},
            {"index", index},
            {"embedding", encoding == "base64"
                ? nlohmann::json(base64_floats(result->embeddings[index]))
                : nlohmann::json(result->embeddings[index])},
        });
    }
    model::InferenceResult metrics_result;
    metrics_result.prompt_tokens = result->prompt_tokens;
    metrics_result.duration_ms = result->duration_ms;
    record_request(deps, requested_model, metrics_result, 200, *slot,
                   0.0, 0, model_name,
                   observe_request(req, resp, deps, "embedding", false));
    write_json(resp, 200, {
        {"object", "list"},
        {"data", data},
        {"model", requested_model},
        {"usage", {
            {"prompt_tokens", result->prompt_tokens},
            {"total_tokens", result->prompt_tokens},
        }},
    });
}
