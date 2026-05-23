#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include "WhisperRuntime.hpp"
#include "routes/AudioTranscriptions.hpp"

#include <fstream>
#include <filesystem>
#include <nlohmann/json.hpp>

namespace {

std::filesystem::path WriteFakeWhisperCli(const std::filesystem::path& dir) {
    auto script = dir / "fake-whisper.cmd";
    std::ofstream file(script);
    file << "@echo off\n";
    file << "set out=\n";
    file << ":loop\n";
    file << "if \"%~1\"==\"\" goto done\n";
    file << "if \"%~1\"==\"-of\" set out=%~2\n";
    file << "shift\n";
    file << "goto loop\n";
    file << ":done\n";
    file << "echo local whisper ok>\"%out%.txt\"\n";
    file << "exit /b 0\n";
    return script;
}

httplib::Request MultipartAudioRequest() {
    httplib::Request req;
    req.set_header("Content-Type", "multipart/form-data; boundary=inferdeck");
    req.form.files.emplace("file", httplib::FormData{"file", "RIFF....WAVEfmt ", "sample.wav", "audio/wav", {}});
    req.form.fields.emplace("model", httplib::FormField{"model", "whisper-1", {}});
    req.form.fields.emplace("language", httplib::FormField{"language", "en", {}});
    return req;
}

void ConfigureFakeRuntime() {
    auto dir = std::filesystem::temp_directory_path() / "inferdeck-whisper-test";
    std::filesystem::create_directories(dir);
    auto model = dir / "ggml-base.en.bin";
    std::ofstream(model) << "fake model";

    inferdeck::gateway::ServerConfig config;
    config.whisper_enabled = true;
    config.whisper_executable = WriteFakeWhisperCli(dir).string();
    config.whisper_model_directory = dir.string();
    config.whisper_model = model.string();
    config.whisper_backend = "vulkan";
    config.whisper_language = "auto";
    inferdeck::gateway::WhisperRuntime::Get().Configure(config);
}

} // namespace

TEST_CASE("ValidateAudioRequest rejects non-multipart content type", "[route][audio]") {
    httplib::Request req;
    req.set_header("Content-Type", "application/json");

    std::string result = inferdeck::gateway::routes::ValidateAudioRequest(req, false);
    REQUIRE(result == "invalid_content_type");
}

TEST_CASE("ValidateAudioRequest rejects missing file field", "[route][audio]") {
    httplib::Request req;
    req.set_header("Content-Type", "multipart/form-data; boundary=inferdeck");

    std::string result = inferdeck::gateway::routes::ValidateAudioRequest(req, true);
    REQUIRE(result == "missing_file");
}

TEST_CASE("AudioTranscriptions returns OpenAI-compatible JSON from local Whisper runtime", "[route][audio]") {
    ConfigureFakeRuntime();
    auto req = MultipartAudioRequest();
    httplib::Response resp;

    inferdeck::gateway::routes::HandleAudioTranscriptions(req, resp);

    REQUIRE(resp.status == 200);
    auto body = nlohmann::json::parse(resp.body);
    REQUIRE(body["task"] == "transcribe");
    REQUIRE(body["language"] == "en");
    REQUIRE(body["text"] == "local whisper ok");
    REQUIRE(body["model"] == "whisper-1");
}

TEST_CASE("AudioTranscriptions supports verbose_json response shape", "[route][audio]") {
    ConfigureFakeRuntime();
    auto req = MultipartAudioRequest();
    req.form.fields.emplace("response_format", httplib::FormField{"response_format", "verbose_json", {}});
    httplib::Response resp;

    inferdeck::gateway::routes::HandleAudioTranscriptions(req, resp);

    REQUIRE(resp.status == 200);
    auto body = nlohmann::json::parse(resp.body);
    REQUIRE(body.contains("segments"));
    REQUIRE(body.contains("no_speech_prob"));
}

TEST_CASE("AudioTranslations returns English translation response shape", "[route][audio]") {
    ConfigureFakeRuntime();
    auto req = MultipartAudioRequest();
    httplib::Response resp;

    inferdeck::gateway::routes::HandleAudioTranslations(req, resp);

    REQUIRE(resp.status == 200);
    auto body = nlohmann::json::parse(resp.body);
    REQUIRE(body["task"] == "translate");
    REQUIRE(body["language"] == "en");
    REQUIRE(body["text"] == "local whisper ok");
}

TEST_CASE("AudioTranslations rejects missing file", "[route][audio]") {
    httplib::Request req;
    req.set_header("Content-Type", "multipart/form-data; boundary=inferdeck");
    httplib::Response resp;

    inferdeck::gateway::routes::HandleAudioTranslations(req, resp);

    REQUIRE(resp.status == 400);
    auto body = nlohmann::json::parse(resp.body);
    REQUIRE(body["error"]["code"] == "missing_file");
}
