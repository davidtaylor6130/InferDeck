#include "gateway/api_key_store.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <utility>

#include <sqlite3.h>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#endif

namespace inferdeck::gateway {
namespace {

using foundation::Err;
using foundation::ErrorCode;
using foundation::Ok;
using Hash = std::array<unsigned char, 32>;

struct ActiveApiKey {
    ApiKeyRecord record;
    Hash hash{};
};

std::int64_t unix_time_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

bool valid_name(const std::string& name) {
    if (name.empty() || name.size() > 80) return false;
    bool visible = false;
    for (const unsigned char character : name) {
        if (character < 0x20 || character == 0x7f) return false;
        if (!std::isspace(character)) visible = true;
    }
    return visible;
}

bool valid_priority(int priority) {
    return priority >= -100 && priority <= 100;
}

foundation::Result<std::vector<unsigned char>> secure_random(
    std::size_t size) {
#ifdef _WIN32
    std::vector<unsigned char> bytes(size);
    const NTSTATUS status = BCryptGenRandom(
        nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status < 0) {
        return Err<std::vector<unsigned char>>(
            ErrorCode::Internal, "cannot generate API key material");
    }
    return Ok(std::move(bytes));
#else
    (void)size;
    return Err<std::vector<unsigned char>>(
        ErrorCode::Unavailable,
        "secure API key generation requires the Windows build");
#endif
}

foundation::Result<Hash> sha256(std::string_view value) {
#ifdef _WIN32
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash_handle = nullptr;
    DWORD object_size = 0;
    DWORD hash_size = 0;
    DWORD received = 0;
    if (BCryptOpenAlgorithmProvider(
            &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptGetProperty(
            algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size),
            &received, 0) < 0 ||
        BCryptGetProperty(
            algorithm, BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&hash_size), sizeof(hash_size),
            &received, 0) < 0 ||
        hash_size != Hash{}.size()) {
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        return Err<Hash>(ErrorCode::Internal, "cannot initialize SHA-256");
    }
    std::vector<unsigned char> object(object_size);
    Hash digest{};
    if (BCryptCreateHash(
            algorithm, &hash_handle, object.data(), object_size,
            nullptr, 0, 0) < 0 ||
        BCryptHashData(
            hash_handle,
            reinterpret_cast<PUCHAR>(const_cast<char*>(value.data())),
            static_cast<ULONG>(value.size()), 0) < 0 ||
        BCryptFinishHash(
            hash_handle, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) {
        if (hash_handle) BCryptDestroyHash(hash_handle);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return Err<Hash>(ErrorCode::Internal, "cannot hash API credential");
    }
    BCryptDestroyHash(hash_handle);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return Ok(digest);
#else
    (void)value;
    return Err<Hash>(ErrorCode::Unavailable,
                     "SHA-256 requires the Windows build");
#endif
}

std::string hex_encode(const unsigned char* data, std::size_t size) {
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < size; ++index) {
        output << std::setw(2) << static_cast<unsigned>(data[index]);
    }
    return output.str();
}

std::string base64url_encode(const std::vector<unsigned char>& bytes) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string output;
    output.reserve((bytes.size() * 4 + 2) / 3);
    for (std::size_t index = 0; index < bytes.size(); index += 3) {
        const std::uint32_t first = bytes[index];
        const std::uint32_t second =
            index + 1 < bytes.size() ? bytes[index + 1] : 0;
        const std::uint32_t third =
            index + 2 < bytes.size() ? bytes[index + 2] : 0;
        const std::uint32_t chunk = (first << 16) | (second << 8) | third;
        output.push_back(alphabet[(chunk >> 18) & 0x3f]);
        output.push_back(alphabet[(chunk >> 12) & 0x3f]);
        if (index + 1 < bytes.size()) {
            output.push_back(alphabet[(chunk >> 6) & 0x3f]);
        }
        if (index + 2 < bytes.size()) {
            output.push_back(alphabet[chunk & 0x3f]);
        }
    }
    return output;
}

