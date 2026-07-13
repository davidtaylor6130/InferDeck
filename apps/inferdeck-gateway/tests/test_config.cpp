#include <catch2/catch_test_macros.hpp>

#include "config.hpp"

using inferdeck::gateway::validate_config_text;

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
}
