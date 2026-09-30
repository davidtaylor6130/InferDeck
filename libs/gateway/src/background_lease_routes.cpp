#include "gateway/background_lease_routes.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>

namespace inferdeck::gateway {
namespace {

using foundation::Err;
using foundation::Error;
using foundation::ErrorCode;
using foundation::Ok;

struct RuntimeAvailability {
    bool available{false};
    std::string reason;
    std::int64_t idle_for_ms{};
    std::int64_t required_idle_ms{};
    std::int64_t last_activity_unix_ms{};
    std::int64_t suggested_base_unix_ms{};
    int active_requests{};
    std::size_t queued_requests{};
};

std::int64_t unix_time_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

void prevent_caching(httplib::Response& resp) {
    resp.set_header("Cache-Control", "no-store");
    resp.set_header("Pragma", "no-cache");
}

std::optional<AuthenticatedApiKey> require_managed_key(
    const httplib::Request& req, httplib::Response& resp,
    const GatewayDeps& deps) {
    if (!deps.api_keys || !deps.api_keys->healthy()) {
        write_error(resp, 503, "api_key_store_unavailable",
                    "API key store is unavailable");
        return std::nullopt;
    }
    const auto key = deps.api_keys->authenticate_bearer(
        header_value(req, "Authorization"));
    if (!key) {
        resp.set_header("WWW-Authenticate", "Bearer");
        write_error(resp, 401, "unauthorized",
                    "valid managed API key required");
        return std::nullopt;
    }
    return key;
}

foundation::Result<std::int64_t> parse_duration_seconds(
    const httplib::Request& req) {
    if (req.body.empty()) return Ok(background_lease_default_seconds);
    const auto body = nlohmann::json::parse(req.body, nullptr, false);
    if (body.is_discarded() || !body.is_object()) {
        return Err<std::int64_t>(ErrorCode::InvalidArgument,
                                 "request body must be a JSON object");
    }
    for (const auto& [field, value] : body.items()) {
        (void)value;
        if (field != "durationSeconds") {
            return Err<std::int64_t>(ErrorCode::InvalidArgument,
                                     "unknown field: " + field, field);
        }
    }
    if (!body.contains("durationSeconds")) {
        return Ok(background_lease_default_seconds);
    }
    const auto& value = body["durationSeconds"];
    std::int64_t seconds = 0;
    if (value.is_number_unsigned()) {
        const std::uint64_t unsigned_seconds = value.get<std::uint64_t>();
        if (unsigned_seconds > static_cast<std::uint64_t>(
                (std::numeric_limits<std::int64_t>::max)())) {
            return Err<std::int64_t>(
                ErrorCode::InvalidArgument,
                "durationSeconds must be between 60 and 43200",
                "durationSeconds");
        }
        seconds = static_cast<std::int64_t>(unsigned_seconds);
    } else if (value.is_number_integer()) {
        seconds = value.get<std::int64_t>();
    } else {
        return Err<std::int64_t>(
            ErrorCode::InvalidArgument,
            "durationSeconds must be an integer", "durationSeconds");
    }
    if (seconds < background_lease_min_seconds ||
        seconds > background_lease_max_seconds) {
        return Err<std::int64_t>(
            ErrorCode::InvalidArgument,
            "durationSeconds must be between 60 and 43200",
            "durationSeconds");
    }
    return Ok(seconds);
}

std::int64_t suggested_report_time(std::int64_t base_unix_ms,
                                   std::string_view key_id) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char value : key_id) {
        hash ^= value;
        hash *= 1099511628211ULL;
    }
    const std::int64_t jitter_ms = 5'000 +
        static_cast<std::int64_t>(hash % 25'001ULL);
    const std::int64_t maximum =
        (std::numeric_limits<std::int64_t>::max)();
    return base_unix_ms > maximum - jitter_ms
        ? maximum : base_unix_ms + jitter_ms;
}

void set_retry_after(httplib::Response& resp, std::int64_t now_unix_ms,
                     std::int64_t suggested_unix_ms) {
    const std::int64_t remaining_ms = std::max<std::int64_t>(
        1, suggested_unix_ms - now_unix_ms);
    resp.set_header("Retry-After",
                    std::to_string((remaining_ms + 999) / 1000));
}

nlohmann::json lease_json(const BackgroundLeaseRecord& lease) {
    return {
        {"id", lease.id},
        {"acquiredAtUnixMs", lease.acquired_at_unix_ms},
        {"expiresAtUnixMs", lease.expires_at_unix_ms},
    };
}

void write_store_error(httplib::Response& resp, const Error& error) {
    if (error.code == ErrorCode::InvalidArgument) {
        write_error(resp, 400, "invalid_request", error.message,
                    error.field.empty() ? nlohmann::json(nullptr)
                                        : nlohmann::json(error.field));
        return;
    }
    if (error.code == ErrorCode::NotFound) {
        write_error(resp, 404, "background_lease_not_found",
                    "background lease not found");
        return;
    }
    if (error.code == ErrorCode::Unavailable) {
        write_error(resp, 503, "api_key_store_unavailable",
                    "API key store is unavailable");
        return;
    }
    write_error(resp, 500, "background_lease_store_error",
                "background lease could not be persisted");
}

RuntimeAvailability runtime_availability(
    const GatewayDeps& deps, std::int64_t now_unix_ms,
    std::int64_t uptime_seconds) {
    RuntimeAvailability result;
    result.active_requests = deps.coordinator.active_request_count();
    result.queued_requests = deps.coordinator.queued_request_count();
    result.required_idle_ms = std::max<std::int64_t>(
        0, static_cast<std::int64_t>(deps.background_idle_after_seconds)) *
        1000;
    if (result.active_requests > 0) {
        result.reason = "active_requests";
        result.suggested_base_unix_ms = now_unix_ms + 30'000;
        return result;
    }
    if (result.queued_requests > 0) {
        result.reason = "queued_requests";
        result.suggested_base_unix_ms = now_unix_ms + 30'000;
        return result;
    }
    const bool swapping = deps.coordinator.swap_in_progress() ||
        (deps.swap_tracker && deps.swap_tracker->snapshot().swapping);
    if (swapping) {
        result.reason = "model_swap";
        result.suggested_base_unix_ms = now_unix_ms + 60'000;
        return result;
    }
    if (maintenance_mode_active(deps)) {
        result.reason = "maintenance";
        result.suggested_base_unix_ms = now_unix_ms + 60'000;
        return result;
    }
    if (!deps.stats_db || !deps.stats_db->healthy()) {
        result.reason = "activity_history_unavailable";
        result.suggested_base_unix_ms = now_unix_ms + 60'000;
        return result;
    }

    const std::int64_t bounded_uptime = std::clamp<std::int64_t>(
        uptime_seconds, 0, now_unix_ms / 1000);
    result.last_activity_unix_ms =
        now_unix_ms - bounded_uptime * 1000;
    const auto requests = deps.stats_db->recent_requests(1);
    if (!requests.empty()) {
        result.last_activity_unix_ms = std::max(
            result.last_activity_unix_ms,
            requests.front().timestamp_unix_ms);
    }
    const auto swaps = deps.stats_db->recent_swaps(1);
    if (!swaps.empty()) {
        result.last_activity_unix_ms = std::max(
            result.last_activity_unix_ms,
            swaps.front().timestamp_unix_ms);
    }
    result.idle_for_ms = std::max<std::int64_t>(
        0, now_unix_ms - result.last_activity_unix_ms);
    if (result.idle_for_ms < result.required_idle_ms) {
        result.reason = "quiet_period";
        result.suggested_base_unix_ms =
            result.last_activity_unix_ms + result.required_idle_ms;
        return result;
    }
    result.available = true;
    result.reason = "idle";
    return result;
}

void write_lease_conflict(httplib::Response& resp,
                          const BackgroundLeaseRecord& lease,
                          std::int64_t now_unix_ms,
                          std::string_view key_id) {
    const std::int64_t suggested = suggested_report_time(
        lease.expires_at_unix_ms, key_id);
    set_retry_after(resp, now_unix_ms, suggested);
    auto body = make_error_json(
        409, "background_lease_unavailable",
        "another background lease is active");
    body["reason"] = "lease_active";
    body["expiresAtUnixMs"] = lease.expires_at_unix_ms;
    body["suggestedReportBackAtUnixMs"] = suggested;
    write_json(resp, 409, body);
}

void write_runtime_conflict(httplib::Response& resp,
                            const RuntimeAvailability& availability,
                            std::int64_t now_unix_ms,
                            std::string_view key_id) {
    const std::int64_t suggested = suggested_report_time(
        availability.suggested_base_unix_ms, key_id);
    set_retry_after(resp, now_unix_ms, suggested);
    auto body = make_error_json(
        409, "background_not_idle",
        "InferDeck is not available for background work");
    body["reason"] = availability.reason;
    body["suggestedReportBackAtUnixMs"] = suggested;
    body["activeRequests"] = availability.active_requests;
    body["queuedRequests"] = availability.queued_requests;
    body["idleForSeconds"] = availability.idle_for_ms / 1000;
    body["requiredIdleSeconds"] = availability.required_idle_ms / 1000;
    write_json(resp, 409, body);
}

void write_lease_success(httplib::Response& resp, int status,
                         std::string_view state,
                         const BackgroundLeaseRecord& lease) {
    write_json(resp, status, {
        {"status", state},
        {"lease", lease_json(lease)},
    });
}

}