std::optional<std::pair<std::string, std::string_view>> parse_bearer(
    std::string_view authorization) {
    constexpr std::size_t identifier_length = 32;
    constexpr std::size_t secret_length = 43;
    if (!authorization.starts_with("Bearer ")) return std::nullopt;
    const std::string_view token = authorization.substr(7);
    if (token.size() != 4 + identifier_length + 1 + secret_length ||
        !token.starts_with("idk_") || token[4 + identifier_length] != '_') {
        return std::nullopt;
    }
    const std::string_view identifier = token.substr(4, identifier_length);
    if (!std::all_of(identifier.begin(), identifier.end(), [](unsigned char value) {
            return std::isdigit(value) || (value >= 'a' && value <= 'f');
        })) {
        return std::nullopt;
    }
    const std::string_view secret = token.substr(5 + identifier_length);
    if (!std::all_of(secret.begin(), secret.end(), [](unsigned char value) {
            return std::isalnum(value) || value == '-' || value == '_';
        })) {
        return std::nullopt;
    }
    return std::pair{std::string(identifier), token};
}

bool constant_time_equal(const Hash& left, const Hash& right) {
    unsigned char difference = 0;
    for (std::size_t index = 0; index < left.size(); ++index) {
        difference |= left[index] ^ right[index];
    }
    return difference == 0;
}

const char* column_text(sqlite3_stmt* statement, int column) {
    const auto* value = sqlite3_column_text(statement, column);
    return value ? reinterpret_cast<const char*>(value) : "";
}

foundation::Result<void> sqlite_failure(sqlite3* database,
                                        std::string operation) {
    return Err<void>(ErrorCode::IoError,
                     std::move(operation) + ": " +
                         (database ? sqlite3_errmsg(database) : "database unavailable"));
}

class SqliteTransaction {
public:
    explicit SqliteTransaction(sqlite3* database) : database_(database) {
        active_ = database_ &&
            sqlite3_exec(database_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) ==
                SQLITE_OK;
    }

    ~SqliteTransaction() {
        if (active_) {
            sqlite3_exec(database_, "ROLLBACK;", nullptr, nullptr, nullptr);
        }
    }

    [[nodiscard]] bool started() const noexcept {
        return active_;
    }

    bool commit() {
        if (!active_) return false;
        if (sqlite3_exec(database_, "COMMIT;", nullptr, nullptr, nullptr) !=
            SQLITE_OK) {
            return false;
        }
        active_ = false;
        return true;
    }

private:
    sqlite3* database_{nullptr};
    bool active_{false};
};

foundation::Result<std::optional<BackgroundLeaseRecord>>
read_active_background_lease(sqlite3* database, std::int64_t now_unix_ms) {
    constexpr const char* delete_sql =
        "DELETE FROM background_leases WHERE expires_at_unix_ms <= ?;";
    sqlite3_stmt* expired = nullptr;
    if (sqlite3_prepare_v2(
            database, delete_sql, -1, &expired, nullptr) != SQLITE_OK) {
        return Err<std::optional<BackgroundLeaseRecord>>(
            ErrorCode::IoError, "cannot prepare expired background lease cleanup");
    }
    sqlite3_bind_int64(expired, 1, now_unix_ms);
    const int delete_status = sqlite3_step(expired);
    sqlite3_finalize(expired);
    if (delete_status != SQLITE_DONE) {
        return Err<std::optional<BackgroundLeaseRecord>>(
            ErrorCode::IoError, "cannot remove expired background lease");
    }

    constexpr const char* select_sql =
        "SELECT id, owner_key_id, acquired_at_unix_ms, expires_at_unix_ms "
        "FROM background_leases WHERE singleton = 1;";
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(
            database, select_sql, -1, &statement, nullptr) != SQLITE_OK) {
        return Err<std::optional<BackgroundLeaseRecord>>(
            ErrorCode::IoError, "cannot inspect background lease");
    }
    const int status = sqlite3_step(statement);
    if (status == SQLITE_DONE) {
        sqlite3_finalize(statement);
        return Ok(std::optional<BackgroundLeaseRecord>{});
    }
    if (status != SQLITE_ROW) {
        sqlite3_finalize(statement);
        return Err<std::optional<BackgroundLeaseRecord>>(
            ErrorCode::IoError, "cannot read background lease");
    }
    BackgroundLeaseRecord lease;
    lease.id = column_text(statement, 0);
    lease.owner_key_id = column_text(statement, 1);
    lease.acquired_at_unix_ms = sqlite3_column_int64(statement, 2);
    lease.expires_at_unix_ms = sqlite3_column_int64(statement, 3);
    sqlite3_finalize(statement);
    return Ok(std::optional<BackgroundLeaseRecord>{std::move(lease)});
}

