#pragma once

#include <httplib.h>
#include <string>
#include <vector>

namespace inferdeck::gateway {

class CorsMiddleware {
public:
    explicit CorsMiddleware(std::vector<std::string> origins)
        : origins_(std::move(origins)) {}

    void apply(httplib::Response& resp) const {
        if (origins_.empty()) return;
        std::string joined;
        for (std::size_t i = 0; i < origins_.size(); ++i) {
            if (i) joined += ", ";
            joined += origins_[i];
        }
        resp.set_header("Access-Control-Allow-Origin", joined);
        resp.set_header("Vary", "Origin");
        resp.set_header("Access-Control-Allow-Methods",
                        "GET, POST, PUT, DELETE, OPTIONS");
        resp.set_header("Access-Control-Allow-Headers",
                        "Authorization, Content-Type, X-Request-Id");
    }

    [[nodiscard]] bool handles_options() const noexcept { return !origins_.empty(); }

private:
    std::vector<std::string> origins_;
};

} // namespace inferdeck::gateway
