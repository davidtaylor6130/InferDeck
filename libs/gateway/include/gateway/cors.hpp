#pragma once

#include <httplib.h>
#include <algorithm>
#include <string>
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
        const bool wildcard = std::find(origins_.begin(), origins_.end(), "*") != origins_.end();
        if (wildcard) {
            resp.set_header("Access-Control-Allow-Origin", "*");
        } else if (std::find(origins_.begin(), origins_.end(), origin) != origins_.end()) {
            resp.set_header("Access-Control-Allow-Origin", origin);
        } else {
            return;
        }
        resp.set_header("Vary", "Origin");
        resp.set_header("Access-Control-Allow-Methods",
                        "GET, POST, PUT, DELETE, OPTIONS");
        resp.set_header("Access-Control-Allow-Headers",
                        "Authorization, Content-Type, X-Request-Id, X-Api-Key, "
                        "Anthropic-Version, Anthropic-Beta, OpenAI-Beta");
    }

    [[nodiscard]] bool handles_options() const noexcept { return !origins_.empty(); }

private:
    std::vector<std::string> origins_;
};

} // namespace inferdeck::gateway
