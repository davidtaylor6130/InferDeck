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
