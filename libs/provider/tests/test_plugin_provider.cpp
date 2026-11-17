#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

#include "model/imodel.hpp"
#include "provider/codec.hpp"
#include "provider/plugin_provider.hpp"

using namespace inferdeck;

namespace {

model::ModelInfo echo_model(std::string runtime, std::map<std::string, std::string> artifacts = {}) {
    model::ModelInfo info;
    info.name = "echo-model";
    info.runtime = std::move(runtime);
    info.n_slots = 2;
    info.context_size = 100;
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

TEST_CASE("A v1 provider library still serves chat and images", "[provider][plugin][v1]") {
    const auto descriptor = provider::load_plugin_provider(INFERDECK_ECHO_V1_PROVIDER);
    REQUIRE(descriptor);
    CHECK(descriptor->contract.name == "echo_v1");
    CHECK(descriptor->version == "1.0.0");
    CHECK(descriptor->contract.artifact_policy == model::RuntimeArtifactPolicy::None);
    CHECK_FALSE(descriptor->initialize);

    const auto image_path = std::filesystem::temp_directory_path() / "inferdeck-echo-provider.bin";
    std::ofstream(image_path, std::ios::binary) << "PNGDATA";
    auto backend = descriptor->factory(echo_model("echo_v1", {{"image", image_path.string()}}));
    REQUIRE(backend);
    CHECK(backend->vram_usage_mb() == 0);
    REQUIRE_FALSE(backend->acquire_slot());
    REQUIRE(backend->load());
    CHECK(backend->vram_usage_mb() == 123);
    const auto slot = backend->acquire_slot();
    REQUIRE(slot);
    CHECK(backend->n_free_slots() == 1);

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
        *slot, user_request("one two three"), [](const model::InferenceDelta&) { return false; });
    REQUIRE_FALSE(stopped);
    CHECK(stopped.error().code == foundation::ErrorCode::Cancelled);

    auto logprobs = user_request("hi");
    logprobs.logprobs = true;
    const auto rejected = text->predict(*slot, logprobs);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == foundation::ErrorCode::InvalidArgument);
    CHECK(rejected.error().message.find("logprobs") != std::string::npos);

    auto* images = dynamic_cast<model::IImageBackend*>(backend.get());
    REQUIRE(images);
    int percent = 0;
    const auto image = images->generate_images(*slot, {}, [&](int value) { percent = value; return true; });
    REQUIRE(image);
    CHECK(percent == 100);
    REQUIRE(image->png_images.size() == 1);
    CHECK(image->png_images[0].size() == 7);

    auto* speech = dynamic_cast<model::ISpeechBackend*>(backend.get());
    REQUIRE(speech);
    const auto unsupported = speech->synthesize(*slot, {});
    REQUIRE_FALSE(unsupported);
    CHECK(unsupported.error().code == foundation::ErrorCode::Unavailable);

    REQUIRE(backend->unload());
    CHECK_FALSE(backend->is_loaded());
    std::filesystem::remove(image_path);
}

