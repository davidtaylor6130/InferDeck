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

TEST_CASE("Configuration schema rejects unknown keys with precise paths",
          "[config][schema][unknown-key]") {
    const auto top_level = validate_config_text(
        "schema_version: 1\nservre: {}\n");
    REQUIRE_FALSE(top_level);
    CHECK(top_level.error().message ==
          "unknown configuration key: servre");

    const auto nested = validate_config_text(
        "schema_version: 1\n"
        "gateway:\n"
        "  sampling:\n"
        "    tempertaure: 0.7\n");
    REQUIRE_FALSE(nested);
    CHECK(nested.error().message ==
          "unknown configuration key: gateway.sampling.tempertaure");

    const auto model = validate_config_text(
        "schema_version: 1\n"
        "model_registry:\n"
        "  - name: test\n"
        "    runtme: llama_cpp\n");
    REQUIRE_FALSE(model);
    CHECK(model.error().message ==
          "unknown configuration key: model_registry[0].runtme");

    const auto removed_profile = validate_config_text(
        "schema_version: 1\nanthropic:\n  model_aliases: {}\n");
    REQUIRE_FALSE(removed_profile);
    CHECK(removed_profile.error().message ==
          "unknown configuration key: anthropic");
}

TEST_CASE("Configuration schema versions extension ownership",
          "[config][schema][version]") {
    CHECK(validate_config_text(
        "schema_version: 1\nextensions:\n  vendor: true\n"));
    const auto missing = validate_config_text(
        "extensions:\n  vendor: true\n");
    REQUIRE_FALSE(missing);
    CHECK(missing.error().message ==
          "schema_version is required when extensions are configured");
    const auto future = validate_config_text("schema_version: 2\n");
    REQUIRE_FALSE(future);
    CHECK(future.error().message == "unsupported schema_version: 2");
}

TEST_CASE("Runtime contracts reject undeclared capabilities",
          "[config][runtime-contract]") {
    const auto result = validate_config_text(R"(
model_registry:
  - name: invalid-capability
    runtime: llama_cpp
    modality: text
    capabilities: [image_generation]
    gguf_path: model.gguf
)");
    REQUIRE_FALSE(result);
    CHECK(result.error().message.find(
              "does not support capability image_generation") !=
          std::string::npos);
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

TEST_CASE("Gateway configuration decodes explicit model resource metadata",
          "[config][resources]") {
    const auto path = std::filesystem::temp_directory_path() /
        "inferdeck-explicit-resource-config.yml";
    {
        std::ofstream output(path);
        output << R"(model_registry:
  - name: helper
    runtime: windows_sapi
    modality: audio_speech
    capabilities: [audio_speech]
    n_slots: 1
    role: media
    compute: cpu
    residency: always
    admission_pool: audio
    concurrency_limit: 1
    memory_required_mb: 32
    eviction_eligible: false
)";
    }
    const auto config = load_config(path);
    REQUIRE(config.models.size() == 1);
    const auto& info = config.models.front();
    CHECK(info.resource_metadata_explicit);
    CHECK(info.role == inferdeck::model::ModelRole::Media);
    CHECK(info.compute == inferdeck::model::ModelCompute::Cpu);
    CHECK(info.residency == inferdeck::model::ResidencyPolicy::Always);
    CHECK(info.admission_pool == "audio");
    CHECK(info.concurrency_limit == 1);
    CHECK(info.memory_required_mb == 32);
    CHECK_FALSE(info.eviction_eligible);
    std::error_code error;
    std::filesystem::remove(path, error);
}

