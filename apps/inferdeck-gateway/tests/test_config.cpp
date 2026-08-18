#include <catch2/catch_test_macros.hpp>

#include "config.hpp"

#include <algorithm>

using inferdeck::gateway::validate_config_text;
using inferdeck::gateway::load_config;
using inferdeck::gateway::load_config_with_active;
using inferdeck::gateway::active_config_path_for;

TEST_CASE("Gateway configuration rejects a missing explicit file", "[config]") {
    const auto missing = std::filesystem::temp_directory_path() /
        "inferdeck-config-that-does-not-exist.yml";
    std::error_code error;
    std::filesystem::remove(missing, error);
    CHECK_THROWS_AS(load_config(missing), std::runtime_error);
}

TEST_CASE("Gateway configuration accepts native runtime artifacts", "[config]") {
    auto result = validate_config_text(R"(
server:
  port: 11434
default_model: image
model_registry:
  - name: image
    runtime: stable_diffusion_cpp
    modality: image
    artifacts:
      model: C:/models/image.gguf
    n_slots: 1
)");
    REQUIRE(result);
}

TEST_CASE("Gateway configuration accepts Windows SAPI without artifacts", "[config]") {
    auto result = validate_config_text(R"(
server:
  port: 11434
default_model: windows-sapi
model_registry:
  - name: windows-sapi
    runtime: windows_sapi
    modality: audio_speech
    n_slots: 1
)");
    REQUIRE(result);
}

TEST_CASE("Repository gateway configuration preserves home-lab LAN access", "[config][lan]") {
    const auto path = std::filesystem::path(INFERDECK_SOURCE_DIR) /
        "config" / "gateway.yml";
    const auto text = inferdeck::gateway::read_text_file(path);
    REQUIRE_FALSE(text.empty());
    REQUIRE(validate_config_text(text));

    const auto config = load_config(path);
    CHECK(config.host == "0.0.0.0");
    CHECK_FALSE(config.auth_required);
    REQUIRE(config.cors_origins.size() == 1);
    CHECK(config.cors_origins.front() == "*");
}

TEST_CASE("Repository gateway configuration keeps statistics in the installed runtime",
          "[config][stats-path]") {
    const auto path = std::filesystem::path(INFERDECK_SOURCE_DIR) /
        "config" / "gateway.yml";
    const auto config = load_config(path);
    CHECK(config.stats_db_path == "C:/InferDeck/data/stats.db");
}

TEST_CASE("Repository retains the tuned Gemma 31B profile",
          "[config][gemma4]") {
    const auto path = std::filesystem::path(INFERDECK_SOURCE_DIR) /
        "config" / "gateway.yml";
    const auto config = load_config(path);
    const auto gemma = std::find_if(
        config.models.begin(), config.models.end(),
        [](const auto& model) { return model.name == "gemma-4-31b"; });
    REQUIRE(gemma != config.models.end());
    CHECK(gemma->gguf_path ==
          "C:/Inferdeck/models/unsloth/gemma-4-31B-it-GGUF/"
          "gemma-4-31B-it-UD-Q4_K_XL.gguf");
    CHECK(gemma->n_slots == 1);
    CHECK(gemma->context_size == 262144);
    CHECK(gemma->vram_required_mb == 29000);
    CHECK_FALSE(gemma->mtp_enabled);
}

