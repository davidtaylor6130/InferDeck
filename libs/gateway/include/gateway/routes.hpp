#pragma once

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "engine/token_sequence.hpp"
#include "model/backend_coordinator.hpp"
#include "model/model_registry.hpp"
#include "scheduler/scheduler.hpp"

#include <chrono>
#include <string>

namespace inferdeck::gateway {

struct GatewayDeps {
    model::BackendCoordinator& coordinator;
    scheduler::Scheduler& scheduler;
    std::string default_swap_timeout_s{"15"};
    bool auto_swap{true};
};

void write_json(httplib::Response& resp, int status, const nlohmann::json& body);
void write_error(httplib::Response& resp, int status, const std::string& code,
                 const std::string& message);
std::string header_value(const httplib::Request& req, const std::string& name);

void handle_models(const httplib::Request& req, httplib::Response& resp,
                   const GatewayDeps& deps);

void handle_swap_to(const httplib::Request& req, httplib::Response& resp,
                    const GatewayDeps& deps, const std::string& model_name);

void handle_swap_status(const httplib::Request& req, httplib::Response& resp,
                        const GatewayDeps& deps);

void handle_chat_completions(const httplib::Request& req, httplib::Response& resp,
                             const GatewayDeps& deps);

} // namespace inferdeck::gateway