TEST_CASE("A v2 SDK provider is indistinguishable from a built-in backend", "[provider][plugin][v2]") {
    const auto descriptor = provider::load_plugin_provider(INFERDECK_ECHO_PROVIDER);
    REQUIRE(descriptor);
    CHECK(descriptor->contract.name == "echo");
    CHECK(descriptor->version == "2.0.0");
    CHECK(descriptor->contract.vision);
    REQUIRE(descriptor->initialize);
    provider::set_plugin_host_settings(R"({"n_batch":256})");
    REQUIRE(descriptor->initialize({echo_model("echo")}));

    auto backend = descriptor->factory(echo_model("echo"));
    REQUIRE(backend);
    REQUIRE_FALSE(backend->acquire_slot());
    CHECK_FALSE(backend->is_loaded());
    REQUIRE(backend->load());
    CHECK(backend->is_loaded());
    CHECK(backend->vram_usage_mb() == 123);
    CHECK(backend->estimate_vram_mb(3) == 153);
    CHECK(backend->additional_vram_reserve_mb() == 7);
    CHECK(backend->live_vram_accounting_complete());
    CHECK(backend->context_pool_capacity() == 200);
    CHECK(backend->min_slots() == 1);
    CHECK(backend->can_resize_slots());
    CHECK_FALSE(backend->can_reclaim_idle_context());
    REQUIRE(backend->resize_slots(3));
    CHECK(backend->n_slots() == 3);

    auto* text = dynamic_cast<model::IModel*>(backend.get());
    REQUIRE(text);
    CHECK(text->chat_template_meta().supports_thinking);
    CHECK(text->chat_template_meta().thinking_start_tag == "<think>");
    const auto demand = text->estimate_request_demand(user_request("a b c"));
    REQUIRE(demand);
    CHECK(demand->prompt_positions == 3);
    CHECK(demand->required_context == 6);
    REQUIRE(text->ensure_request_capacity(*demand, {}));

    const auto slot = backend->acquire_slot();
    REQUIRE(slot);
    CHECK(backend->slot_busy(*slot));
    CHECK(backend->n_free_slots() == 2);

    SECTION("streaming carries reasoning, logprobs and progress") {
        auto request = user_request("one two");
        request.enable_reasoning = true;
        request.logprobs = true;
        request.progress = std::make_shared<inference::RequestProgress>();
        std::string content;
        std::string reasoning;
        std::size_t logprobs = 0;
        const auto result = text->predict_stream(*slot, request, [&](const model::InferenceDelta& delta)
        {
            content += delta.content;
            reasoning += delta.reasoning_text;
            logprobs += delta.logprobs.size();
            return true;
        });
        REQUIRE(result);
        CHECK(content == "one two");
        CHECK(reasoning == "thinking");
        CHECK(result->reasoning_text == "thinking");
        CHECK(logprobs == 2);
        REQUIRE(result->logprobs.size() == 2);
        CHECK(result->logprobs[0].top_logprobs.size() == 1);
        CHECK(request.progress->output_tokens.load() == 2);
        CHECK(request.progress->prompt_tokens.load() == 2);
        CHECK(request.progress->phase.load() == 3);
    }

    SECTION("tool calls and vision blobs cross the boundary") {
        auto request = user_request("call hello");
        request.tools.push_back({"lookup", "find", R"({"type":"object"})"});
        std::vector<inference::ToolCallDelta> deltas;
        const auto called = text->predict_stream(*slot, request, [&](const model::InferenceDelta& delta)
        {
            deltas.insert(deltas.end(), delta.tool_calls.begin(), delta.tool_calls.end());
            return true;
        });
        REQUIRE(called);
        CHECK(called->finish_reason == "tool_calls");
        REQUIRE(called->tool_calls.size() == 1);
        CHECK(called->tool_calls[0].function_name == "lookup");
        CHECK(called->tool_calls[0].function_arguments == R"({"text":"hello"})");
        REQUIRE(deltas.size() == 1);
        CHECK(deltas[0].function_name == "lookup");

        model::InferenceRequest vision;
        inference::Message message;
        message.content.emplace_back(inference::TextContent{"look"});
        message.content.emplace_back(inference::ImageContent{std::vector<std::byte>(5), "image/png", "auto"});
        vision.messages.push_back(std::move(message));
        const auto vision_demand = text->estimate_request_demand(vision);
        REQUIRE(vision_demand);
        CHECK(vision_demand->prompt_positions == 4);
        const auto seen = text->predict(*slot, vision);
        REQUIRE(seen);
        CHECK(seen->text == "look [image 5 bytes]");
    }

    SECTION("options the manifest rules out are rejected, not dropped") {
        auto request = user_request("hi");
        request.sampling.mirostat = 2;
        const auto rejected = text->predict(*slot, request);
        REQUIRE_FALSE(rejected);
        CHECK(rejected.error().code == foundation::ErrorCode::InvalidArgument);
        CHECK_FALSE(text->estimate_request_demand(request));
    }

    SECTION("request_cancel reaches the provider") {
        backend->request_cancel();
        const auto cancelled = text->predict(*slot, user_request("one two"));
        REQUIRE_FALSE(cancelled);
        CHECK(cancelled.error().code == foundation::ErrorCode::Cancelled);
    }

    SECTION("embeddings and decisions") {
        model::EmbeddingRequest embedding;
        embedding.inputs.push_back(model::EmbeddingTextInput{"abcd"});
        embedding.inputs.push_back(model::EmbeddingTokenInput{{1, 2}});
        const auto vectors = dynamic_cast<model::IEmbeddingBackend*>(backend.get())->embed(*slot, embedding);
        REQUIRE(vectors);
        REQUIRE(vectors->embeddings.size() == 2);
        CHECK(vectors->embeddings[0][0] == 4.0f);
        CHECK(vectors->embeddings[1][0] == 2.0f);

        model::DecisionRequest decision;
        decision.questions.push_back({model::DecisionKind::Choice, "pick", {{"a", ""}, {"b", ""}, {"c", ""}}});
        const auto answers = dynamic_cast<model::IDecisionBackend*>(backend.get())->decide(*slot, decision);
        REQUIRE(answers);
        REQUIRE(answers->answers.size() == 1);
        CHECK(answers->answers[0].probabilities.size() == 3);

        const auto images = dynamic_cast<model::IImageBackend*>(backend.get())->generate_images(*slot, {});
        REQUIRE_FALSE(images);
        CHECK(images.error().code == foundation::ErrorCode::Unavailable);
    }

    REQUIRE(backend->release_slot(*slot));
    REQUIRE(backend->unload());
    CHECK_FALSE(backend->is_loaded());
}

