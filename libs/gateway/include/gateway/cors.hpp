#pragma once

#include <httplib.h>
#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace inferdeck::gateway {

class CorsMiddleware {
public:
    explicit CorsMiddleware(std::vector<std::string> origins)
        : origins_(std::move(origins)) {}

    void apply(const httplib::Request& req, httplib::Response& resp) const {
        if (origins_.empty()) return;
        const auto origin = req.get_header_value("Origin");
        if (origin.empty()) return;
        if (allows_wildcard()) {
            resp.set_header("Access-Control-Allow-Origin", "*");
        } else if (allows_origin(origin)) {
            resp.set_header("Access-Control-Allow-Origin", origin);
        } else {
            return;
        }
        resp.set_header("Vary", "Origin");
        resp.set_header("Access-Control-Allow-Methods",
                        "GET, POST, PUT, DELETE, OPTIONS");
        resp.set_header("Access-Control-Allow-Headers",
                        "Authorization, Content-Type, X-Request-Id, OpenAI-Beta");
    }

    [[nodiscard]] bool handles_options() const noexcept { return !origins_.empty(); }
    [[nodiscard]] bool allows_origin(std::string_view origin) const {
        return std::find(origins_.begin(), origins_.end(), origin) != origins_.end();
    }
    [[nodiscard]] bool allows_wildcard() const {
        return std::find(origins_.begin(), origins_.end(), "*") != origins_.end();
    }

private:
    std::vector<std::string> origins_;
};

} // namespace inferdeck::gateway
