#pragma once

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "foundation/event_bus.hpp"
#include "gateway/swap_tracker.hpp"
#include "model/backend_coordinator.hpp"
#include "model/model_registry.hpp"
#include "observability/metrics.hpp"
#include "observability/stats_db.hpp"

#include <chrono>
#include <string>

namespace inferdeck::gateway {

struct GatewayDeps {
    model::BackendCoordinator& coordinator;
    std::string default_swap_timeout_s{"15"};
    bool auto_swap{true};
    observability::Metrics* metrics{nullptr};
    observability::StatsDb* stats_db{nullptr};
    foundation::EventBus* events{nullptr};
    SwapTracker* swap_tracker{nullptr};
};

void write_json(httplib::Response& resp, int status, const nlohmann::json& body);
void write_error(httplib::Response& resp, int status, const std::string& code,
                 const std::string& message);
std::string header_value(const httplib::Request& req, const std::string& name);

struct SwapStartResult {
    int status{200};
    nlohmann::json body;
};

SwapStartResult start_swap_async(const GatewayDeps& deps, const std::string& model_name);

void handle_models(const httplib::Request& req, httplib::Response& resp,
                   const GatewayDeps& deps);

void handle_swap_to(const httplib::Request& req, httplib::Response& resp,
                    const GatewayDeps& deps, const std::string& model_name);

void handle_swap_cancel(const httplib::Request& req, httplib::Response& resp,
                        const GatewayDeps& deps);

void handle_swap_status(const httplib::Request& req, httplib::Response& resp,
                        const GatewayDeps& deps);

void handle_chat_completions(const httplib::Request& req, httplib::Response& resp,
                             const GatewayDeps& deps);

} // namespace inferdeck::gateway
