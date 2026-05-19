#include "routes/FineTuningJobs.hpp"
#include "core/Logger.hpp"
#include <nlohmann/json.hpp>
namespace inferdeck::gateway::routes {
void HandleFineTuningJobsList(const httplib::Request& req, httplib::Response& resp) {
    nlohmann::json response;
    response["data"] = nlohmann::json::array();
    resp.set_content(response.dump(), "application/json");
}
void HandleFineTuningJobsCreate(const httplib::Request& req, httplib::Response& resp) {
    resp.status = 501;
    resp.set_content(R"({"error":"Not implemented"})", "application/json");
}
}