TEST_CASE("Repository Qwen 3.8 27B profile enables adaptive MTP",
          "[config][mtp]") {
    const auto path = std::filesystem::path(INFERDECK_SOURCE_DIR) /
        "config" / "gateway.yml";
    const auto config = load_config(path);
    CHECK(config.default_model == "qwen3.8-27b");
    const auto qwen = std::find_if(
        config.models.begin(), config.models.end(),
        [](const auto& model) { return model.name == "qwen3.8-27b"; });
    REQUIRE(qwen != config.models.end());
    CHECK(qwen->family == "qwen3.8");
    REQUIRE(qwen->prompt_price_per_million);
    REQUIRE(qwen->cached_prompt_price_per_million);
    REQUIRE(qwen->completion_price_per_million);
    CHECK(*qwen->prompt_price_per_million == 0.45);
    CHECK(*qwen->cached_prompt_price_per_million == 0.05);
    CHECK(*qwen->completion_price_per_million == 3.20);
    CHECK(qwen->gguf_path ==
          "E:/InferDeck/models/unsloth/Qwen3.8-27B-GGUF/"
          "Qwen3.8-27B-Q4_K_M.gguf");
    CHECK(qwen->mmproj_path ==
          "E:/InferDeck/models/unsloth/Qwen3.8-27B-GGUF/"
          "mmproj-F16.gguf");
    CHECK(qwen->n_slots == 4);
    CHECK(qwen->min_slots == 4);
    CHECK(qwen->context_size == 100000);
    CHECK(qwen->vram_required_mb == 30000);
    REQUIRE(qwen->n_batch);
    REQUIRE(qwen->n_ubatch);
    CHECK(*qwen->n_batch == 2048);
    CHECK(*qwen->n_ubatch == 2048);
    CHECK(qwen->cache_type_k == "q4_0");
    CHECK(qwen->cache_type_v == "q4_0");
    CHECK(qwen->reasoning.supported);
    CHECK(qwen->reasoning.efforts ==
          std::vector<std::string>{"low", "medium", "xhigh"});
    CHECK(qwen->reasoning.default_effort == "xhigh");
    CHECK(qwen->reasoning.none_disables);
    CHECK(qwen->reasoning.aliases.at("high") == "xhigh");
    CHECK(qwen->mtp_enabled);
    CHECK(qwen->mtp_draft_tokens == 2);
    CHECK(qwen->mtp_p_min == 0.0f);
    CHECK(qwen->mtp_max_active_requests == 1);
    CHECK(qwen->has_vision);
    CHECK(qwen->sampling.temperature == 1.0f);
    CHECK(qwen->sampling.top_p == 0.95f);
    CHECK(qwen->sampling.top_k == 20);
    CHECK(qwen->sampling.min_p == 0.0f);
    CHECK(qwen->sampling.repeat_penalty == 1.0f);
}

TEST_CASE("Repository Qwen 3.6 27B profile enables the measured adaptive MTP settings",
          "[config][mtp]") {
    const auto path = std::filesystem::path(INFERDECK_SOURCE_DIR) /
        "config" / "gateway.yml";
    const auto config = load_config(path);
    const auto qwen = std::find_if(
        config.models.begin(), config.models.end(),
        [](const auto& model) { return model.name == "qwen3.6-27b"; });
    REQUIRE(qwen != config.models.end());
    CHECK(qwen->gguf_path ==
          "E:/InferDeck/models/unsloth/Qwen3.6-27B-MTP-GGUF/"
          "Qwen3.6-27B-Q4_K_M.gguf");
    CHECK(qwen->n_slots == 4);
    CHECK(qwen->min_slots == 4);
    CHECK(qwen->context_size == 100000);
    CHECK(qwen->vram_required_mb == 29791);
    REQUIRE(qwen->n_batch);
    REQUIRE(qwen->n_ubatch);
    CHECK(*qwen->n_batch == 2048);
    CHECK(*qwen->n_ubatch == 2048);
    CHECK(qwen->cache_type_k == "q4_0");
    CHECK(qwen->cache_type_v == "q4_0");
    CHECK(qwen->mtp_enabled);
    CHECK(qwen->mtp_draft_tokens == 2);
    CHECK(qwen->mtp_p_min == 0.0f);
    CHECK(qwen->mtp_max_active_requests == 1);
    CHECK(qwen->optimization.status == "measured");
    CHECK(qwen->optimization.measured_at == "2026-07-26");
    CHECK(qwen->optimization.quality_passes == 3);
    CHECK(qwen->optimization.quality_total == 3);
    CHECK(qwen->optimization.single_tokens_per_second == 50.16);
    CHECK(qwen->optimization.parallel_tokens_per_second == 51.24);
    CHECK_FALSE(qwen->optimization.schedule_enabled);
    CHECK(qwen->optimization.schedule_window_start == "03:00");
    CHECK(qwen->optimization.schedule_window_end == "04:00");
}

