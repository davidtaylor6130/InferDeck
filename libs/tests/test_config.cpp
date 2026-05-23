/// @file test_config.cpp
/// @brief Unit tests for the Config module.

#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>
#include "core/Config.hpp"

TEST_CASE("Config loads from YAML file", "[config][load]") {
    // Create temp config file
    std::filesystem::path temp = std::filesystem::temp_directory_path() / "test_config.yml";
    std::ofstream file(temp);
    file << "server:\n";
    file << "  host: \"127.0.0.1\"\n";
    file << "  port: 9090\n";
    file << "  tls:\n";
    file << "    enabled: false\n";
    file << "model:\n";
    file << "  path: \"models/test.gguf\"\n";
    file << "  precision: \"auto\"\n";
    file << "gpu:\n";
    file << "  device_id: 1\n";
    file << "queue:\n";
    file << "  worker_threads: 2\n";
    file.close();

    auto config = inferdeck::core::Config::Load(temp);
    REQUIRE(config.server.port == 9090);
    REQUIRE(config.server.host == "127.0.0.1");
    REQUIRE(config.server.tls_enabled == false);
    REQUIRE(config.model.precision == "auto");
    REQUIRE(config.gpu.device_id == 1);
    REQUIRE(config.queue.worker_threads == 2);

    std::filesystem::remove(temp);
}

TEST_CASE("Config loads defaults when file not found", "[config][defaults]") {
    auto config = inferdeck::core::Config::Load(std::filesystem::path("/nonexistent/path.yml"));
    REQUIRE(config.server.port == 8080);
    REQUIRE(config.server.host == "0.0.0.0");
    REQUIRE(config.server.tls_enabled == true);
}

TEST_CASE("Config saves and reloads correctly", "[config][save]") {
    auto config = inferdeck::core::Config::FullConfig{};
    config.server.port = 7777;
    config.model.path = "models/model.gguf";
    config.model.precision = "f16";

    std::filesystem::path temp = std::filesystem::temp_directory_path() / "test_save.yml";
    inferdeck::core::Config::Save(temp, config);

    auto reloaded = inferdeck::core::Config::Load(temp);
    REQUIRE(reloaded.server.port == 7777);
    REQUIRE(reloaded.model.precision == "f16");

    std::filesystem::remove(temp);
}

TEST_CASE("Config loads Whisper runtime settings", "[config][whisper]") {
    std::string yaml =
        "whisper:\n"
        "  enabled: true\n"
        "  executable: \"C:/InferDeck/runtime/whisper.cpp/whisper-cli.exe\"\n"
        "  model_directory: \"C:/Users/david/Documents/00_Models/Whisper\"\n"
        "  model: \"ggml-large-v3-turbo.bin\"\n"
        "  backend: \"vulkan\"\n"
        "  language: \"auto\"\n"
        "  task: \"transcribe\"\n"
        "  extra_args: \"--flash-attn\"\n";

    auto config = inferdeck::core::Config::LoadFromString(yaml);
    REQUIRE(config.whisper.enabled == true);
    REQUIRE(config.whisper.executable.find("whisper-cli.exe") != std::string::npos);
    REQUIRE(config.whisper.model_directory.find("Whisper") != std::string::npos);
    REQUIRE(config.whisper.model == "ggml-large-v3-turbo.bin");
    REQUIRE(config.whisper.backend == "vulkan");
    REQUIRE(config.whisper.language == "auto");
    REQUIRE(config.whisper.task == "transcribe");
    REQUIRE(config.whisper.extra_args == "--flash-attn");
}
