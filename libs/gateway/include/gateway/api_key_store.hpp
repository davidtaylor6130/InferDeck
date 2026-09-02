#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "foundation/result.hpp"

namespace inferdeck::gateway {

struct ApiKeyRecord {
    std::string id;
    std::string name;
    std::string prefix;
    int priority{};
    std::int64_t created_at_unix_ms{};
    std::int64_t updated_at_unix_ms{};
    std::optional<std::int64_t> revoked_at_unix_ms;
};

struct CreatedApiKey {
    ApiKeyRecord record;
    std::string key;
};

struct AuthenticatedApiKey {
    std::string id;
    std::string name;
    int priority{};
};

struct BackgroundLeaseRecord {
    std::string id;
    std::string owner_key_id;
    std::int64_t acquired_at_unix_ms{};
    std::int64_t expires_at_unix_ms{};
};

enum class BackgroundLeaseAcquireState {
    Acquired,
    Existing,
    Occupied,
};

struct BackgroundLeaseAcquireResult {
    BackgroundLeaseAcquireState state{BackgroundLeaseAcquireState::Acquired};
    BackgroundLeaseRecord lease;
};

class ApiKeyStore final {
public:
    explicit ApiKeyStore(std::string path);
    ~ApiKeyStore();

    ApiKeyStore(const ApiKeyStore&) = delete;
    ApiKeyStore& operator=(const ApiKeyStore&) = delete;
    ApiKeyStore(ApiKeyStore&&) = delete;
    ApiKeyStore& operator=(ApiKeyStore&&) = delete;

    [[nodiscard]] bool healthy() const noexcept;
    [[nodiscard]] const std::string& path() const noexcept;

    [[nodiscard]] foundation::Result<CreatedApiKey> create(
        const std::string& name, int priority);
    [[nodiscard]] foundation::Result<std::vector<ApiKeyRecord>> list() const;
    [[nodiscard]] foundation::Result<ApiKeyRecord> update(
        std::string_view id, std::optional<std::string> name,
        std::optional<int> priority);
    [[nodiscard]] foundation::Result<void> revoke(std::string_view id);
    [[nodiscard]] std::optional<AuthenticatedApiKey> authenticate_bearer(
        std::string_view authorization) const;
    [[nodiscard]] foundation::Result<std::optional<BackgroundLeaseRecord>>
        active_background_lease(std::int64_t now_unix_ms);
    [[nodiscard]] foundation::Result<BackgroundLeaseAcquireResult>
        acquire_background_lease(std::string_view owner_key_id,
                                 std::int64_t now_unix_ms,
                                 std::int64_t duration_ms);
    [[nodiscard]] foundation::Result<BackgroundLeaseRecord>
        renew_background_lease(std::string_view owner_key_id,
                               std::string_view lease_id,
                               std::int64_t now_unix_ms,
                               std::int64_t duration_ms);
    [[nodiscard]] foundation::Result<void>
        release_background_lease(std::string_view owner_key_id,
                                 std::string_view lease_id);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] int resolve_request_priority(
    const ApiKeyStore* store, std::string_view authorization,
    int requested_priority) noexcept;

[[nodiscard]] std::string credential_fingerprint(
    std::string_view credential);

}
