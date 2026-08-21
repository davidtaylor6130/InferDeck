#include "gateway/config_secrets.hpp"

#include <array>
#include <optional>
#include <string_view>
#include <utility>

#include <yaml-cpp/yaml.h>

namespace inferdeck::gateway {
namespace {

struct ScalarSpan {
    std::size_t begin{0};
    std::size_t end{0};
};

std::optional<ScalarSpan> yaml_scalar_span(
    const std::string& text, const std::string& section,
    const std::string& key) {
    bool in_section = false;
    std::size_t position = 0;
    while (position < text.size()) {
        const auto newline = text.find('\n', position);
        const auto line_end = newline == std::string::npos ? text.size() : newline;
        std::string_view line(text.data() + position, line_end - position);
        const auto first = line.find_first_not_of(" \t\r");
        if (first != std::string_view::npos && line[first] != '#') {
            const auto colon = line.find(':', first);
            if (colon != std::string_view::npos) {
                const std::string name(line.substr(first, colon - first));
                if (first == 0) {
                    in_section = name == section;
                } else if (in_section && name == key) {
                    auto value_begin = colon + 1;
                    while (value_begin < line.size() &&
                           (line[value_begin] == ' ' || line[value_begin] == '\t')) {
                        ++value_begin;
                    }
                    auto value_end = line.size();
                    bool single = false;
                    bool double_quote = false;
                    for (std::size_t i = value_begin; i < line.size(); ++i) {
                        if (line[i] == '\'' && !double_quote) single = !single;
                        if (line[i] == '"' && !single &&
                            (i == 0 || line[i - 1] != '\\')) {
                            double_quote = !double_quote;
                        }
                        if (line[i] == '#' && !single && !double_quote &&
                            (i == value_begin || line[i - 1] == ' ' ||
                             line[i - 1] == '\t')) {
                            value_end = i;
                            break;
                        }
                    }
                    while (value_end > value_begin &&
                           (line[value_end - 1] == ' ' ||
                            line[value_end - 1] == '\t' ||
                            line[value_end - 1] == '\r')) {
                        --value_end;
                    }
                    return ScalarSpan{position + value_begin,
                                      position + value_end};
                }
            }
        }
        if (newline == std::string::npos) break;
        position = newline + 1;
    }
    return std::nullopt;
}

bool secret_field(std::string_view section, std::string_view key) {
    return (section == "auth" && key == "token") ||
        (section == "control" && key == "token") ||
        (section == "model_store" && key == "hf_token");
}

bool secret_nodes_are_masked(const std::string& text) {
    try {
        const auto root = YAML::Load(text);
        if (!root || !root.IsMap()) return true;
        for (const auto& section : root) {
            if (!section.first.IsScalar() || !section.second.IsMap()) continue;
            const auto section_name = section.first.as<std::string>();
            for (const auto& field : section.second) {
                if (!field.first.IsScalar()) continue;
                const auto key = field.first.as<std::string>();
                if (!secret_field(section_name, key)) continue;
                if (!field.second.IsScalar() ||
                    field.second.as<std::string>() != "__INFERDECK_SECRET__") {
                    return false;
                }
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

std::string serialize_masked_config(const std::string& text) {
    try {
        auto root = YAML::Load(text);
        if (!root || !root.IsMap()) return {};
        for (auto section : root) {
            if (!section.first.IsScalar() || !section.second.IsMap()) continue;
            const auto section_name = section.first.as<std::string>();
            for (auto field : section.second) {
                if (!field.first.IsScalar()) continue;
                if (secret_field(section_name, field.first.as<std::string>())) {
                    field.second = "__INFERDECK_SECRET__";
                }
            }
        }
        return YAML::Dump(root);
    } catch (...) {
        return {};
    }
}

}

std::string mask_config_secrets(std::string text) {
    for (const auto& [section, key] : std::array{
             std::pair{"auth", "token"}, std::pair{"control", "token"},
             std::pair{"model_store", "hf_token"}}) {
        if (auto span = yaml_scalar_span(text, section, key)) {
            text.replace(span->begin, span->end - span->begin,
                         "\"__INFERDECK_SECRET__\"");
        }
    }
    if (secret_nodes_are_masked(text)) return text;
    return serialize_masked_config(text);
}

std::string restore_config_secrets(
    std::string submitted, const std::string& current) {
    const auto original_submitted = submitted;
    for (const auto& [section, key] : std::array{
             std::pair{"auth", "token"}, std::pair{"control", "token"},
             std::pair{"model_store", "hf_token"}}) {
        const auto submitted_span = yaml_scalar_span(submitted, section, key);
        const auto current_span = yaml_scalar_span(current, section, key);
        if (!submitted_span || !current_span) continue;
        const auto value = submitted.substr(
            submitted_span->begin, submitted_span->end - submitted_span->begin);
        if (value == "__INFERDECK_SECRET__" ||
            value == "\"__INFERDECK_SECRET__\"" ||
            value == "'__INFERDECK_SECRET__'") {
            submitted.replace(
                submitted_span->begin, submitted_span->end - submitted_span->begin,
                current.substr(current_span->begin,
                               current_span->end - current_span->begin));
        }
    }
    if (submitted.find("__INFERDECK_SECRET__") == std::string::npos) {
        return submitted;
    }
    try {
        auto submitted_root = YAML::Load(original_submitted);
        const auto current_root = YAML::Load(current);
        if (!submitted_root || !submitted_root.IsMap() ||
            !current_root || !current_root.IsMap()) {
            return submitted;
        }
        for (auto section : submitted_root) {
            if (!section.first.IsScalar() || !section.second.IsMap()) continue;
            const auto section_name = section.first.as<std::string>();
            for (auto field : section.second) {
                if (!field.first.IsScalar() || !field.second.IsScalar()) continue;
                const auto key = field.first.as<std::string>();
                if (!secret_field(section_name, key) ||
                    field.second.as<std::string>() != "__INFERDECK_SECRET__") {
                    continue;
                }
                const auto replacement = current_root[section_name][key];
                if (replacement) field.second = replacement.as<std::string>();
            }
        }
        return YAML::Dump(submitted_root);
    } catch (...) {
        return submitted;
    }
}

}
