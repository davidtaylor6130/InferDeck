/// @file test_fine_tuning.cpp
/// @brief Unit tests for FineTuningJobs route handlers.

#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>
#include "routes/FineTuningJobs.hpp"
#include <nlohmann/json.hpp>

TEST_CASE("ValidateFineTuningCreate accepts valid input", "[route][finetune]") {
    nlohmann::json body;
    body["model"] = "llama-7b";
    body["training_file"] = "train.jsonl";

    std::string error = inferdeck::gateway::routes::ValidateFineTuningCreate(body);
    REQUIRE(error.empty());
}

TEST_CASE("ValidateFineTuningCreate rejects missing model", "[route][finetune]") {
    nlohmann::json body;
    body["training_file"] = "train.jsonl";

    std::string error = inferdeck::gateway::routes::ValidateFineTuningCreate(body);
    REQUIRE(!error.empty());
    REQUIRE(error.find("model") != std::string::npos);
}

TEST_CASE("ValidateFineTuningCreate rejects missing training file", "[route][finetune]") {
    nlohmann::json body;
    body["model"] = "llama-7b";

    std::string error = inferdeck::gateway::routes::ValidateFineTuningCreate(body);
    REQUIRE(!error.empty());
    REQUIRE(error.find("training_file") != std::string::npos);
}

TEST_CASE("HandleFineTuningJobsList returns valid response", "[route][finetune]") {
    httplib::Request req;
    httplib::Response resp;

    inferdeck::gateway::routes::HandleFineTuningJobsList(req, resp);

    REQUIRE(resp.status == 200);
    nlohmann::json j = nlohmann::json::parse(resp.body);
    REQUIRE(j["object"] == "list");
    REQUIRE(j.contains("data"));
    REQUIRE(j.contains("has_more"));
    REQUIRE(j["has_more"] == false);
}

TEST_CASE("HandleFineTuningJobsCreate accepts valid request", "[route][finetune]") {
    httplib::Request req;
    nlohmann::json body;
    body["model"] = "llama-7b";
    body["training_file"] = "train.jsonl";
    body["epochs"] = 5;
    body["learning_rate"] = 0.001f;
    body["batch_size"] = 4;
    body["max_steps"] = 1000;
    body["validation_file"] = "val.jsonl";
    body["seed"] = 42;
    body["suffix"] = "my-finetune";
    req.body = body.dump();

    httplib::Response resp;
    inferdeck::gateway::routes::HandleFineTuningJobsCreate(req, resp);

    REQUIRE(resp.status == 200);
    nlohmann::json j = nlohmann::json::parse(resp.body);
    REQUIRE(j["object"] == "fine_tuning.job");
    REQUIRE(j.contains("id"));
    REQUIRE(j["model"] == "llama-7b");
    REQUIRE(j["status"] == "queued");
    REQUIRE(j.contains("created_at"));
    REQUIRE(j.contains("hyperparameters"));
    REQUIRE(j["training_file"] == "train.jsonl");
    REQUIRE(j["hyperparameters"]["n_epochs"] == 5);
    REQUIRE(j["hyperparameters"]["learning_rate_multiplier"] == 0.001);
}

TEST_CASE("HandleFineTuningJobsCreate uses defaults for optional fields", "[route][finetune]") {
    httplib::Request req;
    nlohmann::json body;
    body["model"] = "llama-7b";
    body["training_file"] = "train.jsonl";
    req.body = body.dump();

    httplib::Response resp;
    inferdeck::gateway::routes::HandleFineTuningJobsCreate(req, resp);

    nlohmann::json j = nlohmann::json::parse(resp.body);
    REQUIRE(j["hyperparameters"]["n_epochs"] == 3);
    REQUIRE(j["seed"] == 0);
    REQUIRE(j["suffix"] == "");
}

TEST_CASE("HandleFineTuningJobsCreate rejects invalid JSON", "[route][finetune]") {
    httplib::Request req;
    req.body = "not json";

    httplib::Response resp;
    inferdeck::gateway::routes::HandleFineTuningJobsCreate(req, resp);

    REQUIRE(resp.status == 400);
}

TEST_CASE("HandleFineTuningJobsCreate rejects missing model", "[route][finetune]") {
    httplib::Request req;
    nlohmann::json body;
    body["training_file"] = "train.jsonl";
    req.body = body.dump();

    httplib::Response resp;
    inferdeck::gateway::routes::HandleFineTuningJobsCreate(req, resp);

    REQUIRE(resp.status == 400);
    nlohmann::json j = nlohmann::json::parse(resp.body);
    REQUIRE(j["error"]["message"].get<std::string>().find("model") != std::string::npos);
}

TEST_CASE("HandleFineTuningJobsGet returns job status", "[route][finetune]") {
    httplib::Request req;
    req.path = "/v1/fine_tuning/jobs/ft_12345";

    httplib::Response resp;
    inferdeck::gateway::routes::HandleFineTuningJobsGet(req, resp);

    REQUIRE(resp.status == 200);
    nlohmann::json j = nlohmann::json::parse(resp.body);
    REQUIRE(j["id"] == "ft_12345");
    REQUIRE(j["object"] == "fine_tuning.job");
    REQUIRE(j["status"] == "queued");
}

TEST_CASE("HandleFineTuningJobsCancel returns cancelled confirmation", "[route][finetune]") {
    httplib::Request req;
    req.path = "/v1/fine_tuning/jobs/ft_67890/cancel";

    httplib::Response resp;
    inferdeck::gateway::routes::HandleFineTuningJobsCancel(req, resp);

    REQUIRE(resp.status == 200);
    nlohmann::json j = nlohmann::json::parse(resp.body);
    REQUIRE(j["id"] == "ft_67890");
    REQUIRE(j["cancelled"] == true);
}
