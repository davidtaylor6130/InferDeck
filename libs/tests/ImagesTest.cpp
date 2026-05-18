/// @file test_images.cpp
/// @brief Unit tests for Images route handler.

#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>
#include "routes/Images.hpp"
#include <nlohmann/json.hpp>

TEST_CASE("ValidateImageRequest accepts valid input", "[route][image]") {
    nlohmann::json body;
    body["prompt"] = "a beautiful sunset";

    std::string error = inferdeck::gateway::routes::ValidateImageRequest(body);
    REQUIRE(error.empty());
}

TEST_CASE("ValidateImageRequest rejects empty prompt", "[route][image]") {
    nlohmann::json body;
    body["prompt"] = "";

    std::string error = inferdeck::gateway::routes::ValidateImageRequest(body);
    REQUIRE(!error.empty());
}

TEST_CASE("ValidateImageRequest rejects missing prompt", "[route][image]") {
    nlohmann::json body;

    std::string error = inferdeck::gateway::routes::ValidateImageRequest(body);
    REQUIRE(!error.empty());
}

TEST_CASE("HandleImageGenerate handles valid txt2img request", "[route][image]") {
    httplib::Request req;
    nlohmann::json body;
    body["prompt"] = "a cat sitting on a mat";
    body["model"] = "sdxl";
    body["size"] = 1024;
    body["n"] = 2;
    req.body = body.dump();

    httplib::Response resp;
    inferdeck::gateway::routes::HandleImageGenerate(req, resp);

    REQUIRE(resp.status == 200);
    nlohmann::json j = nlohmann::json::parse(resp.body);
    REQUIRE(j.contains("data"));
    REQUIRE(j["data"].size() == 2);
    REQUIRE(j["data"][0].contains("url"));
}

TEST_CASE("HandleImageGenerate handles img2img request", "[route][image]") {
    httplib::Request req;
    nlohmann::json body;
    body["prompt"] = "modify this image";
    body["image"] = "input.png";
    body["img2img"] = true;
    req.body = body.dump();

    httplib::Response resp;
    inferdeck::gateway::routes::HandleImageGenerate(req, resp);

    REQUIRE(resp.status == 200);
    nlohmann::json j = nlohmann::json::parse(resp.body);
    REQUIRE(j.contains("data"));
    REQUIRE(j["data"][0].contains("url"));
}

TEST_CASE("HandleImageGenerate rejects img2img without image field", "[route][image]") {
    httplib::Request req;
    nlohmann::json body;
    body["prompt"] = "modify";
    body["img2img"] = true;
    req.body = body.dump();

    httplib::Response resp;
    inferdeck::gateway::routes::HandleImageGenerate(req, resp);

    REQUIRE(resp.status == 400);
    nlohmann::json j = nlohmann::json::parse(resp.body);
    REQUIRE(j["error"]["message"].get<std::string>().find("image") != std::string::npos);
}

TEST_CASE("HandleImageGenerate rejects invalid JSON", "[route][image]") {
    httplib::Request req;
    req.body = "not json";

    httplib::Response resp;
    inferdeck::gateway::routes::HandleImageGenerate(req, resp);

    REQUIRE(resp.status == 400);
}

TEST_CASE("HandleImageGenerate rejects empty prompt", "[route][image]") {
    httplib::Request req;
    nlohmann::json body;
    body["prompt"] = "";
    req.body = body.dump();

    httplib::Response resp;
    inferdeck::gateway::routes::HandleImageGenerate(req, resp);

    REQUIRE(resp.status == 400);
}

TEST_CASE("HandleImageGenerate b64_json format", "[route][image]") {
    httplib::Request req;
    nlohmann::json body;
    body["prompt"] = "test";
    body["response_format"] = "b64_json";
    req.body = body.dump();

    httplib::Response resp;
    inferdeck::gateway::routes::HandleImageGenerate(req, resp);

    nlohmann::json j = nlohmann::json::parse(resp.body);
    REQUIRE(j["data"][0].contains("b64_json"));
}
