#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

#include "model/imodel.hpp"
#include "provider/plugin_provider.hpp"

using namespace inferdeck;

namespace {

model::ModelInfo echo_model(std::map<std::string, std::string> artifacts = {}) {
    model::ModelInfo info;
    info.name = "echo-model";
    info.runtime = "echo";
    info.n_slots = 1;
    info.vram_required_mb = 123;
    info.artifacts = std::move(artifacts);
    return info;
}

model::InferenceRequest user_request(std::string text) {
    model::InferenceRequest request;
    request.messages.emplace_back(inference::MessageRole::User, std::move(text));
    return request;
}

}

TEST_CASE("A provider library serves chat and images through the C ABI", "[provider][plugin]") {
    const auto descriptor = provider::load_plugin_provider(INFERDECK_ECHO_PROVIDER);
    REQUIRE(descriptor);
    CHECK(descriptor->contract.name == "echo");
    CHECK(descriptor->version == "1.0.0");
    CHECK(descriptor->contract.artifact_policy == model::RuntimeArtifactPolicy::None);
    CHECK(descriptor->contract.modality_capabilities.at("text").contains("chat_completions"));

    const auto image_path = std::filesystem::temp_directory_path() / "inferdeck-echo-provider.bin";
    std::ofstream(image_path, std::ios::binary) << "PNGDATA";
    auto backend = descriptor->factory(echo_model({{"image", image_path.string()}}));
    REQUIRE(backend);
    CHECK(backend->vram_usage_mb() == 0);
    REQUIRE_FALSE(backend->acquire_slot());
    REQUIRE(backend->load());
    CHECK(backend->vram_usage_mb() == 123);
    const auto slot = backend->acquire_slot();
    REQUIRE(slot);
    CHECK(backend->n_free_slots() == 0);

    auto* text = dynamic_cast<model::IModel*>(backend.get());
    REQUIRE(text);
    const auto reply = text->predict(*slot, user_request("hello provider world"));
    REQUIRE(reply);
    CHECK(reply->text == "hello provider world");
    CHECK(reply->completion_tokens == 3);

    std::string streamed;
    const auto stream = text->predict_stream(
        *slot, user_request("one two three"),
        [&](const model::InferenceDelta& delta) { streamed += delta.content; return true; });
    REQUIRE(stream);
    CHECK(streamed == "one two three");

    const auto stopped = text->predict_stream(
        *slot, user_request("one two three"),
        [](const model::InferenceDelta&) { return false; });
    REQUIRE_FALSE(stopped);
    CHECK(stopped.error().code == foundation::ErrorCode::Cancelled);

    auto* images = dynamic_cast<model::IImageBackend*>(backend.get());
    REQUIRE(images);
    int percent = 0;
    const auto image = images->generate_images(
        *slot, {}, [&](int value) { percent = value; return true; });
    REQUIRE(image);
    CHECK(percent == 100);
    REQUIRE(image->png_images.size() == 1);
    CHECK(image->png_images[0].size() == 7);

    auto* speech = dynamic_cast<model::ISpeechBackend*>(backend.get());
    REQUIRE(speech);
    const auto unsupported = speech->synthesize(*slot, {});
    REQUIRE_FALSE(unsupported);
    CHECK(unsupported.error().code == foundation::ErrorCode::Unavailable);
    CHECK(unsupported.error().message == "echo provider does not implement speech");

    REQUIRE(backend->unload());
    CHECK_FALSE(backend->is_loaded());
    std::filesystem::remove(image_path);
}

TEST_CASE("Provider errors and bad libraries are reported, not thrown", "[provider][plugin]") {
    const auto descriptor = provider::load_plugin_provider(INFERDECK_ECHO_PROVIDER);
    REQUIRE(descriptor);
    auto failing = descriptor->factory(echo_model({{"fail_load", "true"}}));
    REQUIRE(failing);
    const auto loaded = failing->load();
    REQUIRE_FALSE(loaded);
    CHECK(loaded.error().code == foundation::ErrorCode::Unavailable);
    CHECK(loaded.error().message == "echo provider was asked to fail its load");
    CHECK_FALSE(failing->is_loaded());

    const auto directory = std::filesystem::temp_directory_path() / "inferdeck-provider-scan";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    const std::filesystem::path echo = INFERDECK_ECHO_PROVIDER;
    std::filesystem::copy_file(echo, directory / echo.filename());
    std::ofstream(directory / ("broken" + echo.extension().string())) << "not a library";
    const auto report = provider::load_plugin_providers(directory);
    REQUIRE(report.providers.size() == 1);
    CHECK(report.providers[0].contract.name == "echo");
    REQUIRE(report.rejected.size() == 1);
    CHECK(model::runtime_contracts().find("echo") != nullptr);
    CHECK(provider::load_plugin_providers(directory / "missing").providers.empty());
}
