#include "gateway/config_repository.hpp"

#include <fstream>
#include <iomanip>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace inferdeck::gateway {

ConfigRepository::ConfigRepository(
    std::filesystem::path base_path, std::filesystem::path active_path,
    Validator validator, Reload reload)
    : base_path_(std::move(base_path)), active_path_(std::move(active_path)),
      validator_(std::move(validator)), reload_(std::move(reload)) {}

std::string ConfigRepository::revision(const std::string& text) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (unsigned char byte : text) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    std::ostringstream output;
    output << std::hex << std::setw(16) << std::setfill('0') << hash;
    return output.str();
}

std::string ConfigRepository::read(const std::filesystem::path& path) {
    if (path.empty()) return {};
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    std::ostringstream output;
    output << input.rdbuf();
    return output.str();
}

foundation::Result<ConfigSnapshot> ConfigRepository::snapshot_locked() const {
    ConfigSnapshot value;
    value.base = read(base_path_);
    if (value.base.empty()) {
        return foundation::Err<ConfigSnapshot>(
            foundation::ErrorCode::IoError,
            "base configuration is empty or unreadable");
    }
    value.active = read(active_path_);
    value.has_active = !value.active.empty();
    value.base_revision = revision(value.base);
    value.active_revision = revision(value.has_active ? value.active : value.base);
    return foundation::Ok(std::move(value));
}

foundation::Result<ConfigSnapshot> ConfigRepository::snapshot() const {
    std::lock_guard lock(mutex_);
    return snapshot_locked();
}

foundation::Result<void> ConfigRepository::validate_locked(
    const std::string& text) const {
    if (text.empty() || text.size() > 2 * 1024 * 1024) {
        return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                     "configuration size is invalid");
    }
    if (!validator_) {
        return foundation::Err<void>(foundation::ErrorCode::Unavailable,
                                     "configuration validator is unavailable");
    }
    return validator_(text);
}

foundation::Result<void> ConfigRepository::write_atomic_locked(
    const std::filesystem::path& path, const std::string& text) const {
    if (path.empty()) {
        return foundation::Err<void>(foundation::ErrorCode::Unavailable,
                                     "configuration path is unavailable");
    }
    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            return foundation::Err<void>(foundation::ErrorCode::IoError,
                                         "cannot open temporary configuration file");
        }
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        output.flush();
        if (!output) {
            std::filesystem::remove(temporary);
            return foundation::Err<void>(foundation::ErrorCode::IoError,
                                         "cannot write temporary configuration file");
        }
    }
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(temporary);
        return foundation::Err<void>(foundation::ErrorCode::IoError,
                                     "cannot replace configuration file");
    }
#else
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary);
        return foundation::Err<void>(foundation::ErrorCode::IoError,
                                     error.message());
    }
#endif
    return foundation::Ok();
}

foundation::Result<ConfigCommit> ConfigRepository::write_base(
    const std::string& expected_revision, const std::string& text) {
    std::lock_guard lock(mutex_);
    auto current = snapshot_locked();
    if (!current) return foundation::Err<ConfigCommit>(
        current.error().code, current.error().message);
    if (expected_revision != current->base_revision) {
        return foundation::Err<ConfigCommit>(
            foundation::ErrorCode::AlreadyExists,
            "base configuration revision conflict");
    }
    auto valid = validate_locked(text);
    if (!valid) return foundation::Err<ConfigCommit>(
        valid.error().code, valid.error().message);
    auto written = write_atomic_locked(base_path_, text);
    if (!written) return foundation::Err<ConfigCommit>(
        written.error().code, written.error().message);
    return foundation::Ok(ConfigCommit{revision(text), text != current->base});
}

foundation::Result<ConfigCommit> ConfigRepository::write_active(
    const std::string& expected_revision, const std::string& text) {
    std::lock_guard lock(mutex_);
    auto current = snapshot_locked();
    if (!current) return foundation::Err<ConfigCommit>(
        current.error().code, current.error().message);
    if (expected_revision != current->active_revision) {
        return foundation::Err<ConfigCommit>(
            foundation::ErrorCode::AlreadyExists,
            "active configuration revision conflict");
    }
    auto valid = validate_locked(text);
    if (!valid) return foundation::Err<ConfigCommit>(
        valid.error().code, valid.error().message);
    auto written = write_atomic_locked(active_path_, text);
    if (!written) return foundation::Err<ConfigCommit>(
        written.error().code, written.error().message);
    if (reload_) {
        auto reloaded = reload_();
        if (!reloaded) {
            if (current->has_active) {
                (void)write_atomic_locked(active_path_, current->active);
            } else {
                std::error_code ignored;
                std::filesystem::remove(active_path_, ignored);
            }
            return foundation::Err<ConfigCommit>(
                reloaded.error().code,
                "reload failed; active configuration restored: " +
                    reloaded.error().message);
        }
    }
    return foundation::Ok(ConfigCommit{
        revision(text), !current->has_active || text != current->active});
}

foundation::Result<ConfigCommit> ConfigRepository::reset_active(
    const std::string& expected_revision) {
    std::lock_guard lock(mutex_);
    auto current = snapshot_locked();
    if (!current) return foundation::Err<ConfigCommit>(
        current.error().code, current.error().message);
    if (expected_revision != current->active_revision) {
        return foundation::Err<ConfigCommit>(
            foundation::ErrorCode::AlreadyExists,
            "active configuration revision conflict");
    }
    if (!current->has_active) {
        return foundation::Ok(ConfigCommit{current->base_revision, false});
    }
    std::error_code error;
    const bool removed = std::filesystem::remove(active_path_, error);
    if (error) return foundation::Err<ConfigCommit>(
        foundation::ErrorCode::IoError, error.message());
    if (removed && reload_) {
        auto reloaded = reload_();
        if (!reloaded) {
            auto restored = write_atomic_locked(active_path_, current->active);
            const std::string suffix = restored
                ? "active configuration restored"
                : "active configuration rollback failed";
            return foundation::Err<ConfigCommit>(
                reloaded.error().code,
                "reload failed; " + suffix + ": " + reloaded.error().message);
        }
    }
    return foundation::Ok(ConfigCommit{current->base_revision, removed});
}

}