TEST_CASE("Gateway configuration rejects partial and impossible resources",
          "[config][resources]") {
    CHECK_FALSE(validate_config_text(R"(
model_registry:
  - name: partial
    gguf_path: model.gguf
    role: conversation
)"));
    CHECK_FALSE(validate_config_text(R"(
model_registry:
  - name: gpu-helper
    gguf_path: model.gguf
    n_slots: 1
    role: helper
    compute: vulkan_gpu
    residency: always
    admission_pool: helper
    concurrency_limit: 1
    memory_required_mb: 64
    eviction_eligible: false
)"));
    CHECK_FALSE(validate_config_text(R"(
model_registry:
  - name: evict-always
    runtime: windows_sapi
    modality: audio_speech
    n_slots: 1
    role: media
    compute: cpu
    residency: always
    admission_pool: audio
    concurrency_limit: 1
    memory_required_mb: 32
    eviction_eligible: true
)"));
}

TEST_CASE("Compatibility profiles are explicit and default off", "[config]") {
    const auto valid = validate_config_text(R"(
compatibility:
  openai_derivative:
    enabled: true
)");
    REQUIRE(valid);
    CHECK_FALSE(validate_config_text(R"(
compatibility:
  unknown:
    enabled: true
)"));
    CHECK_FALSE(validate_config_text(R"(
compatibility:
  openai_derivative:
    mode: permissive
)"));
}

TEST_CASE("Repository gateway configuration keeps remote control disabled", "[config][lan]") {
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
    CHECK_FALSE(config.control_allow_remote);
    CHECK_FALSE(config.control_allow_data_plane_token);
    CHECK_FALSE(config.openai_derivative_compatibility_enabled);
    CHECK(config.control_token.empty());
    REQUIRE(config.control_origins.size() == 3);
    CHECK(std::find(config.control_origins.begin(), config.control_origins.end(), "*") ==
          config.control_origins.end());
}

TEST_CASE("Security fixture configurations load with distinct control policies",
          "[config][lan][security]") {
    const auto config_dir = std::filesystem::path(INFERDECK_SOURCE_DIR) / "config";

    const auto local_only = load_config(config_dir / "gateway.test-security.yml");
    CHECK(local_only.host == "0.0.0.0");
    CHECK(local_only.auth_required);
    CHECK(local_only.auth_token == "test-data-token");
    CHECK_FALSE(local_only.control_allow_remote);
    CHECK(local_only.control_token.empty());

    const auto remote = load_config(config_dir / "gateway.test-security-remote.yml");
    CHECK(remote.host == "0.0.0.0");
    CHECK(remote.auth_required);
    CHECK(remote.auth_token == "test-data-token");
    CHECK(remote.control_allow_remote);
    CHECK_FALSE(remote.control_allow_data_plane_token);
    CHECK(remote.control_token == "test-control-token-0123456789abcdef");
    REQUIRE(remote.control_origins.size() == 1);
    CHECK(remote.control_origins.front() == "http://admin.example");
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

TEST_CASE("Repository gateway configuration reserves measured Qwen resources",
          "[config][resources][vram]") {
    const auto path = std::filesystem::path(INFERDECK_SOURCE_DIR) /
        "config" / "gateway.yml";
    const auto config = load_config(path);
    const auto qwen35 = std::find_if(
        config.models.begin(), config.models.end(),
        [](const auto& model) { return model.name == "qwen3.6-35b-a3b"; });
    const auto helper = std::find_if(
        config.models.begin(), config.models.end(),
        [](const auto& model) {
            return model.name == "qwen2.5-0.5b-instruct";
        });
    REQUIRE(qwen35 != config.models.end());
    REQUIRE(helper != config.models.end());
    CHECK(qwen35->vram_required_mb == 26000);
    CHECK(helper->n_slots == 1);
    CHECK(helper->context_size == 4096);
    CHECK(helper->vram_required_mb == 900);
    REQUIRE(helper->n_gpu_layers);
    CHECK(*helper->n_gpu_layers == 0);
    CHECK(helper->resource_metadata_explicit);
    CHECK(helper->role == inferdeck::model::ModelRole::Helper);
    CHECK(helper->compute == inferdeck::model::ModelCompute::Cpu);
    CHECK(helper->residency == inferdeck::model::ResidencyPolicy::Always);
    CHECK(helper->admission_pool == "helper");
    CHECK(helper->concurrency_limit == 1);
    CHECK(helper->memory_required_mb == 2048);
    CHECK_FALSE(helper->eviction_eligible);
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
    CHECK_FALSE(validate_config_text("control:\n  allow_remote: true\n"));
    CHECK_FALSE(validate_config_text(R"(
control:
  allow_remote: true
  token: "test-control-token-0123456789abc;ef"
  origins: ["http://192.168.0.168:11434"]
)"));
    CHECK_FALSE(validate_config_text(
        "control:\n  allow_remote: true\n  token: control-secret\n"));
    CHECK_FALSE(validate_config_text(R"(
control:
  allow_remote: true
  token: control-secret-0123456789abcdefghi
  origins: ["*"]
)"));
    CHECK_FALSE(validate_config_text(R"(
control:
  allow_remote: false
  origins: ["*"]
)"));
    CHECK_FALSE(validate_config_text(R"(
control:
  allow_remote: false
  origins: ["null"]
)"));
    CHECK_FALSE(validate_config_text(R"(
control:
  allow_remote: false
  origins: ["https://admin.example/path"]
)"));
    CHECK(validate_config_text(R"(
control:
  allow_remote: true
  token: control-secret-0123456789abcdefghi
  origins: ["https://admin.example"]
)"));
    CHECK_FALSE(validate_config_text(R"(
auth:
  required: true
  token: shared-secret-0123456789abcdefghij
control:
  allow_remote: true
  token: shared-secret-0123456789abcdefghij
  origins: ["https://admin.example"]
)"));
    CHECK(validate_config_text(R"(
auth:
  required: true
  token: shared-secret-0123456789abcdefghij
control:
  allow_remote: true
  allow_data_plane_token: true
  token: shared-secret-0123456789abcdefghij
  origins: ["https://admin.example"]
)"));
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