void handle_background_availability(const httplib::Request& req,
                                    httplib::Response& resp,
                                    const GatewayDeps& deps,
                                    std::int64_t uptime_seconds) {
    prevent_caching(resp);
    const auto key = require_managed_key(req, resp, deps);
    if (!key) return;
    const std::int64_t now = unix_time_ms();
    const auto active_lease = deps.api_keys->active_background_lease(now);
    if (!active_lease) {
        write_store_error(resp, active_lease.error());
        return;
    }
    if (*active_lease) {
        const bool owned = (*active_lease)->owner_key_id == key->id;
        nlohmann::json lease = {
            {"active", true},
            {"ownedByCaller", owned},
            {"expiresAtUnixMs", (*active_lease)->expires_at_unix_ms},
        };
        nlohmann::json body = {
            {"available", owned},
            {"reason", owned ? "lease_owned" : "lease_active"},
            {"checkedAtUnixMs", now},
            {"lease", std::move(lease)},
        };
        if (owned) {
            body["lease"]["id"] = (*active_lease)->id;
            body["lease"]["acquiredAtUnixMs"] =
                (*active_lease)->acquired_at_unix_ms;
        } else {
            const std::int64_t suggested = suggested_report_time(
                (*active_lease)->expires_at_unix_ms, key->id);
            body["suggestedReportBackAtUnixMs"] = suggested;
            set_retry_after(resp, now, suggested);
        }
        write_json(resp, 200, body);
        return;
    }

    const auto availability = runtime_availability(
        deps, now, uptime_seconds);
    nlohmann::json body = {
        {"available", availability.available},
        {"reason", availability.reason},
        {"checkedAtUnixMs", now},
        {"activeRequests", availability.active_requests},
        {"queuedRequests", availability.queued_requests},
        {"idleForSeconds", availability.idle_for_ms / 1000},
        {"requiredIdleSeconds", availability.required_idle_ms / 1000},
        {"lease", {{"active", false}, {"ownedByCaller", false}}},
    };
    if (!availability.available) {
        const std::int64_t suggested = suggested_report_time(
            availability.suggested_base_unix_ms, key->id);
        body["suggestedReportBackAtUnixMs"] = suggested;
        set_retry_after(resp, now, suggested);
    }
    write_json(resp, 200, body);
}

