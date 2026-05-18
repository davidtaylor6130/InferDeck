/// @file ConfigLoader.hpp
/// @brief Gateway configuration loader.

#pragma once

#include "Server.hpp"
#include <string>
#include <filesystem>

namespace inferdeck::gateway {

/// Load gateway configuration from YAML file.
/// @param config_path Path to gateway.yml
/// @return Loaded ServerConfig
ServerConfig LoadConfig(const std::filesystem::path& config_path);

/// Get the default config path.
/// @return Default gateway.yml path
std::filesystem::path GetDefaultConfigPath();

} // namespace inferdeck::gateway
