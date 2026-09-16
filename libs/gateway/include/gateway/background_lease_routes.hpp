#pragma once

#include <httplib.h>

#include "gateway/routes.hpp"

#include <cstdint>
#include <string_view>

namespace inferdeck::gateway {

inline constexpr std::int64_t background_lease_default_seconds = 3600;
inline constexpr std::int64_t background_lease_min_seconds = 60;
inline constexpr std::int64_t background_lease_max_seconds = 43200;

void handle_background_availability(const httplib::Request& req,
                                    httplib::Response& resp,
                                    const GatewayDeps& deps,
                                    std::int64_t uptime_seconds);
void handle_acquire_background_lease(const httplib::Request& req,
                                     httplib::Response& resp,
                                     const GatewayDeps& deps,
                                     std::int64_t uptime_seconds);
void handle_renew_background_lease(const httplib::Request& req,
                                   httplib::Response& resp,
                                   const GatewayDeps& deps,
                                   std::string_view lease_id);
void handle_release_background_lease(const httplib::Request& req,
                                     httplib::Response& resp,
                                     const GatewayDeps& deps,
                                     std::string_view lease_id);

}
