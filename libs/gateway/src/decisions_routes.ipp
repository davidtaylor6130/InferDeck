namespace {

/// <summary>
/// Text of an OpenAI content value: a string, or an array of input_text and
/// input_json parts. Returns nullopt for anything else (images, files), which
/// local decision models cannot read.
/// </summary>
std::optional<std::string> decision_text(const nlohmann::json& value) {
    if (value.is_string()) return value.get<std::string>();
    if (!value.is_array()) return std::nullopt;
    std::string text;
    for (const auto& part : value) {
        if (!part.is_object()) return std::nullopt;
        const std::string type = part.value("type", "");
        if ((type == "input_text" || type == "text") && part.contains("text") &&
            part["text"].is_string()) {
            text += part["text"].get<std::string>();
        } else if (type == "input_json" && part.contains("data")) {
            text += part["data"].dump(-1, ' ', false,
                                      nlohmann::json::error_handler_t::replace);
        } else {
            return std::nullopt;
        }
    }
    return text;
}

/// <summary>
/// Flattens the request input into the text the model reads. A string passes
/// through; a message array becomes one "role: content" line per message.
/// </summary>
std::optional<std::string> decision_input(const nlohmann::json& input) {
    if (input.is_string()) return input.get<std::string>();
    if (!input.is_array()) return std::nullopt;
    std::string text;
    for (const auto& item : input) {
        if (!item.is_object() || !item.contains("role") || !item["role"].is_string() ||
            !item.contains("content") || item.value("type", "message") != "message") {
            return std::nullopt;
        }
        const auto content = decision_text(item["content"]);
        if (!content) return std::nullopt;
        if (!text.empty()) text += '\n';
        text += item["role"].get<std::string>() + ": " + *content;
    }
    return text;
}

/// <summary>Label a choice value renders as: strings as they are, booleans as true/false.</summary>
std::optional<std::string> choice_label(const nlohmann::json& value) {
    if (value.is_string() && !value.get_ref<const std::string&>().empty()) {
        return value.get<std::string>();
    }
    if (value.is_boolean()) return value.get<bool>() ? "true" : "false";
    return std::nullopt;
}

double choice_confidence(const std::vector<double>& probabilities) {
    const double count = static_cast<double>(probabilities.size());
    const double top = *std::max_element(probabilities.begin(), probabilities.end());
    return std::clamp((count * top - 1.0) / (count - 1.0), 0.0, 1.0);
}

/// <summary>Ordinal confidence: 1 minus the probability spread around the top level, relative to an even spread.</summary>
double score_confidence(const std::vector<double>& probabilities) {
    const std::size_t count = probabilities.size();
    const std::size_t top = static_cast<std::size_t>(
        std::max_element(probabilities.begin(), probabilities.end()) - probabilities.begin());
    double spread = 0.0;
    double even_spread = 0.0;
    for (std::size_t index = 0; index < count; ++index) {
        spread += probabilities[index] *
            std::abs(static_cast<double>(index) - static_cast<double>(top));
        even_spread += std::abs(static_cast<double>(index) -
                                static_cast<double>(count - 1) / 2.0);
    }
    even_spread /= static_cast<double>(count);
    return std::clamp(1.0 - spread / even_spread, 0.0, 1.0);
}

}

