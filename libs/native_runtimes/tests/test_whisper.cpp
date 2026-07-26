#include <catch2/catch_test_macros.hpp>

#include "model/imodel.hpp"
#include "model/model_registry.hpp"
#include "native_runtimes/runtime_factories.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

std::uint16_t u16(const char* data) {
    return static_cast<std::uint16_t>(static_cast<unsigned char>(data[0]) |
                                      static_cast<unsigned char>(data[1]) << 8);
}

std::uint32_t u32(const char* data) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(data[0]) |
                                      static_cast<unsigned char>(data[1]) << 8 |
                                      static_cast<unsigned char>(data[2]) << 16 |
                                      static_cast<unsigned char>(data[3]) << 24);
}

inferdeck::model::TranscriptionRequest load_wav(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input);
    const std::string content{std::istreambuf_iterator<char>(input),
                              std::istreambuf_iterator<char>()};
    REQUIRE(content.size() >= 44);
    REQUIRE(std::memcmp(content.data(), "RIFF", 4) == 0);
    REQUIRE(std::memcmp(content.data() + 8, "WAVE", 4) == 0);

    std::uint16_t format = 0;
    std::uint16_t channels = 0;
    std::uint16_t bits = 0;
    std::uint32_t sample_rate = 0;
    const char* samples = nullptr;
    std::size_t sample_bytes = 0;
    for (std::size_t position = 12; position + 8 <= content.size();) {
        const char* chunk = content.data() + position;
        const auto size = u32(chunk + 4);
        REQUIRE(position + 8ULL + size <= content.size());
        if (std::memcmp(chunk, "fmt ", 4) == 0 && size >= 16) {
            format = u16(chunk + 8);
            channels = u16(chunk + 10);
            sample_rate = u32(chunk + 12);
            bits = u16(chunk + 22);
        } else if (std::memcmp(chunk, "data", 4) == 0) {
            samples = chunk + 8;
            sample_bytes = size;
        }
        position += 8 + size + (size & 1U);
    }
    REQUIRE(format == 1);
    REQUIRE(channels > 0);
    REQUIRE(bits == 16);
    REQUIRE(sample_rate > 0);
    REQUIRE(samples);

    const std::size_t frame_size = channels * 2;
    REQUIRE(sample_bytes % frame_size == 0);
    const std::size_t frames = sample_bytes / frame_size;
    inferdeck::model::TranscriptionRequest request;
    request.sample_rate = static_cast<int>(sample_rate);
    request.language = "en";
    request.pcm.resize(frames);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        float mixed = 0.0f;
        for (std::size_t channel = 0; channel < channels; ++channel) {
            const char* value = samples + frame * frame_size + channel * 2;
            mixed += static_cast<float>(static_cast<std::int16_t>(u16(value))) / 32768.0f;
        }
        request.pcm[frame] = mixed / channels;
    }
    return request;
}

}

TEST_CASE("Whisper runtime transcribes a real WAV in-process", "[native-runtimes][whisper][integration]") {
    const char* model_path = std::getenv("INFERDECK_WHISPER_TEST_MODEL");
    const char* audio_path = std::getenv("INFERDECK_WHISPER_TEST_AUDIO");
    if (!model_path || !*model_path || !audio_path || !*audio_path) {
        SKIP("real Whisper model and audio paths are not configured");
    }

    inferdeck::model::ModelRegistry registry;
    inferdeck::native_runtimes::register_factories(registry);
    REQUIRE(registry.has_factory("whisper_cpp"));

    inferdeck::model::ModelInfo info;
    info.name = "whisper-integration";
    info.runtime = "whisper_cpp";
    info.modality = "audio_transcription";
    info.capabilities = {"audio_transcription"};
    info.artifacts["model"] = model_path;
    registry.register_model(info);

    auto created = registry.create_result(info.name);
    REQUIRE(created);
    auto backend = std::move(created.value());
    REQUIRE(backend->load());
    REQUIRE(backend->load());
    auto* transcription = dynamic_cast<inferdeck::model::ITranscriptionBackend*>(backend.get());
    REQUIRE(transcription);

    auto result = transcription->transcribe(0, load_wav(audio_path));
    REQUIRE(result);
    REQUIRE_FALSE(result->text.empty());
    REQUIRE(result->language == "en");
    REQUIRE(result->duration_seconds > 0.0f);
    REQUIRE(result->inference_ms > 0.0f);
    REQUIRE_FALSE(result->segments.empty());
    CHECK(result->text.find("country") != std::string::npos);
    REQUIRE(backend->unload());
}