bool valid_lease_window(std::int64_t now_unix_ms, std::int64_t duration_ms) {
    return now_unix_ms >= 0 && duration_ms > 0 &&
        now_unix_ms <= (std::numeric_limits<std::int64_t>::max)() - duration_ms;
}

}

class ApiKeyStore::Impl {
public:
    explicit Impl(std::string requested_path) : path(std::move(requested_path)) {
        if (path.empty()) return;
        if (path != ":memory:") {
            const std::filesystem::path file(path);
            const auto parent = file.parent_path();
            std::error_code error;
            if (!parent.empty()) std::filesystem::create_directories(parent, error);
            if (error) return;
        }
        if (sqlite3_open_v2(
                path.c_str(), &database,
                SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                nullptr) != SQLITE_OK) {
            close();
            return;
        }
        sqlite3_busy_timeout(database, 5000);
        constexpr const char* schema = R"SQL(
PRAGMA journal_mode=WAL;
PRAGMA synchronous=NORMAL;
PRAGMA foreign_keys=ON;
CREATE TABLE IF NOT EXISTS api_keys (
    id TEXT PRIMARY KEY,
    name TEXT NOT NULL,
    key_prefix TEXT NOT NULL,
    secret_hash BLOB NOT NULL,
    priority INTEGER NOT NULL CHECK(priority BETWEEN -100 AND 100),
    created_at_unix_ms INTEGER NOT NULL,
    updated_at_unix_ms INTEGER NOT NULL,
    revoked_at_unix_ms INTEGER
);
CREATE TABLE IF NOT EXISTS background_leases (
    singleton INTEGER PRIMARY KEY CHECK(singleton = 1),
    id TEXT NOT NULL UNIQUE,
    owner_key_id TEXT NOT NULL,
    acquired_at_unix_ms INTEGER NOT NULL,
    expires_at_unix_ms INTEGER NOT NULL,
    FOREIGN KEY(owner_key_id) REFERENCES api_keys(id)
);
)SQL";
        if (sqlite3_exec(database, schema, nullptr, nullptr, nullptr) != SQLITE_OK ||
            !load_active()) {
            close();
            return;
        }
        is_healthy = true;
    }

    ~Impl() {
        close();
    }

    bool load_active() {
        constexpr const char* sql =
            "SELECT id, name, key_prefix, secret_hash, priority, "
            "created_at_unix_ms, updated_at_unix_ms "
            "FROM api_keys WHERE revoked_at_unix_ms IS NULL;";
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK) {
            return false;
        }
        int status = SQLITE_ROW;
        while ((status = sqlite3_step(statement)) == SQLITE_ROW) {
            const auto* blob = static_cast<const unsigned char*>(
                sqlite3_column_blob(statement, 3));
            if (!blob || sqlite3_column_bytes(statement, 3) != 32) {
                sqlite3_finalize(statement);
                return false;
            }
            ActiveApiKey active_key;
            active_key.record.id = column_text(statement, 0);
            active_key.record.name = column_text(statement, 1);
            active_key.record.prefix = column_text(statement, 2);
            std::copy_n(blob, active_key.hash.size(), active_key.hash.begin());
            active_key.record.priority = sqlite3_column_int(statement, 4);
            active_key.record.created_at_unix_ms = sqlite3_column_int64(statement, 5);
            active_key.record.updated_at_unix_ms = sqlite3_column_int64(statement, 6);
            active.emplace(active_key.record.id, std::move(active_key));
        }
        const bool complete = status == SQLITE_DONE;
        sqlite3_finalize(statement);
        return complete;
    }

    void close() {
        active.clear();
        if (database) {
            sqlite3_close(database);
            database = nullptr;
        }
        is_healthy = false;
    }

    std::string path;
    sqlite3* database{nullptr};
    bool is_healthy{false};
    mutable std::mutex mutex;
    std::unordered_map<std::string, ActiveApiKey> active;
};

