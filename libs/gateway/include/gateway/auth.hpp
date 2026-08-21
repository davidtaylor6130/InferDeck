#pragma once

#include <optional>
#include <charconv>
#include <cctype>
#include <string>
#include <string_view>

#include "gateway/route_manifest.hpp"

namespace inferdeck::gateway {

struct AuthConfig {
    bool required{false};
    std::string token{};
};

enum class RoutePrincipal {
    PublicStatus,
    OpenAIDataPlane,
    DashboardSession,
    ControlRead,
    ControlWrite,
};

enum class AuthorizationStatus {
    Granted,
    AuthenticationRequired,
    Forbidden,
};

struct RouteAuthConfig {
    AuthConfig data_plane{};
    bool control_allow_remote{false};
    bool control_allow_data_plane_token{false};
    std::string control_token{};
};

class AuthMiddleware {
public:
    explicit AuthMiddleware(AuthConfig cfg) : cfg_(std::move(cfg)) {}

    [[nodiscard]] bool check(const std::string& auth_header) const {
        if (!cfg_.required) return true;
        if (cfg_.token.empty()) return false;
        if (auth_header.size() < 7) return false;
        if (auth_header.compare(0, 7, "Bearer ") != 0) return false;
        std::string_view presented = std::string_view(auth_header).substr(7);
        std::size_t difference = presented.size() ^ cfg_.token.size();
        for (std::size_t index = 0; index < cfg_.token.size(); ++index) {
            const unsigned char actual = index < presented.size()
                ? static_cast<unsigned char>(presented[index]) : 0;
            difference |= actual ^ static_cast<unsigned char>(cfg_.token[index]);
        }
        return difference == 0;
    }

    [[nodiscard]] bool required() const noexcept { return cfg_.required; }

private:
    AuthConfig cfg_;
};

class RouteAuthorizer {
public:
    explicit RouteAuthorizer(RouteAuthConfig cfg)
        : cfg_(std::move(cfg)),
          data_plane_(cfg_.data_plane),
          control_({true, cfg_.control_token}) {}

    [[nodiscard]] AuthorizationStatus authorize(
        RoutePrincipal principal,
        const std::string& auth_header,
        std::string_view remote_address,
        std::string_view host,
        bool proxy_indicated) const {
        if (principal == RoutePrincipal::PublicStatus) {
            return AuthorizationStatus::Granted;
        }
        if (principal == RoutePrincipal::OpenAIDataPlane) {
            return data_plane_.check(auth_header)
                ? AuthorizationStatus::Granted
                : AuthorizationStatus::AuthenticationRequired;
        }
        if (is_direct_loopback(remote_address, host, proxy_indicated)) {
            return AuthorizationStatus::Granted;
        }
        if (principal == RoutePrincipal::DashboardSession) {
            return AuthorizationStatus::Forbidden;
        }
        if (!cfg_.control_allow_remote) {
            return AuthorizationStatus::Forbidden;
        }
        if (control_.check(auth_header)) {
            return AuthorizationStatus::Granted;
        }
        if (cfg_.control_allow_data_plane_token &&
            cfg_.data_plane.required && data_plane_.check(auth_header)) {
            return AuthorizationStatus::Granted;
        }
        return AuthorizationStatus::AuthenticationRequired;
    }

    [[nodiscard]] static bool is_loopback(std::string_view address) {
        if (address == "::1" || address == "[::1]" || address == "localhost") {
            return true;
        }
        if (address.starts_with("127.")) {
            return true;
        }
        return address.starts_with("::ffff:127.");
    }

    [[nodiscard]] static bool is_loopback_authority(std::string_view authority) {
        std::string lowered{authority};
        for (auto& c : lowered) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (lowered == "localhost" || lowered.starts_with("localhost:")) {
            const auto port = lowered.substr(std::string_view{"localhost"}.size());
            return port.empty() || valid_port(port.substr(1));
        }
        if (lowered == "[::1]") return true;
        if (lowered.starts_with("[::1]:")) {
            return valid_port(std::string_view{lowered}.substr(6));
        }
        const auto colon = lowered.find(':');
        const auto address = std::string_view{lowered}.substr(0, colon);
        if (colon != std::string::npos &&
            !valid_port(std::string_view{lowered}.substr(colon + 1))) {
            return false;
        }
        unsigned octets[4]{};
        std::size_t begin = 0;
        for (std::size_t index = 0; index < 4; ++index) {
            const auto end = index == 3 ? address.size() : address.find('.', begin);
            if (end == std::string_view::npos || end == begin) return false;
            const auto field = address.substr(begin, end - begin);
            const auto parsed = std::from_chars(field.data(), field.data() + field.size(),
                                                octets[index]);
            if (parsed.ec != std::errc{} || parsed.ptr != field.data() + field.size() ||
                octets[index] > 255) {
                return false;
            }
            begin = end + 1;
        }
        return octets[0] == 127;
    }

    [[nodiscard]] static bool is_direct_loopback(std::string_view address,
                                                  std::string_view host,
                                                  bool proxy_indicated) {
        return !proxy_indicated && is_loopback(address) &&
            is_loopback_authority(host);
    }

private:
    [[nodiscard]] static bool valid_port(std::string_view value) {
        if (value.empty()) return false;
        unsigned port = 0;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), port);
        return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() &&
            port > 0 && port <= 65535;
    }

    RouteAuthConfig cfg_;
    AuthMiddleware data_plane_;
    AuthMiddleware control_;
};

inline RoutePrincipal classify_route(std::string_view method,
                                     std::string_view path) {
    if (is_strict_openai_route(method, path)) {
        return RoutePrincipal::OpenAIDataPlane;
    }
    if (path.starts_with("/v1/")) {
        return RoutePrincipal::OpenAIDataPlane;
    }
    if (path.starts_with("/compat/openai-derivative/v1/")) {
        return RoutePrincipal::OpenAIDataPlane;
    }
    if (path == "/api/inferdeck/v1/status" ||
        path == "/api/inferdeck/v1/pricing" ||
        path == "/api/inferdeck/v1/events/stream") {
        return RoutePrincipal::DashboardSession;
    }
    if (path.starts_with("/api/")) {
        return method == "GET" || method == "HEAD"
            ? RoutePrincipal::ControlRead
            : RoutePrincipal::ControlWrite;
    }
    return RoutePrincipal::PublicStatus;
}

} // namespace inferdeck::gateway
