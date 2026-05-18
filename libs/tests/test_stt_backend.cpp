/// @file test_stt_backend.cpp
/// @brief Tests for speech-to-text backend interface and mock implementation.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include "backends/ISttBackend.hpp"
#include "backends/ITextBackend.hpp"
#include <memory>

namespace {

using namespace inferdeck::backends;

class MockSttBackend : public ISttBackend {
public:
    std::string GetBackendName() const override { return "mock_whisper"; }
    BackendStatus GetStatus() const override { return status_; }
    bool Initialize() override { status_ = BackendStatus::READY; return true; }
    void Shutdown() override { status_ = BackendStatus::UNINITIALIZED; }
    nlohmann::json GetInfo() const override { return nlohmann::json{{"name", "mock_whisper"}}; }
    nlohmann::json GetVRAMUsage() const override { return nlohmann::json{{"used", 0}}; }
    bool IsReady() const override { return status_ == BackendStatus::READY; }

    STTResult Transcribe(const std::string&, const nlohmann::json&) override {
        return {"mock transcription", "en", 3.14, 0.95, {0.0, 1.0, 2.0}, {}};
    }
    STTResult Translate(const std::string&, const nlohmann::json&) override {
        return {"mock translation", "en", 2.5, 0.92, {}, {}};
    }
    nlohmann::json GetAvailableLanguages() const override {
        return nlohmann::json{"en", "es", "fr", "de", "ja"};
    }

    BackendStatus status_ = BackendStatus::UNINITIALIZED;
};

} // namespace

TEST_CASE("STT: Mock backend name and status", "[stt][mock]") {
    MockSttBackend backend;
    REQUIRE(backend.GetBackendName() == "mock_whisper");
    REQUIRE(backend.GetStatus() == BackendStatus::UNINITIALIZED);
    REQUIRE(backend.IsReady() == false);
}

TEST_CASE("STT: Initialize and shutdown", "[stt][mock]") {
    MockSttBackend backend;
    REQUIRE(backend.Initialize());
    REQUIRE(backend.GetStatus() == BackendStatus::READY);
    REQUIRE(backend.IsReady() == true);

    backend.Shutdown();
    REQUIRE(backend.GetStatus() == BackendStatus::UNINITIALIZED);
    REQUIRE(backend.IsReady() == false);
}

TEST_CASE("STT: Transcribe returns valid STTResult", "[stt][mock]") {
    MockSttBackend backend;
    backend.Initialize();

    auto result = backend.Transcribe("audio.wav", {});
    REQUIRE(!result.text.empty());
    REQUIRE(!result.language.empty());
    REQUIRE(result.duration_seconds > 0.0);
    REQUIRE(result.confidence > 0.0 && result.confidence <= 1.0);
    REQUIRE(result.timestamps.size() == 3);
    REQUIRE(result.timestamps[0] == 0.0);
    REQUIRE(result.timestamps[2] == 2.0);
}

TEST_CASE("STT: Translate returns English output", "[stt][mock]") {
    MockSttBackend backend;
    backend.Initialize();

    auto result = backend.Translate("audio.wav", {});
    REQUIRE(result.text.find("translation") != std::string::npos);
    REQUIRE(result.language == "en");
}

TEST_CASE("STT: GetAvailableLanguages returns array", "[stt][mock]") {
    MockSttBackend backend;
    auto langs = backend.GetAvailableLanguages();
    REQUIRE(langs.is_array());
    REQUIRE(langs.size() == 5);
    REQUIRE(langs[0] == "en");
    REQUIRE(langs[4] == "ja");
}

TEST_CASE("STT: Transcribe with language parameter", "[stt][mock]") {
    MockSttBackend backend;
    backend.Initialize();

    nlohmann::json params;
    params["language"] = "es";
    auto result = backend.Transcribe("audio.wav", params);
    REQUIRE(!result.text.empty());
    REQUIRE(result.duration_seconds == 3.14);
    REQUIRE(result.confidence == 0.95);
}

TEST_CASE("STT: GetInfo returns valid JSON", "[stt][mock]") {
    MockSttBackend backend;
    auto info = backend.GetInfo();
    REQUIRE(info.is_object());
    REQUIRE(info.contains("name"));
    REQUIRE(info["name"] == "mock_whisper");
}

TEST_CASE("STT: GetVRAMUsage returns valid JSON", "[stt][mock]") {
    MockSttBackend backend;
    auto vram = backend.GetVRAMUsage();
    REQUIRE(vram.is_object());
    REQUIRE(vram.contains("used"));
}

TEST_CASE("STT: Cast to ITextBackend interface", "[stt][mock]") {
    std::unique_ptr<ITextBackend> base = std::make_unique<MockSttBackend>();
    REQUIRE(base->GetBackendName() == "mock_whisper");
    REQUIRE(base->IsReady() == false);
    REQUIRE(base->Initialize());
    REQUIRE(base->IsReady() == true);
    base->Shutdown();
}

TEST_CASE("STT: Cast to ISttBackend interface", "[stt][mock]") {
    std::unique_ptr<ISttBackend> stt = std::make_unique<MockSttBackend>();
    REQUIRE(stt->GetBackendName() == "mock_whisper");

    auto result = stt->Transcribe("test.wav", {});
    REQUIRE(!result.text.empty());
}