ApiKeyStore::ApiKeyStore(std::string path)
    : impl_(std::make_unique<Impl>(std::move(path))) {}

ApiKeyStore::~ApiKeyStore() = default;

bool ApiKeyStore::healthy() const noexcept {
    std::lock_guard lock(impl_->mutex);
    return impl_->is_healthy;
}

const std::string& ApiKeyStore::path() const noexcept {
    return impl_->path;
}

foundation::Result<CreatedApiKey> ApiKeyStore::create(
    const std::string& name, int priority) {
    if (!valid_name(name)) {
        return Err<CreatedApiKey>(ErrorCode::InvalidArgument,
                                  "API key name must contain 1 to 80 printable characters",
                                  "name");
    }
    if (!valid_priority(priority)) {
        return Err<CreatedApiKey>(ErrorCode::InvalidArgument,
                                  "API key priority must be between -100 and 100",
                                  "priority");
    }
    std::lock_guard lock(impl_->mutex);
    if (!impl_->is_healthy) {
        return Err<CreatedApiKey>(ErrorCode::Unavailable,
                                  "API key store is unavailable");
    }
    for (int attempt = 0; attempt < 4; ++attempt) {
        const auto identifier_bytes = secure_random(16);
        const auto secret_bytes = secure_random(32);
        if (!identifier_bytes) {
            return Err<CreatedApiKey>(identifier_bytes.error().code,
                                      identifier_bytes.error().message);
        }
        if (!secret_bytes) {
            return Err<CreatedApiKey>(secret_bytes.error().code,
                                      secret_bytes.error().message);
        }
        const std::string identifier = hex_encode(
            identifier_bytes->data(), identifier_bytes->size());
        const std::string key = "idk_" + identifier + "_" +
            base64url_encode(*secret_bytes);
        const auto digest = sha256(key);
        if (!digest) {
            return Err<CreatedApiKey>(digest.error().code, digest.error().message);
        }
        const std::int64_t now = unix_time_ms();
        const std::string prefix = key.substr(0, 16);
        constexpr const char* sql =
            "INSERT INTO api_keys(id, name, key_prefix, secret_hash, priority, "
            "created_at_unix_ms, updated_at_unix_ms, revoked_at_unix_ms) "
            "VALUES(?, ?, ?, ?, ?, ?, ?, NULL);";
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(
                impl_->database, sql, -1, &statement, nullptr) != SQLITE_OK) {
            return Err<CreatedApiKey>(ErrorCode::IoError,
                                      "cannot prepare API key creation");
        }
        sqlite3_bind_text(statement, 1, identifier.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 2, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 3, prefix.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_blob(statement, 4, digest->data(),
                          static_cast<int>(digest->size()), SQLITE_TRANSIENT);
        sqlite3_bind_int(statement, 5, priority);
        sqlite3_bind_int64(statement, 6, now);
        sqlite3_bind_int64(statement, 7, now);
        const int status = sqlite3_step(statement);
        sqlite3_finalize(statement);
        if (status == SQLITE_CONSTRAINT) continue;
        if (status != SQLITE_DONE) {
            return Err<CreatedApiKey>(ErrorCode::IoError,
                                      "cannot persist API key");
        }
        ApiKeyRecord record{
            identifier, name, prefix, priority, now, now, std::nullopt};
        impl_->active.emplace(
            identifier, ActiveApiKey{record, *digest});
        return Ok(CreatedApiKey{std::move(record), key});
    }
    return Err<CreatedApiKey>(ErrorCode::AlreadyExists,
                              "could not allocate a unique API key identifier");
}

