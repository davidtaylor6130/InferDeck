#include "foundation/json_utils.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <sstream>
#include <system_error>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <unistd.h>
#endif

namespace inferdeck::foundation {
namespace {

std::uint64_t process_id() {
#ifdef _WIN32
    return static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

std::filesystem::path unique_temporary_path(const std::filesystem::path& path) {
    static std::atomic_uint64_t sequence{0};
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    auto temporary = path;
    temporary += ".tmp." + std::to_string(process_id()) + "." +
                 std::to_string(tick) + "." +
                 std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
    return temporary;
}

class TemporaryFile {
public:
    explicit TemporaryFile(std::filesystem::path path) : path_(std::move(path)) {}
    ~TemporaryFile() {
        if (active_) {
            std::error_code ignored;
            std::filesystem::remove(path_, ignored);
        }
    }

    const std::filesystem::path& path() const noexcept { return path_; }
    void release() noexcept { active_ = false; }

private:
    std::filesystem::path path_;
    bool active_{true};
};

Result<void> replace_file(const std::filesystem::path& source,
                          const std::filesystem::path& destination) {
    static std::mutex replacement_mutex;
    std::lock_guard lock(replacement_mutex);
#ifdef _WIN32
    DWORD error = ERROR_SUCCESS;
    for (DWORD attempt = 0; attempt < 5; ++attempt) {
        if (::MoveFileExW(source.c_str(),
                          destination.c_str(),
                          MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            return Ok();
        }
        error = ::GetLastError();
        if (error != ERROR_ACCESS_DENIED &&
            error != ERROR_SHARING_VIOLATION &&
            error != ERROR_LOCK_VIOLATION) {
            break;
        }
        ::Sleep(1U << attempt);
    }
    return Err<void>(
        ErrorCode::IoError,
        "failed to replace file: " + destination.string() +
            " (win32 error " + std::to_string(error) + ")");
#else
    std::error_code error;
    std::filesystem::rename(source, destination, error);
    if (error) {
        return Err<void>(ErrorCode::IoError,
                         "failed to replace file: " + destination.string() +
                             " (" + error.message() + ")");
    }
#endif
    return Ok();
}

}

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
    std::string serialized;
    try {
        serialized = pretty ? value.dump(2) : value.dump();
        serialized.push_back('\n');
    } catch (const std::exception& error) {
        return Err<void>(ErrorCode::InvalidArgument,
                         std::string("failed to serialize json: ") + error.what());
    }

    TemporaryFile temporary(unique_temporary_path(path));
    std::ofstream out(temporary.path(), std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return Err<void>(ErrorCode::IoError,
                         std::string("failed to open temporary file for write: ") +
                             temporary.path().string());
    }
    out.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
    out.flush();
    if (!out.good()) {
        return Err<void>(ErrorCode::IoError,
                         std::string("write failed: ") + temporary.path().string());
    }
    out.close();
    if (out.fail()) {
        return Err<void>(ErrorCode::IoError,
                         std::string("close failed: ") + temporary.path().string());
    }

    auto replaced = replace_file(temporary.path(), path);
    if (!replaced) return replaced;
    temporary.release();
    return Ok();
}

std::string dump_json(const Json& value, bool pretty) {
    return pretty ? value.dump(2) : value.dump();
}

} // namespace inferdeck::foundation