TEST_CASE("Repository gateway configuration exposes native speech models",
          "[config][speech-config]") {
    const auto path = std::filesystem::path(INFERDECK_SOURCE_DIR) /
        "config" / "gateway.yml";
    const auto config = load_config(path);
    const auto neural = std::find_if(
        config.models.begin(), config.models.end(),
        [](const auto& model) { return model.name == "supertonic-3"; });
    REQUIRE(neural != config.models.end());
    CHECK(neural->runtime == "sherpa_onnx");
    CHECK(neural->modality == "audio_speech");
    CHECK(neural->artifacts.at("engine") == "supertonic");
    CHECK(neural->artifacts.at("duration_predictor") ==
          "C:/InferDeck/models/tts/supertonic-3/duration_predictor.int8.onnx");
    CHECK(neural->artifacts.at("voice_style") ==
          "C:/InferDeck/models/tts/supertonic-3/voice.bin");
    CHECK(neural->vram_required_mb == 0);

    const auto transcription = std::find_if(
        config.models.begin(), config.models.end(),
        [](const auto& model) {
            return model.name == "parakeet-tdt-0.6b-v3";
        });
    REQUIRE(transcription != config.models.end());
    CHECK(transcription->runtime == "sherpa_onnx");
    CHECK(transcription->modality == "audio_transcription");
    CHECK(transcription->vram_required_mb == 0);
    CHECK(transcription->artifacts.at("provider") == "cpu");
    CHECK(transcription->artifacts.at("num_threads") == "4");
    CHECK(transcription->artifacts.at("encoder") ==
          "C:/InferDeck/models/stt/parakeet-tdt-0.6b-v3/encoder.int8.onnx");
    CHECK(std::none_of(
        config.models.begin(), config.models.end(),
        [](const auto& model) { return model.runtime == "windows_sapi"; }));

    const auto whisper = std::find_if(
        config.models.begin(), config.models.end(),
        [](const auto& model) { return model.name == "whisper-base-en"; });
    REQUIRE(whisper != config.models.end());
    CHECK(whisper->runtime == "whisper_cpp");
    CHECK(whisper->modality == "audio_transcription");
    CHECK(whisper->vram_required_mb == 0);
    CHECK(whisper->artifacts.at("model") ==
          "E:/InferDeck/models/stt/whisper/ggml-base.en.bin");
    CHECK(whisper->artifacts.at("provider") == "cpu");
    CHECK(whisper->artifacts.at("num_threads") == "4");
}