foundation::Result<std::vector<ApiKeyRecord>> ApiKeyStore::list() const {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->is_healthy) {
        return Err<std::vector<ApiKeyRecord>>(
            ErrorCode::Unavailable, "API key store is unavailable");
    }
    constexpr const char* sql =
        "SELECT id, name, key_prefix, priority, created_at_unix_ms, "
        "updated_at_unix_ms, revoked_at_unix_ms "
        "FROM api_keys ORDER BY created_at_unix_ms DESC, id ASC;";
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(
            impl_->database, sql, -1, &statement, nullptr) != SQLITE_OK) {
        return Err<std::vector<ApiKeyRecord>>(
            ErrorCode::IoError, "cannot list API keys");
    }
    std::vector<ApiKeyRecord> records;
    int status = SQLITE_ROW;
    while ((status = sqlite3_step(statement)) == SQLITE_ROW) {
        ApiKeyRecord record;
        record.id = column_text(statement, 0);
        record.name = column_text(statement, 1);
        record.prefix = column_text(statement, 2);
        record.priority = sqlite3_column_int(statement, 3);
        record.created_at_unix_ms = sqlite3_column_int64(statement, 4);
        record.updated_at_unix_ms = sqlite3_column_int64(statement, 5);
        if (sqlite3_column_type(statement, 6) != SQLITE_NULL) {
            record.revoked_at_unix_ms = sqlite3_column_int64(statement, 6);
        }
        records.push_back(std::move(record));
    }
    sqlite3_finalize(statement);
    if (status != SQLITE_DONE) {
        return Err<std::vector<ApiKeyRecord>>(
            ErrorCode::IoError, "cannot complete API key listing");
    }
    return Ok(std::move(records));
}

foundation::Result<ApiKeyRecord> ApiKeyStore::update(
    std::string_view id, std::optional<std::string> name,
    std::optional<int> priority) {
    if (!name && !priority) {
        return Err<ApiKeyRecord>(ErrorCode::InvalidArgument,
                                 "name or priority is required");
    }
    if (name && !valid_name(*name)) {
        return Err<ApiKeyRecord>(ErrorCode::InvalidArgument,
                                 "API key name must contain 1 to 80 printable characters",
                                 "name");
    }
    if (priority && !valid_priority(*priority)) {
        return Err<ApiKeyRecord>(ErrorCode::InvalidArgument,
                                 "API key priority must be between -100 and 100",
                                 "priority");
    }
    std::lock_guard lock(impl_->mutex);
    if (!impl_->is_healthy) {
        return Err<ApiKeyRecord>(ErrorCode::Unavailable,
                                 "API key store is unavailable");
    }
    const auto active = impl_->active.find(std::string(id));
    if (active == impl_->active.end()) {
        return Err<ApiKeyRecord>(ErrorCode::NotFound,
                                 "active API key not found");
    }
    ApiKeyRecord updated = active->second.record;
    if (name) updated.name = std::move(*name);
    if (priority) updated.priority = *priority;
    updated.updated_at_unix_ms = unix_time_ms();
    constexpr const char* sql =
        "UPDATE api_keys SET name = ?, priority = ?, updated_at_unix_ms = ? "
        "WHERE id = ? AND revoked_at_unix_ms IS NULL;";
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(
            impl_->database, sql, -1, &statement, nullptr) != SQLITE_OK) {
        return Err<ApiKeyRecord>(ErrorCode::IoError,
                                 "cannot prepare API key update");
    }
    sqlite3_bind_text(statement, 1, updated.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 2, updated.priority);
    sqlite3_bind_int64(statement, 3, updated.updated_at_unix_ms);
    const std::string identifier(id);
    sqlite3_bind_text(statement, 4, identifier.c_str(), -1, SQLITE_TRANSIENT);
    const int status = sqlite3_step(statement);
    sqlite3_finalize(statement);
    if (status != SQLITE_DONE || sqlite3_changes(impl_->database) != 1) {
        return Err<ApiKeyRecord>(ErrorCode::IoError,
                                 "cannot update API key");
    }
    active->second.record = updated;
    return Ok(std::move(updated));
}

