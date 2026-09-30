#include <catch2/catch_test_macros.hpp>

#include "gateway/api_key_store.hpp"

#include <chrono>
#include <atomic>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>

namespace {

namespace fs = std::filesystem;
using inferdeck::foundation::ErrorCode;
using inferdeck::gateway::ApiKeyStore;
using inferdeck::gateway::BackgroundLeaseAcquireState;

struct TempLeaseDb {
    fs::path path = fs::temp_directory_path() /
        ("inferdeck-background-lease-" +
         std::to_string(std::chrono::steady_clock::now()
                            .time_since_epoch().count()) +
         ".sqlite");

    ~TempLeaseDb() {
        std::error_code error;
        fs::remove(path, error);
        fs::remove(path.string() + "-shm", error);
        fs::remove(path.string() + "-wal", error);
    }
};

}

TEST_CASE("Background lease is exclusive, owner-idempotent, and renewable",
          "[auth][api-key][background-lease]") {
    ApiKeyStore store(":memory:");
    const auto first_key = store.create("first worker", -40);
    const auto second_key = store.create("second worker", -20);
    REQUIRE(first_key);
    REQUIRE(second_key);

    constexpr std::int64_t now = 1'800'000'000'000;
    const auto acquired = store.acquire_background_lease(
        first_key->record.id, now, 60'000);
    REQUIRE(acquired);
    CHECK(acquired->state == BackgroundLeaseAcquireState::Acquired);
    CHECK(acquired->lease.owner_key_id == first_key->record.id);
    CHECK(acquired->lease.expires_at_unix_ms == now + 60'000);

    const auto repeated = store.acquire_background_lease(
        first_key->record.id, now + 1'000, 120'000);
    REQUIRE(repeated);
    CHECK(repeated->state == BackgroundLeaseAcquireState::Existing);
    CHECK(repeated->lease.id == acquired->lease.id);
    CHECK(repeated->lease.expires_at_unix_ms == acquired->lease.expires_at_unix_ms);

    const auto occupied = store.acquire_background_lease(
        second_key->record.id, now + 2'000, 60'000);
    REQUIRE(occupied);
    CHECK(occupied->state == BackgroundLeaseAcquireState::Occupied);
    CHECK(occupied->lease.id == acquired->lease.id);

    const auto wrong_owner = store.renew_background_lease(
        second_key->record.id, acquired->lease.id, now + 3'000, 60'000);
    REQUIRE_FALSE(wrong_owner);
    CHECK(wrong_owner.error().code == ErrorCode::NotFound);

    const auto renewed = store.renew_background_lease(
        first_key->record.id, acquired->lease.id, now + 3'000, 90'000);
    REQUIRE(renewed);
    CHECK(renewed->id == acquired->lease.id);
    CHECK(renewed->acquired_at_unix_ms == acquired->lease.acquired_at_unix_ms);
    CHECK(renewed->expires_at_unix_ms == now + 93'000);

    REQUIRE(store.release_background_lease(
        first_key->record.id, acquired->lease.id));
    const auto released = store.active_background_lease(now + 4'000);
    REQUIRE(released);
    CHECK_FALSE(*released);
}

TEST_CASE("Background lease persists, expires, and is released by key revocation",
          "[auth][api-key][background-lease]") {
    TempLeaseDb temp;
    std::string first_key_id;
    std::string second_key_id;
    std::string lease_id;
    constexpr std::int64_t now = 1'800'000'000'000;

    {
        ApiKeyStore store(temp.path.string());
        const auto first_key = store.create("persistent worker", -50);
        const auto second_key = store.create("replacement worker", -30);
        REQUIRE(first_key);
        REQUIRE(second_key);
        first_key_id = first_key->record.id;
        second_key_id = second_key->record.id;
        const auto acquired = store.acquire_background_lease(
            first_key_id, now, 60'000);
        REQUIRE(acquired);
        lease_id = acquired->lease.id;
    }

    {
        ApiKeyStore reopened(temp.path.string());
        const auto persisted = reopened.active_background_lease(now + 10'000);
        REQUIRE(persisted);
        REQUIRE(*persisted);
        CHECK((*persisted)->id == lease_id);
        CHECK((*persisted)->owner_key_id == first_key_id);

        REQUIRE(reopened.revoke(first_key_id));
        const auto after_revoke = reopened.active_background_lease(now + 11'000);
        REQUIRE(after_revoke);
        CHECK_FALSE(*after_revoke);

        const auto replacement = reopened.acquire_background_lease(
            second_key_id, now + 12'000, 60'000);
        REQUIRE(replacement);
        CHECK(replacement->state == BackgroundLeaseAcquireState::Acquired);
        CHECK(replacement->lease.owner_key_id == second_key_id);

        const auto after_expiry = reopened.active_background_lease(now + 73'000);
        REQUIRE(after_expiry);
        CHECK_FALSE(*after_expiry);
    }
}

TEST_CASE("Concurrent background lease acquisition has one winner",
          "[auth][api-key][background-lease]") {
    ApiKeyStore store(":memory:");
    const auto first_key = store.create("first concurrent worker", -50);
    const auto second_key = store.create("second concurrent worker", -50);
    REQUIRE(first_key);
    REQUIRE(second_key);
    std::atomic<bool> start{false};
    std::optional<inferdeck::gateway::BackgroundLeaseAcquireResult> first;
    std::optional<inferdeck::gateway::BackgroundLeaseAcquireResult> second;

    std::thread first_thread([&] {
        while (!start.load()) std::this_thread::yield();
        const auto result = store.acquire_background_lease(
            first_key->record.id, 1'800'000'000'000, 60'000);
        if (result) first = *result;
    });
    std::thread second_thread([&] {
        while (!start.load()) std::this_thread::yield();
        const auto result = store.acquire_background_lease(
            second_key->record.id, 1'800'000'000'000, 60'000);
        if (result) second = *result;
    });
    start.store(true);
    first_thread.join();
    second_thread.join();

    REQUIRE(first);
    REQUIRE(second);
    const int acquired_count =
        (first->state == BackgroundLeaseAcquireState::Acquired ? 1 : 0) +
        (second->state == BackgroundLeaseAcquireState::Acquired ? 1 : 0);
    const int occupied_count =
        (first->state == BackgroundLeaseAcquireState::Occupied ? 1 : 0) +
        (second->state == BackgroundLeaseAcquireState::Occupied ? 1 : 0);
    CHECK(acquired_count == 1);
    CHECK(occupied_count == 1);
    CHECK(first->lease.id == second->lease.id);
}