TEST_CASE("The codec round-trips model settings and requests", "[provider][codec]") {
    model::ModelInfo info = echo_model("echo", {{"key", "value"}});
    info.n_batch = 64;
    info.mtp_enabled = true;
    info.speculative_type = "dflash2";
    info.draft_model_path = "E:/models/draft.gguf";
    info.mtp_draft_tokens = 7;
    info.sampling.dry_multiplier = 0.5f;
    info.compute = model::ModelCompute::Cpu;
    const auto decoded = provider::codec::decode_model_info(provider::codec::encode(info));
    REQUIRE(decoded);
    CHECK(decoded->n_batch == 64);
    CHECK(decoded->mtp_enabled);
    CHECK(decoded->speculative_type == "dflash2");
    CHECK(decoded->draft_model_path == "E:/models/draft.gguf");
    CHECK(decoded->mtp_draft_tokens == 7);
    CHECK(decoded->sampling.dry_multiplier == 0.5f);
    CHECK(decoded->compute == model::ModelCompute::Cpu);
    CHECK(decoded->artifacts.at("key") == "value");
    CHECK_FALSE(decoded->n_ubatch);

    auto request = user_request("hi");
    request.sampling.logit_bias = {{5, -1.5f}};
    request.sampling.tfs_z = 0.9f;
    request.prompt_keep_tokens = 4;
    request.reasoning_format = inference::ReasoningFormat::DeepSeek;
    std::vector<inferdeck_blob> blobs;
    const auto round = provider::codec::decode_inference_request(
        provider::codec::encode(request, 0, false, blobs), blobs);
    REQUIRE(round);
    CHECK(round->sampling.logit_bias == request.sampling.logit_bias);
    CHECK(round->sampling.tfs_z == 0.9f);
    CHECK(round->prompt_keep_tokens == 4);
    CHECK(round->reasoning_format == inference::ReasoningFormat::DeepSeek);
    CHECK_FALSE(round->sampling.temperature);

    const auto broken = provider::codec::decode_inference_request(nlohmann::json{{"messages", 5}}, {});
    REQUIRE_FALSE(broken);
    CHECK(broken.error().code == foundation::ErrorCode::ParseError);
}

TEST_CASE("Provider errors and bad libraries are reported, not thrown", "[provider][plugin]") {
    const auto descriptor = provider::load_plugin_provider(INFERDECK_ECHO_PROVIDER);
    REQUIRE(descriptor);
    auto failing = descriptor->factory(echo_model("echo", {{"fail_load", "true"}}));
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
    std::filesystem::copy_file(INFERDECK_ECHO_V1_PROVIDER, directory / "not-a-provider.dll");
    std::ofstream(directory / ("broken.provider" + echo.extension().string())) << "not a library";
    const auto report = provider::load_plugin_providers(directory);
    REQUIRE(report.providers.size() == 1);
    CHECK(report.providers[0].contract.name == "echo");
    REQUIRE(report.rejected.size() == 1);
    CHECK(report.rejected[0].find("broken.provider") != std::string::npos);
    CHECK(model::runtime_contracts().find("echo") != nullptr);
    CHECK(provider::load_plugin_providers(directory / "missing").providers.empty());
}