TEST_CASE("Gateway configuration rejects unsafe operational values", "[config]") {
    CHECK_FALSE(validate_config_text("server:\n  port: 70000\n"));
    CHECK_FALSE(validate_config_text(R"(
model_registry:
  - name: duplicate
    gguf_path: one.gguf
  - name: duplicate
    gguf_path: two.gguf
)"));
    CHECK_FALSE(validate_config_text(R"(
model_registry:
  - name: speech
    runtime: sherpa_onnx
    modality: audio_speech
)"));
    CHECK_FALSE(validate_config_text("gateway:\n  cache_type_k: made_up\n"));
    CHECK_FALSE(validate_config_text("gateway:\n  sampling:\n    temperature: .nan\n"));
    CHECK_FALSE(validate_config_text(R"(
model_registry:
  - name: bad-cached-price
    gguf_path: model.gguf
    cached_prompt_price_per_million: -0.01
)"));
    CHECK_FALSE(validate_config_text("gateway:\n  n_batch: 128\n  n_ubatch: 256\n"));
    CHECK(validate_config_text("server:\n  host: 0.0.0.0\n"));
    CHECK(validate_config_text("server:\n  host: 0.0.0.0\nauth:\n  required: true\n  token: secret\n"));
    CHECK_FALSE(validate_config_text(R"(
model_registry:
  - name: unknown
    runtime: external_proxy
    gguf_path: model.gguf
)"));
    CHECK_FALSE(validate_config_text(R"(
model_registry:
  - name: bad-speech
    runtime: whisper_cpp
    modality: audio_speech
    artifacts:
      model: speech.bin
)"));
    CHECK_FALSE(validate_config_text(R"(
model_registry:
  - name: false-vision
    gguf_path: model.gguf
    has_vision: true
)"));
    CHECK(validate_config_text(R"(
model_registry:
  - name: vision
    gguf_path: model.gguf
    mmproj_path: mmproj.gguf
    has_vision: true
)"));
    CHECK_FALSE(validate_config_text(R"(
model_registry:
  - name: bad-sampling
    gguf_path: model.gguf
    sampling:
      top_p: 1.5
)"));
    CHECK_FALSE(validate_config_text(R"(
model_registry:
  - name: bad-reasoning-default
    gguf_path: model.gguf
    reasoning:
      efforts: [low, high]
      default: medium
)"));
    CHECK_FALSE(validate_config_text(R"(
model_registry:
  - name: bad-reasoning-alias
    gguf_path: model.gguf
    reasoning:
      efforts: [low, high]
      default: high
      aliases:
        xhigh: unsupported
)"));
    CHECK_FALSE(validate_config_text(R"(
model_registry:
  - name: bad-mtp-depth
    gguf_path: model.gguf
    speculative:
      type: mtp
      draft_tokens: 8
)"));
    CHECK_FALSE(validate_config_text(R"(
model_registry:
  - name: bad-mtp-window
    gguf_path: model.gguf
    n_slots: 2
    speculative:
      type: mtp
      max_active_requests: 3
)"));
    CHECK_FALSE(validate_config_text(R"(
model_registry:
  - name: bad-model-batch
    gguf_path: model.gguf
    n_batch: 256
    n_ubatch: 512
)"));
    CHECK_FALSE(validate_config_text(R"(
model_registry:
  - name: text-only
    gguf_path: model.gguf
    context_size: 4096
model_aliases:
  - name: assistant
    target: text-only
    required_context_size: 8192
)"));
    CHECK_FALSE(validate_config_text(R"(
model_registry:
  - name: text-only
    gguf_path: model.gguf
model_aliases:
  - name: vision-assistant
    target: text-only
    required_capabilities: [vision]
)"));
}

TEST_CASE("Active configuration is preferred and invalid profiles fall back safely",
          "[config][active-profile]") {
    const auto directory = std::filesystem::temp_directory_path() /
        "inferdeck-active-config-test";
    std::filesystem::create_directories(directory);
    const auto base = directory / "gateway.yml";
    const auto active = active_config_path_for(base);
    {
        std::ofstream output(base);
        output << "server:\n  host: 0.0.0.0\n  port: 11434\n";
    }
    {
        std::ofstream output(active);
        output << "server:\n  host: 0.0.0.0\n  port: 11435\n";
    }

    const auto selected = load_config_with_active(base);
    CHECK(selected.using_active);
    CHECK(selected.loaded_path == active);
    CHECK(selected.config.port == 11435);

    {
        std::ofstream output(active, std::ios::trunc);
        output << "server:\n  port: 70000\n";
    }
    const auto fallback = load_config_with_active(base);
    CHECK_FALSE(fallback.using_active);
    CHECK(fallback.loaded_path == base);
    CHECK(fallback.config.port == 11434);
    CHECK_FALSE(fallback.fallback_reason.empty());

    std::error_code error;
    std::filesystem::remove_all(directory, error);
}
