#pragma once

#include <optional>
#include <string>

namespace inferdeck::gateway {

struct AuthConfig {
    bool required{false};
    std::string token{};
};

class AuthMiddleware {
public:
    explicit AuthMiddleware(AuthConfig cfg) : cfg_(std::move(cfg)) {}

    [[nodiscard]] bool check(const std::string& auth_header) const {
        if (!cfg_.required) return true;
        if (cfg_.token.empty()) return true;
        if (auth_header.size() < 7) return false;
        if (auth_header.compare(0, 7, "Bearer ") != 0) return false;
        std::string_view presented = std::string_view(auth_header).substr(7);
        return presented == cfg_.token;
    }

    [[nodiscard]] bool required() const noexcept { return cfg_.required; }

private:
    AuthConfig cfg_;
};

} // namespace inferdeck::gateway