foundation::Result<void> ApiKeyStore::revoke(std::string_view id) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->is_healthy) {
        return Err<void>(ErrorCode::Unavailable, "API key store is unavailable");
    }
    const std::string identifier(id);
    SqliteTransaction transaction(impl_->database);
    if (!transaction.started()) {
        return sqlite_failure(impl_->database,
                              "cannot begin API key revocation");
    }
    constexpr const char* lookup_sql =
        "SELECT revoked_at_unix_ms FROM api_keys WHERE id = ?;";
    sqlite3_stmt* lookup = nullptr;
    if (sqlite3_prepare_v2(
            impl_->database, lookup_sql, -1, &lookup, nullptr) != SQLITE_OK) {
        return sqlite_failure(impl_->database, "cannot inspect API key");
    }
    sqlite3_bind_text(lookup, 1, identifier.c_str(), -1, SQLITE_TRANSIENT);
    const int lookup_status = sqlite3_step(lookup);
    if (lookup_status == SQLITE_DONE) {
        sqlite3_finalize(lookup);
        return Err<void>(ErrorCode::NotFound, "API key not found");
    }
    if (lookup_status != SQLITE_ROW) {
        sqlite3_finalize(lookup);
        return sqlite_failure(impl_->database, "cannot inspect API key");
    }
    const bool already_revoked =
        sqlite3_column_type(lookup, 0) != SQLITE_NULL;
    sqlite3_finalize(lookup);
    if (already_revoked) {
        if (!transaction.commit()) {
            return sqlite_failure(impl_->database,
                                  "cannot complete API key revocation");
        }
        return Ok();
    }

    constexpr const char* update_sql =
        "UPDATE api_keys SET revoked_at_unix_ms = ?, updated_at_unix_ms = ? "
        "WHERE id = ? AND revoked_at_unix_ms IS NULL;";
    sqlite3_stmt* update_statement = nullptr;
    if (sqlite3_prepare_v2(
            impl_->database, update_sql, -1, &update_statement, nullptr) != SQLITE_OK) {
        return sqlite_failure(impl_->database, "cannot prepare API key revocation");
    }
    const std::int64_t now = unix_time_ms();
    sqlite3_bind_int64(update_statement, 1, now);
    sqlite3_bind_int64(update_statement, 2, now);
    sqlite3_bind_text(update_statement, 3, identifier.c_str(), -1, SQLITE_TRANSIENT);
    const int status = sqlite3_step(update_statement);
    sqlite3_finalize(update_statement);
    const int updated_rows = sqlite3_changes(impl_->database);
    if (status != SQLITE_DONE || updated_rows != 1) {
        return sqlite_failure(impl_->database, "cannot revoke API key");
    }

    constexpr const char* release_sql =
        "DELETE FROM background_leases WHERE owner_key_id = ?;";
    sqlite3_stmt* release_statement = nullptr;
    if (sqlite3_prepare_v2(
            impl_->database, release_sql, -1, &release_statement,
            nullptr) != SQLITE_OK) {
        return sqlite_failure(impl_->database,
                              "cannot prepare revoked key lease release");
    }
    sqlite3_bind_text(
        release_statement, 1, identifier.c_str(), -1, SQLITE_TRANSIENT);
    const int release_status = sqlite3_step(release_statement);
    sqlite3_finalize(release_statement);
    if (release_status != SQLITE_DONE) {
        return sqlite_failure(impl_->database,
                              "cannot release revoked key lease");
    }
    if (!transaction.commit()) {
        return sqlite_failure(impl_->database,
                              "cannot complete API key revocation");
    }
    impl_->active.erase(identifier);
    return Ok();
}

foundation::Result<std::optional<BackgroundLeaseRecord>>
ApiKeyStore::active_background_lease(std::int64_t now_unix_ms) {
    if (now_unix_ms < 0) {
        return Err<std::optional<BackgroundLeaseRecord>>(
            ErrorCode::InvalidArgument, "lease time must not be negative");
    }
    std::lock_guard lock(impl_->mutex);
    if (!impl_->is_healthy) {
        return Err<std::optional<BackgroundLeaseRecord>>(
            ErrorCode::Unavailable, "API key store is unavailable");
    }
    SqliteTransaction transaction(impl_->database);
    if (!transaction.started()) {
        return Err<std::optional<BackgroundLeaseRecord>>(
            ErrorCode::IoError, "cannot begin background lease inspection");
    }
    auto lease = read_active_background_lease(
        impl_->database, now_unix_ms);
    if (!lease) return lease;
    if (!transaction.commit()) {
        return Err<std::optional<BackgroundLeaseRecord>>(
            ErrorCode::IoError, "cannot complete background lease inspection");
    }
    return lease;
}

