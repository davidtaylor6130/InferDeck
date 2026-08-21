#pragma once

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "config_decode.hpp"

namespace inferdeck::gateway {

inline std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) return {};
    std::ostringstream output;
    output << input.rdbuf();
    return output.str();
}

inline std::filesystem::path default_config_path() {
    namespace fs = std::filesystem;
    fs::path candidates[] = {
        fs::current_path() / "config" / "gateway.yml",
        fs::current_path() / "gateway.yml",
    };
    for (const auto& candidate : candidates) {
        if (fs::exists(candidate)) return candidate;
    }
    return candidates[0];
}

inline std::filesystem::path active_config_path_for(
    const std::filesystem::path& base_path) {
    const auto extension = base_path.extension().string();
    return base_path.parent_path() /
        (base_path.stem().string() + ".active" + extension);
}

struct ConfigLoadSelection {
    GatewayConfig config;
    std::filesystem::path loaded_path;
    std::filesystem::path active_path;
    bool using_active{false};
    std::string fallback_reason;
};

inline ConfigLoadSelection load_config_with_active(
    const std::filesystem::path& base_path) {
    ConfigLoadSelection selection;
    selection.active_path = active_config_path_for(base_path);
    if (std::filesystem::exists(selection.active_path)) {
        try {
            selection.config = load_config(selection.active_path);
            selection.loaded_path = selection.active_path;
            selection.using_active = true;
            return selection;
        } catch (const std::exception& error) {
            selection.fallback_reason = error.what();
        }
    }
    selection.config = load_config(base_path);
    selection.loaded_path = base_path;
    return selection;
}

}
