/// @file test_audio_speech.cpp
/// @brief Unit tests for AudioSpeech route handler.

#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>
#include "routes/AudioSpeech.hpp"
#include <nlohmann/json.hpp>

TEST_CASE("ValidateSpeechRequest accepts valid input", "[route][tts]") {
    nlohmann::json body;
    body["input"] = "Hello world";
    body["model"] = "piper";

    std::string error = inferdeck::gateway::routes::ValidateSpeechRequest(body);
    REQUIRE(error.empty());
}

TEST_CASE("ValidateSpeechRequest rejects missing input", "[route][tts]") {
    nlohmann::json body;
    body["model"] = "piper";

    std::string error = inferdeck::gateway::routes::ValidateSpeechRequest(body);
    REQUIRE(!error.empty());
    REQUIRE(error.find("input") != std::string::npos);
}

TEST_CASE("ValidateSpeechRequest rejects empty input", "[route][tts]") {
    nlohmann::json body;
    body["input"] = "";
    body["model"] = "piper";

    std::string error = inferdeck::gateway::routes::ValidateSpeechRequest(body);
    REQUIRE(!error.empty());
}

TEST_CASE("ValidateSpeechRequest rejects missing model", "[route][tts]") {
    nlohmann::json body;
    body["input"] = "Hello";

    std::string error = inferdeck::gateway::routes::ValidateSpeechRequest(body);
    REQUIRE(!error.empty());
    REQUIRE(error.find("model") != std::string::npos);
}

TEST_CASE("HandleAudioSpeech handles valid request", "[route][tts]") {
    httplib::Request req;
    nlohmann::json body;
    body["input"] = "Hello world";
    body["model"] = "piper";
    body["voice"] = "alloy";
    body["response_format"] = "wav";

    req.body = body.dump();

    httplib::Response resp;
    inferdeck::gateway::routes::HandleAudioSpeech(req, resp);

    REQUIRE(resp.status == 200);
    REQUIRE(resp.content_type == "audio/wav");
}

TEST_CASE("HandleAudioSpeech rejects invalid JSON body", "[route][tts]") {
    httplib::Request req;
    req.body = "not json";

    httplib::Response resp;
    inferdeck::gateway::routes::HandleAudioSpeech(req, resp);

    REQUIRE(resp.status == 400);
    nlohmann::json j = nlohmann::json::parse(resp.body);
    REQUIRE(j["error"]["message"] == "Invalid JSON body");
}

TEST_CASE("HandleAudioSpeech rejects missing input", "[route][tts]") {
    httplib::Request req;
    nlohmann::json body;
    body["model"] = "piper";
    req.body = body.dump();

    httplib::Response resp;
    inferdeck::gateway::routes::HandleAudioSpeech(req, resp);

    REQUIRE(resp.status == 400);
    nlohmann::json j = nlohmann::json::parse(resp.body);
    REQUIRE(j["error"]["type"] == "invalid_request_error");
}

TEST_CASE("HandleAudioSpeech rejects missing model", "[route][tts]") {
    httplib::Request req;
    nlohmann::json body;
    body["input"] = "Hello";
    req.body = body.dump();

    httplib::Response resp;
    inferdeck::gateway::routes::HandleAudioSpeech(req, resp);

    REQUIRE(resp.status == 400);
}

TEST_CASE("HandleAudioSpeech supports mp3 format", "[route][tts]") {
    httplib::Request req;
    nlohmann::json body;
    body["input"] = "test";
    body["model"] = "piper";
    body["response_format"] = "mp3";
    req.body = body.dump();

    httplib::Response resp;
    inferdeck::gateway::routes::HandleAudioSpeech(req, resp);

    REQUIRE(resp.status == 200);
    REQUIRE(resp.content_type == "audio/mp3");
}