foundation::Result<BackgroundLeaseAcquireResult>
ApiKeyStore::acquire_background_lease(std::string_view owner_key_id,
                                      std::int64_t now_unix_ms,
                                      std::int64_t duration_ms) {
    if (!valid_lease_window(now_unix_ms, duration_ms)) {
        return Err<BackgroundLeaseAcquireResult>(
            ErrorCode::InvalidArgument, "background lease window is invalid");
    }
    std::lock_guard lock(impl_->mutex);
    if (!impl_->is_healthy) {
        return Err<BackgroundLeaseAcquireResult>(
            ErrorCode::Unavailable, "API key store is unavailable");
    }
    const std::string owner(owner_key_id);
    if (!impl_->active.contains(owner)) {
        return Err<BackgroundLeaseAcquireResult>(
            ErrorCode::NotFound, "active API key not found");
    }
    SqliteTransaction transaction(impl_->database);
    if (!transaction.started()) {
        return Err<BackgroundLeaseAcquireResult>(
            ErrorCode::IoError, "cannot begin background lease acquisition");
    }
    const auto current = read_active_background_lease(
        impl_->database, now_unix_ms);
    if (!current) {
        return Err<BackgroundLeaseAcquireResult>(
            current.error().code, current.error().message);
    }
    if (*current) {
        const auto state = (*current)->owner_key_id == owner
            ? BackgroundLeaseAcquireState::Existing
            : BackgroundLeaseAcquireState::Occupied;
        if (!transaction.commit()) {
            return Err<BackgroundLeaseAcquireResult>(
                ErrorCode::IoError,
                "cannot complete background lease acquisition");
        }
        return Ok(BackgroundLeaseAcquireResult{state, **current});
    }

    const auto identifier_bytes = secure_random(16);
    if (!identifier_bytes) {
        return Err<BackgroundLeaseAcquireResult>(
            identifier_bytes.error().code, identifier_bytes.error().message);
    }
    BackgroundLeaseRecord lease;
    lease.id = hex_encode(identifier_bytes->data(), identifier_bytes->size());
    lease.owner_key_id = owner;
    lease.acquired_at_unix_ms = now_unix_ms;
    lease.expires_at_unix_ms = now_unix_ms + duration_ms;

    constexpr const char* insert_sql =
        "INSERT INTO background_leases("
        "singleton, id, owner_key_id, acquired_at_unix_ms, expires_at_unix_ms) "
        "VALUES(1, ?, ?, ?, ?);";
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(
            impl_->database, insert_sql, -1, &statement, nullptr) != SQLITE_OK) {
        return Err<BackgroundLeaseAcquireResult>(
            ErrorCode::IoError, "cannot prepare background lease acquisition");
    }
    sqlite3_bind_text(statement, 1, lease.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, lease.owner_key_id.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 3, lease.acquired_at_unix_ms);
    sqlite3_bind_int64(statement, 4, lease.expires_at_unix_ms);
    const int status = sqlite3_step(statement);
    sqlite3_finalize(statement);
    if (status != SQLITE_DONE || !transaction.commit()) {
        return Err<BackgroundLeaseAcquireResult>(
            ErrorCode::IoError, "cannot persist background lease");
    }
    return Ok(BackgroundLeaseAcquireResult{
        BackgroundLeaseAcquireState::Acquired, std::move(lease)});
}

