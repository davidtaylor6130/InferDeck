/// @file test_tts_backend.cpp
/// @brief Tests for text-to-speech backend interface and mock implementation.

#include <catch2/catch_test_macros.hpp>
#include "backends/ITtsBackend.hpp"
#include "backends/ITextBackend.hpp"
#include <memory>

namespace {

using namespace inferdeck::backends;

class MockTtsBackend : public ITtsBackend {
public:
    std::string GetBackendName() const override { return "mock_piper"; }
    BackendStatus GetStatus() const override { return status_; }
    bool Initialize() override { status_ = BackendStatus::READY; return true; }
    void Shutdown() override { status_ = BackendStatus::UNINITIALIZED; }
    nlohmann::json GetInfo() const override { return nlohmann::json{{"name", "mock_piper"}}; }
    nlohmann::json GetVRAMUsage() const override { return nlohmann::json{{"used", 0}}; }
    bool IsReady() const override { return status_ == BackendStatus::READY; }

    std::vector<uint8_t> Synthesize(const std::string& text, const nlohmann::json&) override {
        return std::vector<uint8_t>(text.size() * 10, 0xAA);
    }
    nlohmann::json GetAvailableVoices() const override {
        return nlohmann::json{{"alloy", "neutral"}, {"echo", "warm"}, {"fable", "bright"}};
    }
    bool SetVoice(const std::string& voice_id) override {
        current_voice_ = voice_id;
        return true;
    }
    std::string GetVoiceId() const override { return current_voice_; }
    std::vector<TTSSegment> SegmentText(const std::string& text) override {
        return {{text, 0, static_cast<int>(text.length()) * 50, 0.5f, 1.0f}};
    }

    BackendStatus status_ = BackendStatus::UNINITIALIZED;
    std::string current_voice_ = "alloy";
};

} // namespace

TEST_CASE("TTS: Mock backend name and status", "[tts][mock]") {
    MockTtsBackend backend;
    REQUIRE(backend.GetBackendName() == "mock_piper");
    REQUIRE(backend.GetStatus() == BackendStatus::UNINITIALIZED);
    REQUIRE(!backend.IsReady());
}

TEST_CASE("TTS: Initialize and shutdown", "[tts][mock]") {
    MockTtsBackend backend;
    REQUIRE(backend.Initialize());
    REQUIRE(backend.IsReady());

    backend.Shutdown();
    REQUIRE(!backend.IsReady());
}

TEST_CASE("TTS: Synthesize returns WAV bytes", "[tts][mock]") {
    MockTtsBackend backend;
    backend.Initialize();

    auto wav = backend.Synthesize("Hello world", {});
    REQUIRE(wav.size() > 0);
    REQUIRE(wav.size() == 50); // 10 chars * 10 bytes per char
    REQUIRE(wav[0] == 0xAA);
}

TEST_CASE("TTS: GetAvailableVoices returns object", "[tts][mock]") {
    MockTtsBackend backend;
    auto voices = backend.GetAvailableVoices();
    REQUIRE(voices.is_object());
    REQUIRE(voices.contains("alloy"));
    REQUIRE(voices.contains("echo"));
    REQUIRE(voices.contains("fable"));
}

TEST_CASE("TTS: SetVoice changes current voice", "[tts][mock]") {
    MockTtsBackend backend;
    backend.Initialize();

    REQUIRE(backend.SetVoice("echo"));
    REQUIRE(backend.GetVoiceId() == "echo");

    REQUIRE(backend.SetVoice("fable"));
    REQUIRE(backend.GetVoiceId() == "fable");
}

TEST_CASE("TTS: Default voice is alloy", "[tts][mock]") {
    MockTtsBackend backend;
    REQUIRE(backend.GetVoiceId() == "alloy");
}

TEST_CASE("TTS: SegmentText creates segments", "[tts][mock]") {
    MockTtsBackend backend;
    backend.Initialize();

    auto segments = backend.SegmentText("Hello world");
    REQUIRE(segments.size() == 1);
    REQUIRE(segments[0].text == "Hello world");
    REQUIRE(segments[0].start_ms == 0);
    REQUIRE(segments[0].end_ms > 0);
    REQUIRE(segments[0].pitch == 0.5f);
    REQUIRE(segments[0].speed == 1.0f);
}

TEST_CASE("TTS: Synthesize with voice parameter", "[tts][mock]") {
    MockTtsBackend backend;
    backend.Initialize();

    nlohmann::json params;
    params["voice"] = "echo";
    backend.SetVoice("echo");

    auto wav = backend.Synthesize("test", params);
    REQUIRE(wav.size() == 40); // 4 chars * 10
    REQUIRE(backend.GetVoiceId() == "echo");
}

TEST_CASE("TTS: Cast to ITextBackend interface", "[tts][mock]") {
    std::unique_ptr<ITextBackend> base = std::make_unique<MockTtsBackend>();
    REQUIRE(base->GetBackendName() == "mock_piper");
    REQUIRE(base->Initialize());
    REQUIRE(base->IsReady());
    base->Shutdown();
}

TEST_CASE("TTS: Cast to ITtsBackend interface", "[tts][mock]") {
    std::unique_ptr<ITtsBackend> tts = std::make_unique<MockTtsBackend>();
    REQUIRE(tts->GetBackendName() == "mock_piper");
    auto wav = tts->Synthesize("test", {});
    REQUIRE(!wav.empty());
}