void handle_decisions(const httplib::Request& req, httplib::Response& resp,
                      const GatewayDeps& deps) {
    if (!require_json_media_type(req, resp)) return;
    const nlohmann::json body = nlohmann::json::parse(req.body, nullptr, false);
    if (!body.is_object()) {
        write_error(resp, 400, "invalid_json", "request body must be a JSON object");
        return;
    }
    static const std::unordered_set<std::string> supported{
        "model", "input", "questions", "prompt_cache_key",
        "prompt_cache_options", "safety_identifier",
    };
    for (const auto& field : body.items()) {
        if (!supported.contains(field.key())) {
            write_error(resp, 400, "unsupported_parameter",
                        "unsupported Decisions parameter: " + field.key(), field.key());
            return;
        }
    }
    if (!body.contains("model") || !body["model"].is_string() ||
        body["model"].get_ref<const std::string&>().empty()) {
        write_error(resp, 400, "missing_model", "request body must include 'model'", "model");
        return;
    }
    const std::string requested_model = body["model"].get<std::string>();

    model::DecisionRequest request;
    const auto input = body.contains("input")
        ? decision_input(body["input"]) : std::nullopt;
    if (!input) {
        write_error(resp, 400, "invalid_input",
                    "input must be a string or an array of text messages; images and tool items are not supported",
                    "input");
        return;
    }
    request.input = *input;
    if (!body.contains("questions") || !body["questions"].is_array() ||
        body["questions"].empty() || body["questions"].size() > 400) {
        write_error(resp, 400, "invalid_questions",
                    "questions must be an array of 1 to 400 items", "questions");
        return;
    }
    for (const auto& item : body["questions"]) {
        const auto invalid = [&resp](const std::string& message) {
            write_error(resp, 400, "invalid_question", message, "questions");
        };
        if (!item.is_object()) return invalid("every question must be an object");
        const auto instructions = item.contains("instructions")
            ? decision_text(item["instructions"]) : std::nullopt;
        if (!instructions || instructions->empty()) {
            return invalid("question instructions must be non-empty text");
        }
        if (item.contains("name") && !item["name"].is_string() && !item["name"].is_null()) {
            return invalid("question name must be a string");
        }
        model::DecisionQuestion question;
        question.instructions = *instructions;
        const std::string type = item.value("type", "");
        const char* list_key = nullptr;
        if (type == "predicate") {
            question.kind = model::DecisionKind::Predicate;
        } else if (type == "choice") {
            question.kind = model::DecisionKind::Choice;
            list_key = "choices";
        } else if (type == "score") {
            question.kind = model::DecisionKind::Score;
            list_key = "levels";
        } else {
            return invalid("question type must be predicate, choice, or score");
        }
        if (list_key) {
            const std::size_t maximum = type == "choice" ? 255 : 10;
            if (!item.contains(list_key) || !item[list_key].is_array() ||
                item[list_key].size() < 2 || item[list_key].size() > maximum) {
                return invalid(std::string(list_key) + " must have 2 to " +
                               std::to_string(maximum) + " items");
            }
            std::unordered_set<std::string> labels;
            for (const auto& entry : item[list_key]) {
                const auto label = !entry.is_object() ? std::nullopt
                    : type == "choice"
                        ? (entry.contains("value") ? choice_label(entry["value"]) : std::nullopt)
                        : (entry.contains("label") && entry["label"].is_string() &&
                           !entry["label"].get_ref<const std::string&>().empty()
                              ? std::optional<std::string>(entry["label"].get<std::string>())
                              : std::nullopt);
                if (!label || (entry.contains("description") && !entry["description"].is_string())) {
                    return invalid(type == "choice"
                        ? "every choice needs a string or boolean value and an optional string description"
                        : "every level needs a string label and an optional string description");
                }
                if (type == "choice" && !labels.insert(*label).second) {
                    return invalid("choice values must be unique: " + *label);
                }
                question.options.push_back({*label, entry.value("description", "")});
            }
        }
        request.questions.push_back(std::move(question));
    }

    const auto resolved_model = resolve_model_name(deps, requested_model);
    if (!resolved_model) {
        write_error(resp, 404, "model_not_found", resolved_model.error().message);
        return;
    }
    const std::string& model_name = resolved_model->resolved;
    const auto info = deps.coordinator.registry().get_info_result(model_name);
    if (!info || !info->supports("decisions")) {
        write_error(resp, 400, "unsupported_capability",
                    "model does not support decisions: " + requested_model, "model");
        return;
    }
    auto slot = acquire_for_request(req, deps, model_name, 0);
    if (!slot) {
        const int status = slot.error().code == foundation::ErrorCode::NotFound ? 404 : 503;
        write_error(resp, status, "decision_unavailable", slot.error().message);
        return;
    }
    struct SlotGuard {
        model::BackendCoordinator& coordinator;
        std::string model;
        int slot;
        ~SlotGuard() { (void)coordinator.release_slot(model, slot); }
    } guard{deps.coordinator, model_name, *slot};

    const auto result = deps.coordinator.decide(
        model_name, *slot, request, [&req] { return req.is_connection_closed(); });
    const auto observation = observe_request(req, resp, deps, "decision", false);
    if (!result || result->answers.size() != request.questions.size()) {
        const auto code = result ? foundation::ErrorCode::Internal : result.error().code;
        const int status = code == foundation::ErrorCode::InvalidArgument ||
                code == foundation::ErrorCode::ContextLengthExceeded ? 400
            : code == foundation::ErrorCode::Cancelled ? 499 : 500;
        record_request(deps, requested_model, model::InferenceResult{}, status, *slot,
                       0.0, 0, model_name, observation);
        write_error(resp, status,
                    code == foundation::ErrorCode::ContextLengthExceeded
                        ? "context_length_exceeded" : "decision_failed",
                    result ? "decision backend returned the wrong number of answers"
                           : result.error().message);
        return;
    }

    nlohmann::json answers = nlohmann::json::array();
    for (std::size_t index = 0; index < request.questions.size(); ++index) {
        const auto& item = body["questions"][index];
        const auto& probabilities = result->answers[index].probabilities;
        const model::DecisionKind kind = request.questions[index].kind;
        const std::size_t expected =
            kind == model::DecisionKind::Predicate ? 2 : request.questions[index].options.size();
        if (probabilities.size() != expected) {
            write_error(resp, 500, "decision_failed",
                        "decision backend returned the wrong number of probabilities");
            return;
        }
        nlohmann::json answer{
            {"type", item["type"]},
            {"name", item.contains("name") ? item["name"] : nlohmann::json(nullptr)},
        };
        if (kind == model::DecisionKind::Predicate) {
            answer["probability"] = probabilities[1];
        } else {
            const std::size_t top = static_cast<std::size_t>(
                std::max_element(probabilities.begin(), probabilities.end()) - probabilities.begin());
            nlohmann::json listed = nlohmann::json::array();
            double weighted = 0.0;
            for (std::size_t option = 0; option < probabilities.size(); ++option) {
                weighted += static_cast<double>(option) * probabilities[option];
                if (kind == model::DecisionKind::Choice) {
                    listed.push_back({{"value", item["choices"][option]["value"]},
                                      {"probability", probabilities[option]}});
                } else {
                    listed.push_back({{"value", option},
                                      {"label", item["levels"][option]["label"]},
                                      {"probability", probabilities[option]}});
                }
            }
            if (kind == model::DecisionKind::Choice) {
                answer["choice"] = item["choices"][top]["value"];
                answer["confidence"] = choice_confidence(probabilities);
            } else {
                answer["score"] = weighted;
                answer["confidence"] = score_confidence(probabilities);
            }
            answer["probabilities"] = std::move(listed);
        }
        answers.push_back(std::move(answer));
    }
    model::InferenceResult metrics_result;
    metrics_result.prompt_tokens = result->input_tokens;
    metrics_result.duration_ms = result->duration_ms;
    record_request(deps, requested_model, metrics_result, 200, *slot,
                   0.0, 0, model_name, observation);
    write_json(resp, 200, {
        {"model", requested_model},
        {"answers", std::move(answers)},
        {"usage", {
            {"input_tokens", result->input_tokens},
            {"input_tokens_details", {{"cached_tokens", 0}, {"cache_write_tokens", 0}}},
            {"output_tokens", 0},
            {"output_tokens_details", {{"reasoning_tokens", 0}}},
            {"total_tokens", result->input_tokens},
        }},
    });
}
