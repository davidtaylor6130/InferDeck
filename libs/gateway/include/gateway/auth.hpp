#pragma once

#include <optional>
#include <string>
#include <string_view>

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

} // namespace inferdeck::gateway