void handle_acquire_background_lease(const httplib::Request& req,
                                     httplib::Response& resp,
                                     const GatewayDeps& deps,
                                     std::int64_t uptime_seconds) {
    prevent_caching(resp);
    const auto key = require_managed_key(req, resp, deps);
    if (!key) return;
    const auto duration_seconds = parse_duration_seconds(req);
    if (!duration_seconds) {
        write_store_error(resp, duration_seconds.error());
        return;
    }
    const std::int64_t now = unix_time_ms();
    const auto active_lease = deps.api_keys->active_background_lease(now);
    if (!active_lease) {
        write_store_error(resp, active_lease.error());
        return;
    }
    if (*active_lease) {
        if ((*active_lease)->owner_key_id == key->id) {
            write_lease_success(resp, 200, "existing", **active_lease);
        } else {
            write_lease_conflict(resp, **active_lease, now, key->id);
        }
        return;
    }
    const auto availability = runtime_availability(
        deps, now, uptime_seconds);
    if (!availability.available) {
        write_runtime_conflict(resp, availability, now, key->id);
        return;
    }
    const auto acquired = deps.api_keys->acquire_background_lease(
        key->id, now, *duration_seconds * 1000);
    if (!acquired) {
        write_store_error(resp, acquired.error());
        return;
    }
    if (acquired->state == BackgroundLeaseAcquireState::Occupied) {
        write_lease_conflict(resp, acquired->lease, now, key->id);
        return;
    }
    const bool created =
        acquired->state == BackgroundLeaseAcquireState::Acquired;
    write_lease_success(resp, created ? 201 : 200,
                        created ? "acquired" : "existing",
                        acquired->lease);
}

void handle_renew_background_lease(const httplib::Request& req,
                                   httplib::Response& resp,
                                   const GatewayDeps& deps,
                                   std::string_view lease_id) {
    prevent_caching(resp);
    const auto key = require_managed_key(req, resp, deps);
    if (!key) return;
    const auto duration_seconds = parse_duration_seconds(req);
    if (!duration_seconds) {
        write_store_error(resp, duration_seconds.error());
        return;
    }
    const auto renewed = deps.api_keys->renew_background_lease(
        key->id, lease_id, unix_time_ms(), *duration_seconds * 1000);
    if (!renewed) {
        write_store_error(resp, renewed.error());
        return;
    }
    write_lease_success(resp, 200, "renewed", *renewed);
}

void handle_release_background_lease(const httplib::Request& req,
                                     httplib::Response& resp,
                                     const GatewayDeps& deps,
                                     std::string_view lease_id) {
    prevent_caching(resp);
    const auto key = require_managed_key(req, resp, deps);
    if (!key) return;
    const auto released = deps.api_keys->release_background_lease(
        key->id, lease_id);
    if (!released) {
        write_store_error(resp, released.error());
        return;
    }
    resp.status = 204;
}

}
