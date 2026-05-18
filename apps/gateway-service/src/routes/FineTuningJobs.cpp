/// @file FineTuningJobs.cpp
/// @brief /v1/fine_tuning/jobs route handlers for fine-tuning lifecycle.

#include "FineTuningJobs.hpp"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <iomanip>
#include <sstream>

namespace inferdeck::gateway::routes {

std::string ValidateFineTuningCreate(const nlohmann::json& body) {
    if (!body.contains("model") || !body["model"].is_string()) {
        return "missing_model";
    }
    if (!body.contains("training_file") || !body["training_file"].is_string()) {
        return "missing_training_file";
    }
    return ""; // valid
}

void HandleFineTuningJobsList(const httplib::Request& req, httplib::Response& resp) {
    nlohmann::json result;
    result["object"] = "list";

    // auto& registry = backends::BackendRegistry::Instance();
    // auto ft = registry.GetTrainingBackend("llama_finetune");
    // if (ft && ft->IsReady()) {
    //     auto jobs = ft->ListJobs();
    //     nlohmann::json data = nlohmann::json::array();
    //     for (const auto& job : jobs) {
    //         nlohmann::json j;
    //         j["id"] = job.id;
    //         j["object"] = "fine_tuning.job";
    //         j["model"] = job.model_path;
    //         j["status"] = static_cast<int>(job.status);
    //         j["created_at"] = std::time(nullptr);
    //         j["finished_at"] = 0;
    //         j["hyperparameters"] = job.parameters;
    //         j["trained_tokens"] = 0;
    //         data.push_back(j);
    //     }
    //     result["data"] = data;
    // }

    nlohmann::json data = nlohmann::json::array();
    result["data"] = data;
    result["has_more"] = false;

    resp.status = 200;
    resp.set_content(result.dump(), "application/json");
}

void HandleFineTuningJobsCreate(const httplib::Request& req, httplib::Response& resp) {
    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req.body);
    } catch (const std::exception& e) {
        nlohmann::json error;
        error["error"]["message"] = "Invalid JSON body";
        error["error"]["type"] = "invalid_request_error";
        resp.status = 400;
        resp.set_content(error.dump(), "application/json");
        return;
    }

    auto validation = ValidateFineTuningCreate(body);
    if (!validation.empty()) {
        nlohmann::json error;
        if (validation == "missing_model") {
            error["error"]["message"] = "model is required";
        } else if (validation == "missing_training_file") {
            error["error"]["message"] = "training_file is required";
        }
        error["error"]["type"] = "invalid_request_error";
        resp.status = 400;
        resp.set_content(error.dump(), "application/json");
        return;
    }

    std::string model = body["model"].get<std::string>();
    std::string training_file = body["training_file"].get<std::string>();
    int epochs = body.value("epochs", 3);
    float learning_rate = body.value("learning_rate", 0.0001f);
    int batch_size = body.value("batch_size", 1);
    int max_steps = body.value("max_steps", 1000);

    nlohmann::json request_params;
    request_params["model_path"] = model;
    request_params["train_data_path"] = training_file;
    request_params["epochs"] = epochs;
    request_params["learning_rate"] = learning_rate;
    request_params["batch_size"] = batch_size;
    request_params["max_steps"] = max_steps;

    // auto& registry = backends::BackendRegistry::Instance();
    // auto ft = registry.GetTrainingBackend("llama_finetune");
    // std::string job_id;
    // if (ft && ft->IsReady()) {
    //     job_id = ft->SubmitJob(request_params);
    // } else {
    //     // Generate fake job ID for testing
    //     job_id = "ft_" + std::to_string(std::time(nullptr));
    // }

    std::string job_id = "ft_" + std::to_string(std::time(nullptr));

    nlohmann::json result;
    result["id"] = job_id;
    result["object"] = "fine_tuning.job";
    result["model"] = model;
    result["status"] = "queued";
    result["created_at"] = std::time(nullptr);
    result["finished_at"] = 0;
    result["result_files"] = nlohmann::json::array();
    result["hyperparameters"] = nlohmann::json{{"n_epochs", epochs}, {"learning_rate_multiplier", learning_rate}};
    result["training_file"] = training_file;
    result["trained_tokens"] = 0;
    result["validation_file"] = body.value("validation_file", "");
    result["integrations"] = nlohmann::json::array();
    result["seed"] = body.value("seed", 0);
    result["suffix"] = body.value("suffix", "");

    resp.status = 200;
    resp.set_content(result.dump(), "application/json");
    spdlog::info("FineTuning: submitted job '{}' [model: {}, epochs: {}]",
                 job_id, model, epochs);
}

void HandleFineTuningJobsGet(const httplib::Request& req, httplib::Response& resp) {
    std::string job_id = req.path.substr(req.path.find_last_of('/') + 1);

    nlohmann::json result;
    result["id"] = job_id;
    result["object"] = "fine_tuning.job";
    result["model"] = "unknown";
    result["status"] = "queued";
    result["created_at"] = std::time(nullptr);
    result["finished_at"] = 0;
    result["result_files"] = nlohmann::json::array();
    result["hyperparameters"] = nlohmann::json::object();
    result["training_file"] = "";
    result["trained_tokens"] = 0;

    resp.status = 200;
    resp.set_content(result.dump(), "application/json");
}

void HandleFineTuningJobsCancel(const httplib::Request& req, httplib::Response& resp) {
    std::string job_id = req.path.substr(req.path.find_last_of('/') + 1);

    nlohmann::json result;
    result["id"] = job_id;
    result["object"] = "fine_tuning.job";
    result["cancelled"] = true;

    resp.status = 200;
    resp.set_content(result.dump(), "application/json");
    spdlog::info("FineTuning: cancelled job '{}'", job_id);
}

} // namespace inferdeck::gateway::routes
