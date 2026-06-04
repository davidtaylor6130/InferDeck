#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "foundation/result.hpp"

namespace inferdeck::foundation {

using Json = nlohmann::json;

Result<Json> parse_json(std::string_view text);
Result<Json> load_json_file(const std::filesystem::path& path);
Result<void> save_json_file(const std::filesystem::path& path, const Json& value, bool pretty = true);
std::string dump_json(const Json& value, bool pretty = false);

} // namespace inferdeck::foundation
