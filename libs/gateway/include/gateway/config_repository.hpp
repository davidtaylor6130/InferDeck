#pragma once

#include <filesystem>
#include <functional>
#include <mutex>
#include <string>

#include "foundation/result.hpp"

namespace inferdeck::gateway {

struct ConfigSnapshot {
    std::string base;
    std::string active;
    std::string base_revision;
    std::string active_revision;
    bool has_active{false};
};

struct ConfigCommit {
    std::string revision;
    bool changed{false};
};

class ConfigRepository {
public:
    using Validator = std::function<foundation::Result<void>(const std::string&)>;
    using Reload = std::function<foundation::Result<void>()>;

    ConfigRepository(std::filesystem::path base_path,
                     std::filesystem::path active_path,
                     Validator validator, Reload reload);

    foundation::Result<ConfigSnapshot> snapshot() const;
    foundation::Result<ConfigCommit> write_base(
        const std::string& expected_revision, const std::string& text);
    foundation::Result<ConfigCommit> write_active(
        const std::string& expected_revision, const std::string& text);
    foundation::Result<ConfigCommit> reset_active(
        const std::string& expected_revision);

    static std::string revision(const std::string& text);

private:
    foundation::Result<ConfigSnapshot> snapshot_locked() const;
    foundation::Result<void> validate_locked(const std::string& text) const;
    foundation::Result<void> write_atomic_locked(
        const std::filesystem::path& path, const std::string& text) const;
    static std::string read(const std::filesystem::path& path);

    std::filesystem::path base_path_;
    std::filesystem::path active_path_;
    Validator validator_;
    Reload reload_;
    mutable std::mutex mutex_;
};

}
