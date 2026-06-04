#include "foundation/json_utils.hpp"

#include <fstream>
#include <sstream>

namespace inferdeck::foundation {

Result<Json> parse_json(std::string_view text) {
    if (text.empty()) {
        return Err<Json>(ErrorCode::InvalidArgument, "empty json input");
    }
    try {
        return Ok(Json::parse(text));
    } catch (const Json::parse_error& e) {
        return Err<Json>(ErrorCode::ParseError,
                         std::string("json parse error: ") + e.what());
    }
}

Result<Json> load_json_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return Err<Json>(ErrorCode::IoError,
                         std::string("failed to open file: ") + path.string());
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return parse_json(ss.str());
}

Result<void> save_json_file(const std::filesystem::path& path,
                            const Json& value,
                            bool pretty) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return Err<void>(ErrorCode::IoError,
                         std::string("failed to open file for write: ") + path.string());
    }
    if (pretty) {
        out << value.dump(2) << '\n';
    } else {
        out << value.dump() << '\n';
    }
    if (!out.good()) {
        return Err<void>(ErrorCode::IoError,
                         std::string("write failed: ") + path.string());
    }
    return Ok();
}

std::string dump_json(const Json& value, bool pretty) {
    return pretty ? value.dump(2) : value.dump();
}

} // namespace inferdeck::foundation
