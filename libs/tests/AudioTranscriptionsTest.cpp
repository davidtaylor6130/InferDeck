/// @file test_audio_transcriptions.cpp
/// @brief Unit tests for AudioTranscriptions route handler.

#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>
#include "routes/AudioTranscriptions.hpp"
#include <nlohmann/json.hpp>

TEST_CASE("ValidateAudioRequest rejects non-multipart content type", "[route][audio]") {
    httplib::Request req;
    req.set_header("Content-Type", "application/json");
    httplib::Response resp;

    std::string result = inferdeck::gateway::routes::ValidateAudioRequest(req, false);
    REQUIRE(!result.empty());
}

TEST_CASE("ValidateAudioRequest rejects missing file field", "[route][audio]") {
    httplib::Request req;
    req.set_header("Content-Type", "multipart/form-data");
    httplib::Response resp;

    std::string result = inferdeck::gateway::routes::ValidateAudioRequest(req, false);
    REQUIRE(!result.empty());
    REQUIRE(result == "missing_file");
}

TEST_CASE("AudioTranscriptions handles valid transcription request", "[route][audio]") {
    httplib::Request req;
    req.set_header("Content-Type", "multipart/form-data");
    req.set_param_value("model", "whisper-1");
    req.set_param_value("language", "es");

    httplib::Response resp;
    inferdeck::gateway::routes::HandleAudioTranscriptions(req, resp);

    REQUIRE(resp.status == 200);
    nlohmann::json j = nlohmann::json::parse(resp.body);
    REQUIRE(j["task"] == "transcribe");
    REQUIRE(j["language"] == "es");
    REQUIRE(j.contains("text"));
    REQUIRE(j["model"] == "whisper-1");
}

TEST_CASE("AudioTranscriptions default model is whisper", "[route][audio]") {
    httplib::Request req;
    req.set_header("Content-Type", "multipart/form-data");
    httplib::Response resp;

    inferdeck::gateway::routes::HandleAudioTranscriptions(req, resp);

    nlohmann::json j = nlohmann::json::parse(resp.body);
    REQUIRE(j["model"] == "whisper");
}

TEST_CASE("AudioTranscriptions verbose_json format", "[route][audio]") {
    httplib::Request req;
    req.set_header("Content-Type", "multipart/form-data");
    req.set_param_value("response_format", "verbose_json");
    httplib::Response resp;

    inferdeck::gateway::routes::HandleAudioTranscriptions(req, resp);

    nlohmann::json j = nlohmann::json::parse(resp.body);
    REQUIRE(j.contains("segments"));
    REQUIRE(j.contains("no_speech_prob"));
}

TEST_CASE("AudioTranslations handles valid translation request", "[route][audio]") {
    httplib::Request req;
    req.set_header("Content-Type", "multipart/form-data");
    httplib::Response resp;

    inferdeck::gateway::routes::HandleAudioTranslations(req, resp);

    REQUIRE(resp.status == 200);
    nlohmann::json j = nlohmann::json::parse(resp.body);
    REQUIRE(j["task"] == "translate");
    REQUIRE(j["language"] == "en");
    REQUIRE(j.contains("text"));
}

TEST_CASE("AudioTranslations rejects invalid content type", "[route][audio]") {
    httplib::Request req;
    req.set_header("Content-Type", "application/json");
    httplib::Response resp;

    inferdeck::gateway::routes::HandleAudioTranslations(req, resp);

    REQUIRE(resp.status == 400);
    nlohmann::json j = nlohmann::json::parse(resp.body);
    REQUIRE(j["error"]["message"] == "Content-Type must be multipart/form-data");
}

TEST_CASE("AudioTranslations rejects missing file", "[route][audio]") {
    httplib::Request req;
    req.set_header("Content-Type", "multipart/form-data");
    httplib::Response resp;

    inferdeck::gateway::routes::HandleAudioTranslations(req, resp);

    REQUIRE(resp.status == 400);
    nlohmann::json j = nlohmann::json::parse(resp.body);
    REQUIRE(j["error"]["type"] == "invalid_request_error");
}