foundation::Result<BackgroundLeaseRecord>
ApiKeyStore::renew_background_lease(std::string_view owner_key_id,
                                    std::string_view lease_id,
                                    std::int64_t now_unix_ms,
                                    std::int64_t duration_ms) {
    if (!valid_lease_window(now_unix_ms, duration_ms)) {
        return Err<BackgroundLeaseRecord>(
            ErrorCode::InvalidArgument, "background lease window is invalid");
    }
    std::lock_guard lock(impl_->mutex);
    if (!impl_->is_healthy) {
        return Err<BackgroundLeaseRecord>(
            ErrorCode::Unavailable, "API key store is unavailable");
    }
    const std::string owner(owner_key_id);
    if (!impl_->active.contains(owner)) {
        return Err<BackgroundLeaseRecord>(
            ErrorCode::NotFound, "background lease not found");
    }
    SqliteTransaction transaction(impl_->database);
    if (!transaction.started()) {
        return Err<BackgroundLeaseRecord>(
            ErrorCode::IoError, "cannot begin background lease renewal");
    }
    const auto current = read_active_background_lease(
        impl_->database, now_unix_ms);
    if (!current) {
        return Err<BackgroundLeaseRecord>(
            current.error().code, current.error().message);
    }
    if (!*current || (*current)->owner_key_id != owner ||
        (*current)->id != lease_id) {
        if (!transaction.commit()) {
            return Err<BackgroundLeaseRecord>(
                ErrorCode::IoError, "cannot complete background lease renewal");
        }
        return Err<BackgroundLeaseRecord>(
            ErrorCode::NotFound, "background lease not found");
    }

    BackgroundLeaseRecord renewed = **current;
    renewed.expires_at_unix_ms = now_unix_ms + duration_ms;
    constexpr const char* update_sql =
        "UPDATE background_leases SET expires_at_unix_ms = ? "
        "WHERE singleton = 1 AND id = ? AND owner_key_id = ?;";
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(
            impl_->database, update_sql, -1, &statement, nullptr) != SQLITE_OK) {
        return Err<BackgroundLeaseRecord>(
            ErrorCode::IoError, "cannot prepare background lease renewal");
    }
    sqlite3_bind_int64(statement, 1, renewed.expires_at_unix_ms);
    sqlite3_bind_text(statement, 2, renewed.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 3, owner.c_str(), -1, SQLITE_TRANSIENT);
    const int status = sqlite3_step(statement);
    sqlite3_finalize(statement);
    const int updated_rows = sqlite3_changes(impl_->database);
    if (status != SQLITE_DONE || updated_rows != 1 || !transaction.commit()) {
        return Err<BackgroundLeaseRecord>(
            ErrorCode::IoError, "cannot renew background lease");
    }
    return Ok(std::move(renewed));
}

foundation::Result<void>
ApiKeyStore::release_background_lease(std::string_view owner_key_id,
                                      std::string_view lease_id) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->is_healthy) {
        return Err<void>(ErrorCode::Unavailable,
                         "API key store is unavailable");
    }
    SqliteTransaction transaction(impl_->database);
    if (!transaction.started()) {
        return Err<void>(ErrorCode::IoError,
                         "cannot begin background lease release");
    }
    constexpr const char* delete_sql =
        "DELETE FROM background_leases "
        "WHERE singleton = 1 AND id = ? AND owner_key_id = ?;";
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(
            impl_->database, delete_sql, -1, &statement, nullptr) != SQLITE_OK) {
        return Err<void>(ErrorCode::IoError,
                         "cannot prepare background lease release");
    }
    const std::string identifier(lease_id);
    const std::string owner(owner_key_id);
    sqlite3_bind_text(statement, 1, identifier.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, owner.c_str(), -1, SQLITE_TRANSIENT);
    const int status = sqlite3_step(statement);
    sqlite3_finalize(statement);
    if (status != SQLITE_DONE || !transaction.commit()) {
        return Err<void>(ErrorCode::IoError,
                         "cannot release background lease");
    }
    return Ok();
}

std::optional<AuthenticatedApiKey> ApiKeyStore::authenticate_bearer(
    std::string_view authorization) const {
    const auto parsed = parse_bearer(authorization);
    if (!parsed) return std::nullopt;
    const auto digest = sha256(parsed->second);
    if (!digest) return std::nullopt;
    std::lock_guard lock(impl_->mutex);
    if (!impl_->is_healthy) return std::nullopt;
    const auto active = impl_->active.find(parsed->first);
    if (active == impl_->active.end() ||
        !constant_time_equal(active->second.hash, *digest)) {
        return std::nullopt;
    }
    return AuthenticatedApiKey{
        active->second.record.id,
        active->second.record.name,
        active->second.record.priority};
}

int resolve_request_priority(const ApiKeyStore* store,
                             std::string_view authorization,
                             int requested_priority) noexcept {
    if (store) {
        const auto principal = store->authenticate_bearer(authorization);
        if (principal) return principal->priority;
    }
    return std::clamp(requested_priority, -100, 100);
}

std::string credential_fingerprint(std::string_view credential) {
    const auto digest = sha256(credential);
    if (!digest) return {};
    return hex_encode(digest->data(), digest->size());
}

}
