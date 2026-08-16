#include <catch2/catch_test_macros.hpp>

#include "model/imodel.hpp"
#include "model/model_registry.hpp"
#include "native_runtimes/runtime_factories.hpp"

#include <cstdlib>
#include <cstring>
#include <string>

namespace {

std::uint32_t u32(const std::vector<std::byte>& data, std::size_t offset) {
    return static_cast<std::uint32_t>(
        std::to_integer<unsigned char>(data[offset]) |
        std::to_integer<unsigned char>(data[offset + 1]) << 8 |
        std::to_integer<unsigned char>(data[offset + 2]) << 16 |
        std::to_integer<unsigned char>(data[offset + 3]) << 24);
}

}

TEST_CASE("Supertonic GPU providers require VRAM accounting",
          "[native-runtimes][sherpa-onnx][supertonic]") {
    inferdeck::model::ModelRegistry registry;
    inferdeck::native_runtimes::register_factories(registry);
    inferdeck::model::ModelInfo info;
    info.name = "supertonic-gpu-unaccounted";
    info.runtime = "sherpa_onnx";
    info.modality = "audio_speech";
    info.artifacts["engine"] = "supertonic";
    info.artifacts["provider"] = "cuda";
    info.vram_required_mb = 0;
    registry.register_model(info);

    auto created = registry.create_result(info.name);
    REQUIRE(created);
    const auto loaded = created.value()->load();
    REQUIRE_FALSE(loaded);
    CHECK(loaded.error().code == inferdeck::foundation::ErrorCode::InvalidArgument);
    CHECK(loaded.error().message.find("VRAM accounting") != std::string::npos);
}

TEST_CASE("Supertonic synthesizes a real WAV in-process",
          "[native-runtimes][sherpa-onnx][supertonic][integration]") {
    const char* model_dir = std::getenv("INFERDECK_SUPERTONIC_TEST_MODEL_DIR");
    if (!model_dir || !*model_dir) {
        SKIP("real Supertonic model path is not configured");
    }

    const std::string root = model_dir;
    inferdeck::model::ModelRegistry registry;
    inferdeck::native_runtimes::register_factories(registry);
    REQUIRE(registry.has_factory("sherpa_onnx"));

    inferdeck::model::ModelInfo info;
    info.name = "supertonic-integration";
    info.runtime = "sherpa_onnx";
    info.modality = "audio_speech";
    info.capabilities = {"audio_speech"};
    info.artifacts = {
        {"engine", "supertonic"},
        {"duration_predictor", root + "/duration_predictor.int8.onnx"},
        {"text_encoder", root + "/text_encoder.int8.onnx"},
        {"vector_estimator", root + "/vector_estimator.int8.onnx"},
        {"vocoder", root + "/vocoder.int8.onnx"},
        {"tts_json", root + "/tts.json"},
        {"unicode_indexer", root + "/unicode_indexer.bin"},
        {"voice_style", root + "/voice.bin"},
        {"provider", "cpu"},
        {"num_threads", "4"},
    };
    registry.register_model(info);

    auto created = registry.create_result(info.name);
    REQUIRE(created);
    auto backend = std::move(created.value());
    REQUIRE(backend->load());
    auto* speech =
        dynamic_cast<inferdeck::model::ISpeechBackend*>(backend.get());
    REQUIRE(speech);

    inferdeck::model::SpeechRequest request;
    request.input = "InferDeck neural speech is ready.";
    request.voice = "alloy";
    request.format = "wav";
    request.speed = 1.0f;
    auto result = speech->synthesize(0, request, {});
    REQUIRE(result);
    REQUIRE(result->content_type == "audio/wav");
    REQUIRE(result->bytes.size() > 44);
    REQUIRE(std::memcmp(result->bytes.data(), "RIFF", 4) == 0);
    REQUIRE(std::memcmp(result->bytes.data() + 8, "WAVE", 4) == 0);
    CHECK(u32(result->bytes, 24) == 44100);
    CHECK(result->duration_ms > 0.0f);

    request.voice = "not-a-voice";
    auto invalid = speech->synthesize(0, request, {});
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error().code ==
          inferdeck::foundation::ErrorCode::InvalidArgument);
    REQUIRE(backend->unload());
}
