std::string read_text(const std::filesystem::path& path);

nlohmann::json alias_json(const model::ModelAlias& alias) {
    return {
        {"name", alias.name},
        {"target", alias.target},
        {"requiredContextSize", alias.required_context_size},
        {"requiredCapabilities", alias.required_capabilities},
    };
}

std::string replace_top_level_yaml_section(
    std::string text, const std::string& key, const std::string& value) {
    const std::string marker = key + ":";
    std::size_t start = std::string::npos;
    std::size_t end = text.size();
    std::size_t line_start = 0;
    while (line_start < text.size()) {
        const auto line_end = text.find('\n', line_start);
        const auto length = (line_end == std::string::npos ? text.size() : line_end) - line_start;
        const auto line = text.substr(line_start, length);
        if (start == std::string::npos && line.starts_with(marker)) {
            start = line_start;
        } else if (start != std::string::npos && !line.empty() &&
                   line.front() != ' ' && line.front() != '\t' &&
                   line.front() != '\r') {
            end = line_start;
            break;
        }
        if (line_end == std::string::npos) break;
        line_start = line_end + 1;
    }
    std::string block = marker;
    if (value == "[]") {
        block += " []\n";
    } else {
        block += "\n";
        std::size_t value_start = 0;
        while (value_start <= value.size()) {
            const auto value_end = value.find('\n', value_start);
            block += "  " + value.substr(
                value_start, value_end == std::string::npos
                    ? std::string::npos : value_end - value_start) + "\n";
            if (value_end == std::string::npos) break;
            value_start = value_end + 1;
        }
    }
    if (start == std::string::npos) {
        if (!text.empty() && text.back() != '\n') text += '\n';
        if (!text.empty()) text += '\n';
        text += block;
        return text;
    }
    text.replace(start, end - start, block);
    return text;
}

foundation::Result<std::string> remove_model_registry_entry(
    const std::string& text, const std::string& name) {
    const auto root = YAML::Load(text);
    const auto models = root["model_registry"];
    if (!models || !models.IsSequence()) {
        return foundation::Err<std::string>(
            foundation::ErrorCode::NotFound, "model registry is missing");
    }
    bool configured = false;
    for (const auto& entry : models) {
        if (entry["name"] && entry["name"].as<std::string>() == name) {
            configured = true;
            break;
        }
    }
    if (!configured) {
        return foundation::Err<std::string>(
            foundation::ErrorCode::NotFound, "configured model not found: " + name);
    }

    std::size_t section_start = std::string::npos;
    std::size_t section_end = text.size();
    std::vector<std::size_t> entries;
    std::size_t line_start = 0;
    while (line_start < text.size()) {
        const auto newline = text.find('\n', line_start);
        const auto line_end = newline == std::string::npos ? text.size() : newline;
        const auto line = std::string_view{text}.substr(line_start, line_end - line_start);
        if (section_start == std::string::npos) {
            if (line.starts_with("model_registry:")) section_start = line_start;
        } else {
            if (!line.empty() && line.front() != ' ' && line.front() != '\t' &&
                line.front() != '\r' && line.front() != '#') {
                section_end = line_start;
                break;
            }
            if (line.starts_with("  - ") || line == "  -") entries.push_back(line_start);
        }
        if (newline == std::string::npos) break;
        line_start = newline + 1;
    }
    if (section_start == std::string::npos) {
        return foundation::Err<std::string>(
            foundation::ErrorCode::NotFound, "model registry is missing");
    }

    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto begin = entries[index];
        const auto end = index + 1 < entries.size() ? entries[index + 1] : section_end;
        const auto block = text.substr(begin, end - begin);
        try {
            const auto parsed = YAML::Load("model_registry:\n" + block);
            const auto entry = parsed["model_registry"];
            if (entry && entry.IsSequence() && entry.size() == 1 &&
                entry[0]["name"] && entry[0]["name"].as<std::string>() == name) {
                auto removal_end = end;
                auto comment_start = begin;
                while (comment_start < end) {
                    const auto newline = text.find('\n', comment_start);
                    const auto line_end = newline == std::string::npos ? end : newline;
                    const auto line = std::string_view{text}.substr(
                        comment_start, line_end - comment_start);
                    if (comment_start > begin &&
                        (line.starts_with("#") || line.starts_with("  #"))) {
                        removal_end = comment_start;
                        break;
                    }
                    if (newline == std::string::npos || newline >= end) break;
                    comment_start = newline + 1;
                }
                std::string updated = text;
                updated.erase(begin, removal_end - begin);
                return foundation::Ok(std::move(updated));
            }
        } catch (const std::exception&) {
        }
    }
    return foundation::Err<std::string>(
        foundation::ErrorCode::ParseError,
        "could not locate the configured model block without rewriting the configuration");
}

foundation::Result<std::string> render_aliases(
    const std::string& text, const std::vector<model::ModelAlias>& aliases) {
    try {
        YAML::Load(text);
        YAML::Node entries(YAML::NodeType::Sequence);
        for (const auto& alias : aliases) {
            YAML::Node entry;
            entry["name"] = alias.name;
            entry["target"] = alias.target;
            entry["required_context_size"] = alias.required_context_size;
            for (const auto& capability : alias.required_capabilities) {
                entry["required_capabilities"].push_back(capability);
            }
            entries.push_back(entry);
        }
        YAML::Emitter emitter;
        emitter << entries;
        if (!emitter.good()) {
            return foundation::Err<std::string>(foundation::ErrorCode::ParseError,
                                                emitter.GetLastError());
        }
        return foundation::Ok(replace_top_level_yaml_section(
            text, "model_aliases", emitter.c_str()));
    } catch (const std::exception& error) {
        return foundation::Err<std::string>(
            foundation::ErrorCode::ParseError, error.what());
    }
}
