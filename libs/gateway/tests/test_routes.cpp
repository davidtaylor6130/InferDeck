#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "foundation/result.hpp"
#include "gateway/auth.hpp"
#include "gateway/cors.hpp"
#include "gateway/openai_routes.hpp"
#include "gateway/media_routes.hpp"
#include "gateway/openai_adapter.hpp"
#include "gateway/openai_error.hpp"
#include "gateway/route_manifest.hpp"
#include "gateway/responses_adapter.hpp"
#include "gateway/routes.hpp"
#include "httplib.h"
#include "model/backend_coordinator.hpp"
#include "model/imodel.hpp"
#include "model/model_registry.hpp"
#include "observability/metrics.hpp"
#include "observability/stats_db.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <nlohmann/json.hpp>
#include <thread>
#include <tuple>
#include <type_traits>

using namespace inferdeck;
using namespace inferdeck::model;
using namespace inferdeck::gateway;

using inferdeck::foundation::ErrorCode;
using inferdeck::foundation::Ok;
using inferdeck::foundation::Result;

namespace {

const nlohmann::json& openai_schema_snapshots() {
    static const nlohmann::json snapshots = [] {
        const auto path = std::filesystem::path(INFERDECK_SOURCE_DIR) /
            "tests/fixtures/openai_schema_snapshots.json";
        std::ifstream input(path, std::ios::binary);
        if (!input.good()) throw std::runtime_error("cannot read OpenAI schema snapshots");
        return nlohmann::json::parse(input);
    }();
    return snapshots;
}

nlohmann::json object_keys(const nlohmann::json& value) {
    nlohmann::json keys = nlohmann::json::array();
    for (const auto& item : value.items()) keys.push_back(item.key());
    return keys;
}

void check_schema(const nlohmann::json& value, const char* snapshot) {
    REQUIRE(value.is_object());
    CHECK(object_keys(value) == openai_schema_snapshots().at(snapshot));
}

}

TEST_CASE("OpenAI error mapping is centralized and typed",
          "[routes][errors]") {
    const auto context = map_openai_error(
        ErrorCode::ContextLengthExceeded, "messages");
    CHECK(context.status == 400);
    CHECK(context.type == "invalid_request_error");
    CHECK(context.code == "context_length_exceeded");
    CHECK(context.parameter == "messages");
    CHECK(map_openai_error(ErrorCode::NotFound).status == 404);
    CHECK(map_openai_error(ErrorCode::Unavailable).status == 503);
    CHECK(map_openai_error(ErrorCode::Timeout).status == 504);
}

TEST_CASE("OpenAI adapter produces canonical multimodal and tool inputs",
          "[routes][adapter]") {
    const nlohmann::json body = {
        {"model", "adapter-model"},
        {"messages", nlohmann::json::array({
            {{"role", "developer"}, {"content", "policy"}},
            {{"role", "user"},
             {"content", nlohmann::json::array({
                 {{"type", "text"}, {"text", "describe"}},
                 {{"type", "image_url"},
                  {"image_url", {{"url", "data:image/png;base64,AQID"},
                                 {"detail", "high"}}}}
             })}}
        })},
        {"tools", nlohmann::json::array({
            {{"type", "function"},
             {"function", {{"name", "lookup"}, {"description", "find"}}}}
        })},
        {"frequency_penalty", 1.2},
        {"presence_penalty", -0.4}
    };

    const auto parsed = parse_openai_chat_request(body, false);
    REQUIRE(parsed);
    REQUIRE(parsed->messages.size() == 2);
    CHECK(parsed->messages[0].role == inference::MessageRole::Developer);
    REQUIRE(parsed->messages[1].content.size() == 2);
    const auto* text =
        std::get_if<inference::TextContent>(&parsed->messages[1].content[0]);
    REQUIRE(text != nullptr);
    CHECK(text->text == "describe");
    const auto* image =
        std::get_if<inference::ImageContent>(&parsed->messages[1].content[1]);
    REQUIRE(image != nullptr);
    CHECK(image->media_type == "image/png");
    CHECK(image->detail == "high");
    REQUIRE(image->bytes.size() == 3);
    CHECK(std::to_integer<unsigned char>(image->bytes[0]) == 1);
    CHECK(std::to_integer<unsigned char>(image->bytes[1]) == 2);
    CHECK(std::to_integer<unsigned char>(image->bytes[2]) == 3);
    REQUIRE(parsed->tools.size() == 1);
    CHECK(parsed->tools[0].parameters_schema == "{}");
    REQUIRE(parsed->sampling.frequency_penalty.has_value());
    CHECK(*parsed->sampling.frequency_penalty == 1.2f);
    REQUIRE(parsed->sampling.presence_penalty.has_value());
    CHECK(*parsed->sampling.presence_penalty == -0.4f);
}

TEST_CASE("OpenAI adapter rejects malformed image base64",
          "[routes][adapter][validation]") {
    for (const std::string encoded : {"A===", "AA==junk", "AB==", "AAB="}) {
        const nlohmann::json body = {
            {"messages", nlohmann::json::array({
                {{"role", "user"},
                 {"content", nlohmann::json::array({
                     {{"type", "image_url"},
                      {"image_url", "data:image/png;base64," + encoded}}
                 })}}
            })}
        };
        INFO(encoded);
        CHECK_FALSE(parse_openai_chat_request(body, false));
    }
}

TEST_CASE("OpenAI adapter validates standard penalty ranges",
          "[routes][adapter][validation]") {
    const auto request_with = [](const char* field,
                                 const nlohmann::json& value) {
        nlohmann::json body{
            {"model", "adapter-model"},
            {"messages", nlohmann::json::array({
                {{"role", "user"}, {"content", "test"}}
            })}
        };
        body[field] = value;
        return body;
    };
    CHECK(parse_openai_chat_request(
        request_with("frequency_penalty", -2.0), false));
    CHECK(parse_openai_chat_request(
        request_with("frequency_penalty", 2.0), false));
    CHECK(parse_openai_chat_request(
        request_with("presence_penalty", -2.0), false));
    CHECK(parse_openai_chat_request(
        request_with("presence_penalty", 2.0), false));
    CHECK_FALSE(parse_openai_chat_request(
        request_with("frequency_penalty", -2.01), false));
    CHECK_FALSE(parse_openai_chat_request(
        request_with("frequency_penalty", 2.01), false));
    CHECK_FALSE(parse_openai_chat_request(
        request_with("presence_penalty", "high"), false));
}

TEST_CASE("OpenAI adapter validates assistant tool call type",
          "[routes][adapter][validation]") {
    const auto request_with = [](const nlohmann::json& tool_call) {
        return nlohmann::json{
            {"model", "adapter-model"},
            {"messages", nlohmann::json::array({
                {{"role", "assistant"},
                 {"content", nullptr},
                 {"tool_calls", nlohmann::json::array({tool_call})}}
            })}
        };
    };
    const nlohmann::json function = {
        {"id", "call_1"},
        {"function", {{"name", "lookup"}, {"arguments", "{}"}}}
    };
    CHECK_FALSE(parse_openai_chat_request(request_with(function), false));
    auto unsupported = function;
    unsupported["type"] = "custom";
    CHECK_FALSE(parse_openai_chat_request(request_with(unsupported), false));
    auto valid = function;
    valid["type"] = "function";
    CHECK(parse_openai_chat_request(request_with(valid), false));
}

TEST_CASE("Responses adapter builds canonical messages directly",
          "[routes][responses][adapter]") {
    const auto parsed = parse_openai_responses_request(nlohmann::json{
        {"model", "test-model"},
        {"instructions", "top-level policy"},
        {"input", nlohmann::json::array({
            {{"type", "message"}, {"role", "developer"},
             {"content", "item policy"}},
            {{"type", "function_call"}, {"call_id", "call_1"},
             {"name", "lookup"}, {"arguments", "{}"}},
            {{"type", "function_call_output"}, {"call_id", "call_1"},
             {"output", nlohmann::json::array({
                 {{"type", "input_text"}, {"text", "result"}},
             })}},
        })},
        {"store", false},
        {"background", false},
        {"conversation", nullptr},
        {"previous_response_id", nullptr},
        {"include", nlohmann::json::array()},
        {"truncation", "disabled"},
        {"service_tier", "default"},
        {"prompt_cache_key", nullptr},
    }, false);
    REQUIRE(parsed);
    CHECK(parsed->requested_model == "test-model");
    REQUIRE(parsed->generation.messages.size() == 4);
    CHECK(parsed->generation.messages[0].role ==
          inference::MessageRole::Developer);
    CHECK(parsed->generation.messages[1].role ==
          inference::MessageRole::Developer);
    CHECK(parsed->generation.messages[2].role ==
          inference::MessageRole::Assistant);
    REQUIRE(parsed->generation.messages[2].tool_calls.size() == 1);
    CHECK(parsed->generation.messages[2].tool_calls[0].id == "call_1");
    CHECK(parsed->generation.messages[3].role ==
          inference::MessageRole::Tool);
    CHECK(parsed->generation.messages[3].tool_call_id == "call_1");
    const auto* output = std::get_if<inference::TextContent>(
        &parsed->generation.messages[3].content[0]);
    REQUIRE(output);
    CHECK(output->text == "result");
}

TEST_CASE("Responses route contains no Chat translation shim",
          "[routes][responses][dependency]") {
    const auto path = std::filesystem::path(INFERDECK_SOURCE_DIR) /
        "libs/gateway/src/openai_routes.cpp";
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.good());
    const std::string source{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}};
    CHECK(source.find("responses_to_chat") == std::string::npos);
    CHECK(source.find("chat_body") == std::string::npos);
    CHECK(source.find("parse_openai_chat_request") == std::string::npos);
}

TEST_CASE("Derivative OpenAI adapter preserves reasoning toggle",
          "[routes][adapter][reasoning]") {
    const nlohmann::json body = {
        {"model", "adapter-model"},
        {"messages", nlohmann::json::array({
            {{"role", "user"}, {"content", "answer"}}
        })},
        {"chat_template_kwargs", {{"enable_thinking", false}}}
    };
    const auto derivative = parse_openai_chat_request(body, true);
    REQUIRE(derivative);
    REQUIRE(derivative->enable_reasoning.has_value());
    CHECK_FALSE(*derivative->enable_reasoning);
    const auto strict = parse_openai_chat_request(body, false);
    REQUIRE(strict);
    CHECK_FALSE(strict->enable_reasoning.has_value());
}

TEST_CASE("OpenAI adapter matches the canonical golden fixture",
          "[routes][adapter][golden]") {
    const auto path = std::filesystem::path(INFERDECK_SOURCE_DIR) /
        "tests/fixtures/openai_adapter_golden.json";
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.good());
    const auto fixture = nlohmann::json::parse(input);
    const auto parsed = parse_openai_chat_request(fixture["chat_request"], false);
    REQUIRE(parsed);
    const auto role_name = [](inference::MessageRole role) {
        if (role == inference::MessageRole::Developer) return "developer";
        if (role == inference::MessageRole::System) return "system";
        if (role == inference::MessageRole::Assistant) return "assistant";
        if (role == inference::MessageRole::Tool) return "tool";
        return "user";
    };
    nlohmann::json actual;
    actual["roles"] = nlohmann::json::array();
    actual["texts"] = nlohmann::json::array();
    for (const auto& message : parsed->messages) {
        actual["roles"].push_back(role_name(message.role));
        const auto* text = std::get_if<inference::TextContent>(&message.content[0]);
        REQUIRE(text);
        actual["texts"].push_back(text->text);
    }
    actual["max_output_tokens"] = parsed->max_output_tokens;
    const auto snapshot_float = [](float value) {
        return std::round(static_cast<double>(value) * 1'000'000.0) /
               1'000'000.0;
    };
    actual["temperature"] = snapshot_float(*parsed->sampling.temperature);
    actual["top_p"] = snapshot_float(*parsed->sampling.top_p);
    actual["tool_name"] = parsed->tools[0].name;
    actual["tool_schema"] = parsed->tools[0].parameters_schema;
    actual["tool_choice"] = parsed->tool_choice.function_name;
    actual["output_kind"] = "json_schema";
    actual["output_name"] = parsed->output.name;
    actual["output_strict"] = parsed->output.strict;
    CHECK(actual == fixture["canonical"]);
}

namespace {

class IModelMock : public IModel, public IEmbeddingBackend, public IImageBackend,
                   public ISpeechBackend, public ITranscriptionBackend {
public:
    ModelInfo model_info{};
    std::atomic<bool> loaded{false};
    std::atomic<int> vram_mb{4096};
    std::atomic<int> max_slots{2};
    std::atomic<int> load_delay_ms{0};
    std::atomic<bool> load_started{false};
    std::atomic<bool> load_should_fail{false};
    std::atomic<bool> block_until_cancel{false};
    std::atomic<bool> block_media_until_cancel{false};
    std::atomic<bool> split_utf8{false};
    std::atomic<bool> context_error{false};
    std::atomic<int> last_max_tokens{0};
    std::atomic<int> stream_delta_count{0};
    std::atomic<int> stream_deltas_emitted{0};
    std::atomic<int> speech_chunk_count{0};
    std::atomic<int> speech_chunks_emitted{0};
    std::atomic<bool> speech_should_fail{false};
    std::atomic<bool> speech_should_cancel{false};
    std::atomic<bool> transcription_should_cancel{false};
    std::atomic<int> transcription_delay_ms{0};
    std::vector<int> busy_slots;
    mutable std::mutex mtx;
    InferenceRequest last_request;
    ChatTemplateMeta chat_meta_{};

    explicit IModelMock(ModelInfo info) : model_info(std::move(info)) {
        busy_slots.assign(max_slots.load(), 0);
    }

    const ModelInfo& info() const override { return model_info; }
    const ChatTemplateMeta& chat_template_meta() const override { return chat_meta_; }

    Result<void> load() override {
        load_started.store(true);
        const int delay = load_delay_ms.load();
        if (delay > 0) std::this_thread::sleep_for(std::chrono::milliseconds{delay});
        if (load_should_fail.load()) {
            return inferdeck::foundation::Err<void>(
                ErrorCode::Internal, "mock load failed");
        }
        loaded.store(true);
        return Ok();
    }
    Result<void> unload() override {
        loaded.store(false);
        std::lock_guard<std::mutex> lock(mtx);
        std::fill(busy_slots.begin(), busy_slots.end(), 0);
        return Ok();
    }
    bool is_loaded() const override { return loaded.load(); }
    int vram_usage_mb() const override { return vram_mb.load(); }
    int n_slots() const override { return max_slots.load(); }

    int n_free_slots() const override {
        std::lock_guard<std::mutex> lock(mtx);
        int busy = 0;
        for (int b : busy_slots) if (b) ++busy;
        return max_slots.load() - busy;
    }

    Result<int> acquire_slot() override {
        std::lock_guard<std::mutex> lock(mtx);
        for (int i = 0; i < static_cast<int>(busy_slots.size()); ++i) {
            if (busy_slots[i] == 0) {
                busy_slots[i] = 1;
                return Ok(i);
            }
        }
        return inferdeck::foundation::Err<int>(ErrorCode::Unavailable, "no free slots");
    }

    Result<void> release_slot(int slot_id) override {
        std::lock_guard<std::mutex> lock(mtx);
        if (slot_id < 0 || slot_id >= static_cast<int>(busy_slots.size())) {
            return inferdeck::foundation::Err<void>(ErrorCode::InvalidArgument, "bad slot");
        }
        busy_slots[slot_id] = 0;
        return Ok();
    }

    bool slot_busy(int slot_id) const override {
        std::lock_guard<std::mutex> lock(mtx);
        if (slot_id < 0 || slot_id >= static_cast<int>(busy_slots.size())) return false;
        return busy_slots[slot_id] != 0;
    }

    Result<InferenceResult> predict(int, const InferenceRequest& request) override {
        last_max_tokens.store(request.max_output_tokens);
        {
            std::lock_guard<std::mutex> lock(mtx);
            last_request = request;
        }
        if (context_error.load()) {
            return inferdeck::foundation::Err<InferenceResult>(
                ErrorCode::ContextLengthExceeded, "prompt is too long");
        }
        InferenceResult r;
        if (!request.tools.empty()) {
            r.reasoning_text = "need a tool";
            ToolCall call;
            call.id = "call_test";
            call.type = "function";
            call.function_name = "list_workspace";
            call.function_arguments = "{\"path\":\".\"}";
            r.tool_calls.push_back(std::move(call));
        } else {
            r.text = "Hello from model";
            if (request.logprobs) {
                inference::TokenLogprob token;
                token.token = "Hello";
                token.logprob = -0.125f;
                token.bytes = {72, 101, 108, 108, 111};
                token.top_logprobs.push_back(
                    {"Hi", -1.5f, {72, 105}});
                r.logprobs.push_back(std::move(token));
            }
        }
        r.prompt_tokens = 3;
        r.completion_tokens = 4;
        return Ok(std::move(r));
    }

    Result<InferenceResult> predict_stream(
        int, const InferenceRequest& request, const TokenCallback& callback,
        const std::atomic<bool>* cancel = nullptr) override {
        {
            std::lock_guard<std::mutex> lock(mtx);
            last_request = request;
        }
        if (context_error.load()) {
            return inferdeck::foundation::Err<InferenceResult>(
                ErrorCode::ContextLengthExceeded, "prompt is too long");
        }
        const int deltas = stream_delta_count.load();
        if (deltas > 0) {
            InferenceDelta delta;
            delta.content.assign(64 * 1024, 'x');
            if (request.logprobs) {
                inference::TokenLogprob token;
                token.token = "x";
                token.logprob = -0.25f;
                token.bytes = {120};
                token.top_logprobs.push_back({"y", -1.0f, {121}});
                delta.logprobs.push_back(std::move(token));
            }
            for (int index = 0; index < deltas; ++index) {
                const bool accepted = callback(delta);
                stream_deltas_emitted.fetch_add(1);
                if (!accepted) return Ok(InferenceResult{});
            }
            return Ok(InferenceResult{});
        }
        if (block_until_cancel.load()) {
            // Simulate a long generation that only ends when cancelled.
            while (!(cancel && cancel->load())) {
                std::this_thread::sleep_for(std::chrono::milliseconds{5});
            }
            return Ok(InferenceResult{});
        }
        if (split_utf8.load()) {
            InferenceDelta first;
            first.content = std::string("cost \xe2\x82", 7);
            first.reasoning_text = std::string("idea \xf0\x9f\x92", 8);
            ToolCallDelta first_call;
            first_call.index = 0;
            first_call.id = "call_utf8";
            first_call.type = "function";
            first_call.function_name = "lookup";
            first_call.function_arguments = std::string("{\"city\":\"M") +
                std::string("\xc3", 1);
            first.tool_calls.push_back(std::move(first_call));
            if (!callback(first)) return Ok(InferenceResult{});

            InferenceDelta second;
            second.content = std::string("\xac", 1) + "5";
            second.reasoning_text = std::string("\xa1", 1);
            ToolCallDelta second_call;
            second_call.index = 0;
            second_call.function_arguments = std::string("\xbc", 1) + "nchen\"}";
            second.tool_calls.push_back(std::move(second_call));
            if (!callback(second)) return Ok(InferenceResult{});

            InferenceDelta trailing;
            trailing.content = std::string("\xe2", 1);
            if (!callback(trailing)) return Ok(InferenceResult{});

            InferenceResult result;
            result.prompt_tokens = 5;
            result.completion_tokens = 7;
            ToolCall call;
            call.id = "call_utf8";
            call.function_name = "lookup";
            call.function_arguments = std::string("{\"city\":\"M\xc3\xbcnchen\"}", 19);
            result.tool_calls.push_back(std::move(call));
            return Ok(std::move(result));
        }
        InferenceDelta reasoning;
        reasoning.reasoning_text = "need a tool";
        if (!callback(reasoning)) return Ok(InferenceResult{});

        InferenceDelta header;
        ToolCallDelta tc_header;
        tc_header.index = 0;
        tc_header.id = "call_test";
        tc_header.type = "function";
        tc_header.function_name = "list_workspace";
        header.tool_calls.push_back(std::move(tc_header));
        if (!callback(header)) return Ok(InferenceResult{});

        InferenceDelta args;
        ToolCallDelta tc_args;
        tc_args.index = 0;
        tc_args.function_arguments = "{\"path\":\".\"}";
        args.tool_calls.push_back(std::move(tc_args));
        if (!callback(args)) return Ok(InferenceResult{});

        InferenceResult r;
        r.prompt_tokens = 8;
        r.completion_tokens = 12;
        ToolCall call;
        call.id = "call_test";
        call.type = "function";
        call.function_name = "list_workspace";
        call.function_arguments = "{\"path\":\".\"}";
        r.tool_calls.push_back(std::move(call));
        return Ok(std::move(r));
    }

    Result<EmbeddingResult> embed(
        int, const EmbeddingRequest& request,
        const std::function<bool()>& cancelled = {}) override {
        if (cancelled && cancelled()) {
            return inferdeck::foundation::Err<EmbeddingResult>(ErrorCode::Cancelled, "cancelled");
        }
        EmbeddingResult result;
        const int dimensions = request.dimensions.value_or(3);
        for (std::size_t i = 0; i < request.inputs.size(); ++i) {
            result.embeddings.emplace_back(static_cast<std::size_t>(dimensions),
                                           static_cast<float>(i + 1));
            result.prompt_tokens += static_cast<int>(std::visit(
                [](const auto& input) -> std::size_t {
                    using T = std::decay_t<decltype(input)>;
                    if constexpr (std::is_same_v<T, EmbeddingTextInput>) {
                        return input.text.size();
                    } else {
                        return input.tokens.size();
                    }
                }, request.inputs[i]));
        }
        result.duration_ms = 2.0f;
        return Ok(std::move(result));
    }

    Result<ImageGenerationResult> generate_images(
        int, const ImageGenerationRequest& request,
        const std::function<bool(int)>& progress = {}) override {
        if (block_media_until_cancel.load()) {
            while (!progress || progress(50)) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            return inferdeck::foundation::Err<ImageGenerationResult>(ErrorCode::Cancelled, "cancelled");
        }
        if (progress && !progress(50)) return inferdeck::foundation::Err<ImageGenerationResult>(ErrorCode::Cancelled, "cancelled");
        ImageGenerationResult result;
        result.duration_ms = 12;
        for (int index = 0; index < request.count; ++index) {
            result.png_images.push_back({std::byte{0x89}, std::byte{0x50}, std::byte{0x4e}, std::byte{0x47}});
        }
        return Ok(std::move(result));
    }

    Result<AudioResult> synthesize(
        int, const SpeechRequest& request,
        const std::function<bool(const std::byte*, std::size_t)>& stream = {}) override {
        if (speech_should_fail.load()) {
            return inferdeck::foundation::Err<AudioResult>(
                ErrorCode::InvalidArgument, "mock speech failure");
        }
        if (speech_should_cancel.load()) {
            return inferdeck::foundation::Err<AudioResult>(
                ErrorCode::Cancelled, "cancelled");
        }
        AudioResult result;
        result.bytes = {std::byte{0x52}, std::byte{0x49}, std::byte{0x46}, std::byte{0x46}};
        result.content_type = request.format == "wav" ? "audio/wav" : "audio/mpeg";
        result.duration_ms = 8;
        result.output_audio_seconds = 1.25;
        const int chunks = speech_chunk_count.load();
        if (stream && chunks > 0) {
            std::vector<std::byte> chunk(128 * 1024, std::byte{0x41});
            for (int index = 0; index < chunks; ++index) {
                const bool accepted = stream(chunk.data(), chunk.size());
                speech_chunks_emitted.fetch_add(1);
                if (!accepted) {
                    return inferdeck::foundation::Err<AudioResult>(
                        ErrorCode::Cancelled, "cancelled");
                }
            }
            result.bytes.clear();
            return Ok(std::move(result));
        }
        if (stream && !stream(result.bytes.data(), result.bytes.size())) {
            return inferdeck::foundation::Err<AudioResult>(ErrorCode::Cancelled, "cancelled");
        }
        return Ok(std::move(result));
    }

    Result<void> validate_speech_request(
        const SpeechRequest& request) override {
        if (request.voice == "not-a-voice" || request.voice == "999") {
            return inferdeck::foundation::Err<void>(
                ErrorCode::InvalidArgument, "voice is not available");
        }
        return Ok();
    }

    Result<TranscriptionResult> transcribe(
        int, const TranscriptionRequest& request,
        const std::function<bool(int)>& progress = {}) override {
        if (transcription_should_cancel.load()) {
            return inferdeck::foundation::Err<TranscriptionResult>(
                ErrorCode::Cancelled, "cancelled");
        }
        if (const int delay = transcription_delay_ms.load(); delay > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds{delay});
        }
        if (progress && !progress(75)) return inferdeck::foundation::Err<TranscriptionResult>(ErrorCode::Cancelled, "cancelled");
        TranscriptionResult result;
        result.text = "test transcript";
        result.language = request.language.empty() ? "en" : request.language;
        result.duration_seconds = static_cast<float>(request.pcm.size()) / request.sample_rate;
        result.inference_ms = 7;
        TranscriptionSegment segment;
        segment.id = 0;
        segment.start_seconds = 0.0f;
        segment.end_seconds = 0.5f;
        segment.text = " test transcript";
        segment.tokens = {50364, 1234, 50389};
        segment.avg_logprob = -0.25f;
        segment.no_speech_probability = 0.01f;
        result.segments.push_back(std::move(segment));
        return Ok(std::move(result));
    }
};

ModelInfo make_info(const std::string& name) {
    ModelInfo m;
    m.name = name;
    m.family = "qwen3.6";
    m.gguf_path = "C:/fake/" + name + ".gguf";
    m.n_slots = 2;
    m.vram_required_mb = 8192;
    m.context_size = 65536;
    return m;
}

std::string test_wav() {
    std::string wav(44 + 320, '\0');
    auto put16 = [&wav](std::size_t at, std::uint16_t value) {
        wav[at] = static_cast<char>(value & 0xff);
        wav[at + 1] = static_cast<char>((value >> 8) & 0xff);
    };
    auto put32 = [&wav](std::size_t at, std::uint32_t value) {
        for (int byte = 0; byte < 4; ++byte) wav[at + byte] = static_cast<char>((value >> (8 * byte)) & 0xff);
    };
    std::memcpy(wav.data(), "RIFF", 4);
    put32(4, static_cast<std::uint32_t>(wav.size() - 8));
    std::memcpy(wav.data() + 8, "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, 1);
    put16(22, 1);
    put32(24, 16000);
    put32(28, 32000);
    put16(32, 2);
    put16(34, 16);
    std::memcpy(wav.data() + 36, "data", 4);
    put32(40, 320);
    return wav;
}

std::string test_float_wav(float sample) {
    std::string wav(48, '\0');
    auto put16 = [&wav](std::size_t at, std::uint16_t value) {
        wav[at] = static_cast<char>(value & 0xff);
        wav[at + 1] = static_cast<char>((value >> 8) & 0xff);
    };
    auto put32 = [&wav](std::size_t at, std::uint32_t value) {
        for (int byte = 0; byte < 4; ++byte) {
            wav[at + byte] =
                static_cast<char>((value >> (8 * byte)) & 0xff);
        }
    };
    std::memcpy(wav.data(), "RIFF", 4);
    put32(4, 40);
    std::memcpy(wav.data() + 8, "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, 3);
    put16(22, 1);
    put32(24, 16000);
    put32(28, 64000);
    put16(32, 4);
    put16(34, 32);
    std::memcpy(wav.data() + 36, "data", 4);
    put32(40, 4);
    std::memcpy(wav.data() + 44, &sample, sizeof(sample));
    return wav;
}

struct TestServer {
    ModelRegistry registry;
    BackendCoordinator coordinator;
    observability::Metrics metrics;
    observability::StatsDb stats_db{":memory:"};
    SwapTracker swap_tracker;
    httplib::Server server;
    std::thread th;
    int port{0};
    std::string voice_default_model;
    int voice_session_grace_ms{15000};
    CompatibilityProfile compatibility_profile{
        CompatibilityProfile::StrictOpenAI};

    TestServer() : coordinator(registry) {
        registry.set_factory([](const ModelInfo& info) {
            return std::make_unique<IModelMock>(info);
        });
        for (const std::string runtime : {"stable_diffusion_cpp", "sherpa_onnx", "whisper_cpp"}) {
            registry.register_factory(runtime, [](const ModelInfo& info) {
                return std::make_unique<IModelMock>(info);
            });
        }

        server.Get("/v1/models", [this](const httplib::Request& req, httplib::Response& resp) {
            handle_models(req, resp, make_deps());
        });
        server.Post("/api/inferdeck/v1/swap/to/:name",
                    [this](const httplib::Request& req, httplib::Response& resp) {
                        handle_swap_to(req, resp, make_deps(), req.path_params.at("name"));
                    });
        server.Get("/api/inferdeck/v1/swap/status", [this](const httplib::Request& req, httplib::Response& resp) {
            handle_swap_status(req, resp, make_deps());
        });
        server.Post("/v1/chat/completions",
                    [this](const httplib::Request& req, httplib::Response& resp) {
                        handle_chat_completions(req, resp, make_deps());
                    });
        server.Post("/v1/embeddings",
                    [this](const httplib::Request& req, httplib::Response& resp) {
                        handle_embeddings(req, resp, make_deps());
                    });
        server.Post("/v1/responses",
                    [this](const httplib::Request& req, httplib::Response& resp) {
                        handle_responses(req, resp, make_deps());
                    });
        server.Post("/v1/images/generations",
                    [this](const httplib::Request& req, httplib::Response& resp) {
                        handle_image_generations(req, resp, make_deps());
                    });
        server.Post("/v1/audio/speech",
                    [this](const httplib::Request& req, httplib::Response& resp) {
                        handle_audio_speech(req, resp, make_deps());
                    });
        server.Post("/v1/audio/transcriptions",
                    [this](const httplib::Request& req, httplib::Response& resp) {
                        handle_audio_transcriptions(req, resp, make_deps());
                    });
    }

    GatewayDeps make_deps() {
        GatewayDeps deps{coordinator, "10"};
        deps.auto_swap = false;
        deps.metrics = &metrics;
        deps.stats_db = &stats_db;
        deps.swap_tracker = &swap_tracker;
        deps.default_model = voice_default_model;
        deps.voice_session_grace_ms = voice_session_grace_ms;
        deps.compatibility_profile = compatibility_profile;
        return deps;
    }

    bool start() {
        int p = server.bind_to_any_port("127.0.0.1");
        if (p <= 0) return false;
        port = p;
        th = std::thread([this]() { server.listen_after_bind(); });
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        return true;
    }

    void stop() {
        server.stop();
        if (th.joinable()) th.join();
    }
};

bool wait_for_count(const std::atomic<int>& count, int minimum) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < deadline) {
        if (count.load() >= minimum) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    return count.load() >= minimum;
}

} // namespace

TEST_CASE("Route manifest matches the pinned strict OpenAI snapshot",
          "[routes][manifest]") {
    const auto fixture_path = std::filesystem::path(INFERDECK_SOURCE_DIR) /
        "tests/fixtures/openai_route_manifest.json";
    std::ifstream fixture_stream(fixture_path, std::ios::binary);
    REQUIRE(fixture_stream.good());
    const auto fixture = nlohmann::json::parse(fixture_stream);
    REQUIRE(fixture["baseline"].get<std::string>() ==
            std::string(kOpenAICompatibilityBaseline));
    REQUIRE(fixture["profile"].get<std::string>() ==
            std::string(kDefaultCompatibilityProfile));
    REQUIRE(fixture["sdk"]["python"].get<std::string>() ==
            std::string(kOpenAIPythonSdkVersion));
    REQUIRE(fixture["sdk"]["javascript"].get<std::string>() ==
            std::string(kOpenAIJavaScriptSdkVersion));
    REQUIRE(fixture["routes"].size() == kStrictOpenAIRoutes.size());
    for (std::size_t index = 0; index < kStrictOpenAIRoutes.size(); ++index) {
        CHECK(fixture["routes"][index]["method"].get<std::string>() ==
              std::string(kStrictOpenAIRoutes[index].method));
        CHECK(fixture["routes"][index]["path"].get<std::string>() ==
              std::string(kStrictOpenAIRoutes[index].path));
        CHECK(is_strict_openai_route(kStrictOpenAIRoutes[index].method,
                                     kStrictOpenAIRoutes[index].path));
    }
    REQUIRE(fixture["derivative_routes"].size() ==
            kOpenAIDerivativeRoutes.size());
    for (std::size_t index = 0; index < kOpenAIDerivativeRoutes.size(); ++index) {
        CHECK(fixture["derivative_routes"][index]["method"].get<std::string>() ==
              std::string(kOpenAIDerivativeRoutes[index].method));
        CHECK(fixture["derivative_routes"][index]["path"].get<std::string>() ==
              std::string(kOpenAIDerivativeRoutes[index].path));
    }

    const auto main_path = std::filesystem::path(INFERDECK_SOURCE_DIR) /
        "apps/inferdeck-gateway/src/main.cpp";
    std::ifstream main_stream(main_path, std::ios::binary);
    REQUIRE(main_stream.good());
    const std::string main_source(std::istreambuf_iterator<char>{main_stream}, {});
    CHECK(main_source.find("/v1/") == std::string::npos);
}

TEST_CASE("Strict JSON endpoints reject wrong media types before admission",
          "[routes][media-type][strict]") {
    TestServer server;
    REQUIRE(server.start());
    httplib::Client client("127.0.0.1", server.port);
    for (const std::string path : {
             "/v1/chat/completions", "/v1/responses", "/v1/embeddings",
             "/v1/images/generations", "/v1/audio/speech"}) {
        INFO(path);
        auto response = client.Post(path, "{}", "text/plain");
        REQUIRE(response);
        CHECK(response->status == 415);
        const auto error = nlohmann::json::parse(response->body)["error"];
        CHECK(error["code"] == "unsupported_media_type");
    }
    CHECK(server.metrics.total_requests() == 0);
    CHECK(server.coordinator.active_request_count() == 0);
    server.stop();
}

TEST_CASE("Malformed strict requests cannot mutate admission or residency state",
          "[routes][strict][mutation]") {
    TestServer server;
    REQUIRE(server.start());
    httplib::Client client("127.0.0.1", server.port);
    const auto initial_jobs = media_jobs().size();
    const auto initial_loaded = server.coordinator.get_loaded_models();

    const std::vector<std::tuple<std::string, nlohmann::json, std::string>>
        malformed_json{
        {"/v1/chat/completions",
         {{"model", "missing"}, {"messages", 42}}, "messages"},
        {"/v1/responses",
         {{"model", "missing"}, {"input", 42}}, "input"},
        {"/v1/embeddings",
         {{"model", "missing"}, {"input", nlohmann::json::object()}}, "input"},
        {"/v1/images/generations",
         {{"model", "missing"}, {"prompt", 42}}, "prompt"},
        {"/v1/audio/speech",
         {{"model", "missing"}, {"input", 42}, {"voice", "default"},
          {"response_format", "wav"}}, "input"},
    };
    for (const auto& [path, body, parameter] : malformed_json) {
        INFO(path);
        const auto response =
            client.Post(path, body.dump(), "application/json");
        REQUIRE(response);
        CHECK(response->status == 400);
        CHECK(nlohmann::json::parse(response->body)["error"]["param"] ==
              parameter);
    }

    const auto transcription = client.Post(
        "/v1/audio/transcriptions", httplib::UploadFormDataItems{
            {"file", test_wav(), "test.wav", "audio/wav"},
            {"model", "missing", "", ""},
            {"response_format", "future", "", ""},
        });
    REQUIRE(transcription);
    CHECK(transcription->status == 400);
    CHECK(nlohmann::json::parse(transcription->body)["error"]["param"] ==
          "response_format");

    CHECK(server.metrics.total_requests() == 0);
    CHECK(server.coordinator.active_request_count() == 0);
    CHECK(server.coordinator.queued_request_count() == 0);
    CHECK(server.coordinator.get_loaded_models() == initial_loaded);
    CHECK(media_jobs().size() == initial_jobs);
    CHECK(server.stats_db.recent_requests(1).empty());
    server.stop();
}

TEST_CASE("Strict Chat rejects derivative fields before admission",
          "[routes][chat][profile]") {
    TestServer ts;
    ts.registry.register_model(make_info("profile-model"));
    REQUIRE(ts.coordinator.load("profile-model"));
    auto strict_deps = ts.make_deps();
    const std::vector<std::pair<std::string, nlohmann::json>> extensions{
        {"priority", 1},
        {"chat_template_kwargs", nlohmann::json::object()},
        {"reasoning_content", "private"},
        {"reasoning_format", "none"},
        {"add_generation_prompt", false},
        {"grammar", "root ::= 'ok'"},
        {"json_schema", nlohmann::json::object()},
        {"future_extension", true},
    };
    for (const auto& [field, value] : extensions) {
        httplib::Request request;
        request.is_connection_closed = [] { return false; };
        nlohmann::json body{{"model", "profile-model"},
                            {"messages", nlohmann::json::array()}};
        body[field] = value;
        request.body = body.dump();
        httplib::Response response;
        handle_chat_completions(request, response, strict_deps);
        REQUIRE(response.status == 400);
        CHECK(nlohmann::json::parse(response.body)["error"]["code"] ==
              "unsupported_parameter");
        CHECK(ts.coordinator.active_request_count() == 0);
    }

    httplib::Request request;
    request.is_connection_closed = [] { return false; };
    request.body =
        R"({"model":"profile-model","messages":[{"role":"user","content":"test"}],"top_k":20})";
    auto derivative_deps = ts.make_deps();
    derivative_deps.compatibility_profile =
        CompatibilityProfile::OpenAIDerivative;
    httplib::Response derivative_response;
    handle_chat_completions(request, derivative_response, derivative_deps);
    REQUIRE(derivative_response.status == 200);
}

TEST_CASE("Strict Chat accepts the complete Open WebUI advanced payload",
          "[routes][chat][openwebui]") {
    TestServer ts;
    auto info = make_info("openwebui-model");
    info.context_size = 65536;
    ts.registry.register_model(info);
    REQUIRE(ts.coordinator.load("openwebui-model"));
    httplib::Request request;
    request.is_connection_closed = [] { return false; };
    request.body = nlohmann::json{
        {"model", "openwebui-model"},
        {"messages", nlohmann::json::array({
            {{"role", "user"}, {"content", "test"}}
        })},
        {"frequency_penalty", 1.2},
        {"num_ctx", 32768},
        {"num_predict", 64},
        {"top_k", 20},
        {"min_p", 0.1},
        {"repeat_penalty", 1.1},
        {"repeat_last_n", 64},
        {"mirostat", 2},
        {"mirostat_eta", 0.1},
        {"mirostat_tau", 5.0},
        {"tfs_z", 1.0},
        {"num_keep", 256},
        {"num_batch", 512},
        {"num_thread", 8},
        {"num_gpu", 99},
        {"use_mmap", true},
        {"use_mlock", false},
        {"keep_alive", "5m"},
        {"think", false},
        {"format", "json"}
    }.dump();
    httplib::Response response;
    handle_chat_completions(request, response, ts.make_deps());
    REQUIRE(response.status == 200);
    const auto* backend = dynamic_cast<const IModelMock*>(
        ts.coordinator.get_backend("openwebui-model"));
    REQUIRE(backend != nullptr);
    CHECK(backend->last_request.context_window == 32768);
    CHECK(backend->last_request.prompt_keep_tokens == 256);
    CHECK(backend->last_request.max_output_tokens == 64);
    CHECK(backend->last_request.sampling.top_k == 20);
    CHECK(backend->last_request.sampling.min_p == 0.1f);
    CHECK(backend->last_request.sampling.repeat_penalty == 1.1f);
    CHECK(backend->last_request.sampling.repeat_last_n == 64);
    CHECK(backend->last_request.sampling.mirostat == 2);
    CHECK(backend->last_request.sampling.mirostat_eta == 0.1f);
    CHECK(backend->last_request.sampling.mirostat_tau == 5.0f);
    CHECK(backend->last_request.sampling.tfs_z == 1.0f);
    CHECK(backend->last_request.enable_reasoning == false);
    CHECK(backend->last_request.output.kind ==
          inference::StructuredOutputKind::JsonObject);

    auto too_large = nlohmann::json::parse(request.body);
    too_large["num_ctx"] = 65537;
    request.body = too_large.dump();
    handle_chat_completions(request, response, ts.make_deps());
    REQUIRE(response.status == 400);
    const auto error = nlohmann::json::parse(response.body)["error"];
    CHECK(error["param"] == "num_ctx");
    CHECK(ts.coordinator.active_request_count() == 0);
}

TEST_CASE("Routes: GET /v1/models lists registered models", "[routes][models]") {
    TestServer ts;
    auto reasoning_model = make_info("qwen3.6-27b");
    reasoning_model.reasoning.supported = true;
    reasoning_model.reasoning.efforts = {"low", "medium", "high"};
    reasoning_model.reasoning.default_effort = "medium";
    reasoning_model.reasoning.none_disables = true;
    ts.registry.register_model(reasoning_model);
    ts.registry.register_model(make_info("qwen3-coder-next"));
    REQUIRE(ts.start());

    httplib::Client cli("127.0.0.1", ts.port);
    auto res = cli.Get("/v1/models");
    REQUIRE(res);
    REQUIRE(res->status == 200);

    auto body = nlohmann::json::parse(res->body);
    check_schema(body, "models");
    REQUIRE(body["object"] == "list");
    REQUIRE(body["data"].is_array());
    REQUIRE(body["data"].size() == 2);
    REQUIRE(body["data"][0]["object"] == "model");
    check_schema(body["data"][0], "model");
    REQUIRE(body["data"][0].size() == 4);
    const auto reasoning_entry = std::find_if(
        body["data"].begin(), body["data"].end(), [](const auto& entry) {
            return entry.value("id", "") == "qwen3.6-27b";
        });
    REQUIRE(reasoning_entry != body["data"].end());
    REQUIRE(reasoning_entry->size() == 4);
    ts.stop();
}

TEST_CASE("Strict Chat preserves native sampling and logprobs end to end",
          "[routes][chat][sampling]") {
    TestServer ts;
    ts.registry.register_model(make_info("penalty-model"));
    REQUIRE(ts.coordinator.load("penalty-model"));
    httplib::Request request;
    request.is_connection_closed = [] { return false; };
    request.body = nlohmann::json{
        {"model", "penalty-model"},
        {"messages", nlohmann::json::array({
            {{"role", "user"}, {"content", "test"}}
        })},
        {"frequency_penalty", 1.2},
        {"presence_penalty", -0.4},
        {"logit_bias", {{"42", 6.5}}},
        {"logprobs", true},
        {"top_logprobs", 1},
        {"service_tier", "default"}
    }.dump();
    httplib::Response response;
    handle_chat_completions(request, response, ts.make_deps());
    REQUIRE(response.status == 200);
    const auto* backend = dynamic_cast<const IModelMock*>(
        ts.coordinator.get_backend("penalty-model"));
    REQUIRE(backend != nullptr);
    REQUIRE(backend->last_request.sampling.frequency_penalty.has_value());
    CHECK(*backend->last_request.sampling.frequency_penalty == 1.2f);
    REQUIRE(backend->last_request.sampling.presence_penalty.has_value());
    CHECK(*backend->last_request.sampling.presence_penalty == -0.4f);
    REQUIRE(backend->last_request.sampling.logit_bias.size() == 1);
    CHECK(backend->last_request.sampling.logit_bias[0].first == 42);
    CHECK(backend->last_request.sampling.logit_bias[0].second == 6.5f);
    CHECK(backend->last_request.logprobs);
    CHECK(backend->last_request.top_logprobs == 1);
    const auto body = nlohmann::json::parse(response.body);
    CHECK(body["service_tier"] == "default");
    const auto& logprobs = body["choices"][0]["logprobs"];
    REQUIRE(logprobs["content"].size() == 1);
    CHECK(logprobs["content"][0]["token"] == "Hello");
    CHECK(logprobs["content"][0]["logprob"] == Catch::Approx(-0.125));
    CHECK(logprobs["content"][0]["bytes"] ==
          nlohmann::json::array({72, 101, 108, 108, 111}));
}

TEST_CASE("Routes: GET /v1/models does not expose residency", "[routes][models]") {
    TestServer ts;
    ts.registry.register_model(make_info("qwen3.6-27b"));
    auto r = ts.coordinator.load("qwen3.6-27b");
    REQUIRE(r);
    REQUIRE(ts.start());

    httplib::Client cli("127.0.0.1", ts.port);
    auto res = cli.Get("/v1/models");
    REQUIRE(res);
    auto body = nlohmann::json::parse(res->body);
    REQUIRE(body["data"].size() == 1);
    REQUIRE_FALSE(body["data"][0].contains("loaded"));
    REQUIRE_FALSE(body["data"][0].contains("inferdeck"));
    ts.stop();
}

TEST_CASE("Routes: stable aliases resolve while preserving request attribution",
          "[routes][models][aliases]") {
    TestServer ts;
    auto concrete = make_info("qwen-concrete");
    concrete.capabilities = {"chat_completions", "responses"};
    ts.registry.register_model(concrete);
    REQUIRE(ts.registry.set_alias({"assistant-stable", "qwen-concrete", 65536,
                                   {"chat_completions"}}));
    REQUIRE(ts.coordinator.load("qwen-concrete"));
    REQUIRE(ts.start());

    httplib::Client client("127.0.0.1", ts.port);
    const auto models_response = client.Get("/v1/models");
    REQUIRE(models_response);
    REQUIRE(models_response->status == 200);
    const auto models = nlohmann::json::parse(models_response->body)["data"];
    const auto alias = std::find_if(models.begin(), models.end(), [](const auto& entry) {
        return entry.value("id", "") == "assistant-stable";
    });
    REQUIRE(alias != models.end());
    CHECK(alias->size() == 4);

    const auto chat_response = client.Post(
        "/v1/chat/completions",
        R"({"model":"assistant-stable","messages":[{"role":"user","content":"hi"}]})",
        "application/json");
    REQUIRE(chat_response);
    REQUIRE(chat_response->status == 200);
    const auto chat = nlohmann::json::parse(chat_response->body);
    CHECK(chat["model"] == "assistant-stable");
    check_schema(chat, "chat_completion");
    check_schema(chat["choices"][0], "chat_choice");
    check_schema(chat["choices"][0]["message"], "chat_message");
    check_schema(chat["usage"], "chat_usage");

    const auto stream_response = client.Post(
        "/v1/chat/completions",
        R"({"model":"assistant-stable","stream":true,"stream_options":{"include_obfuscation":false},"messages":[{"role":"user","content":"hi again"}]})",
        "application/json");
    REQUIRE(stream_response);
    REQUIRE(stream_response->status == 200);
    REQUIRE(stream_response->get_header_value_count("Content-Type") == 1);
    CHECK(stream_response->get_header_value("Content-Type") == "text/event-stream");
    std::size_t position = 0;
    while (position < stream_response->body.size()) {
        auto newline = stream_response->body.find('\n', position);
        if (newline == std::string::npos) newline = stream_response->body.size();
        const auto line = stream_response->body.substr(position, newline - position);
        position = newline + 1;
        if (!line.starts_with("data: ") || line == "data: [DONE]") continue;
        const auto chunk = nlohmann::json::parse(line.substr(6));
        check_schema(chunk, chunk.contains("usage")
                                ? "chat_stream_chunk"
                                : "chat_stream_chunk_without_usage");
        if (chunk.contains("model")) {
            CHECK(chunk["model"] == "assistant-stable");
        }
    }
    const auto rows = ts.stats_db.recent_requests(1);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].model == "assistant-stable");
    CHECK(rows[0].resolved_model == "qwen-concrete");
    ts.stop();
}

TEST_CASE("Strict Chat never emits derivative reasoning fields",
          "[routes][chat][profile][reasoning]") {
    TestServer ts;
    ts.registry.register_model(make_info("strict-reasoning"));
    REQUIRE(ts.coordinator.load("strict-reasoning"));
    REQUIRE(ts.start());
    httplib::Client client("127.0.0.1", ts.port);
    const std::string tools = R"(
      "tools":[{"type":"function","function":{"name":"list_workspace","parameters":{"type":"object"}}}]
    )";

    const auto nonstream = client.Post(
        "/v1/chat/completions",
        "{\"model\":\"strict-reasoning\",\"messages\":[{\"role\":\"user\",\"content\":\"list files\"}]," +
            tools + "}",
        "application/json");
    REQUIRE(nonstream);
    REQUIRE(nonstream->status == 200);
    CHECK_FALSE(nlohmann::json::parse(nonstream->body)["choices"][0]["message"]
                    .contains("reasoning_content"));

    const auto stream = client.Post(
        "/v1/chat/completions",
        "{\"model\":\"strict-reasoning\",\"stream\":true,\"messages\":[{\"role\":\"user\",\"content\":\"list files\"}]," +
            tools + "}",
        "application/json");
    REQUIRE(stream);
    REQUIRE(stream->status == 200);
    CHECK(stream->body.find("reasoning_content") == std::string::npos);
    CHECK(stream->body.ends_with("data: [DONE]\n\n"));
    ts.stop();
}

TEST_CASE("Routes: POST control swap to missing returns 404", "[routes][swap]") {
    TestServer ts;
    REQUIRE(ts.start());
    httplib::Client cli("127.0.0.1", ts.port);
    auto res = cli.Post("/api/inferdeck/v1/swap/to/missing", "", "application/json");
    REQUIRE(res);
    REQUIRE(res->status == 404);
    const auto error = nlohmann::json::parse(res->body)["error"];
    REQUIRE(error["message"] == "model not registered: missing");
    REQUIRE(error["type"] == "invalid_request_error");
    REQUIRE(error["param"].is_null());
    REQUIRE(error["code"] == "model_not_found");
    ts.stop();
}

TEST_CASE("Routes: POST control swap to loaded returns 200", "[routes][swap]") {
    TestServer ts;
    ts.registry.register_model(make_info("qwen3.6-27b"));
    REQUIRE(ts.coordinator.load("qwen3.6-27b"));
    REQUIRE(ts.start());
    httplib::Client cli("127.0.0.1", ts.port);
    auto res = cli.Post("/api/inferdeck/v1/swap/to/qwen3.6-27b", "", "application/json");
    REQUIRE(res);
    REQUIRE(res->status == 200);
    auto body = nlohmann::json::parse(res->body);
    REQUIRE(body["status"] == "ready");
    ts.stop();
}

TEST_CASE("Routes: GET control swap status returns model info", "[routes][swap]") {
    TestServer ts;
    ts.registry.register_model(make_info("qwen3.6-27b"));
    REQUIRE(ts.coordinator.load("qwen3.6-27b"));
    REQUIRE(ts.start());
    httplib::Client cli("127.0.0.1", ts.port);
    auto res = cli.Get("/api/inferdeck/v1/swap/status");
    REQUIRE(res);
    REQUIRE(res->status == 200);
    auto body = nlohmann::json::parse(res->body);
    REQUIRE(body["loaded_model"] == "qwen3.6-27b");
    REQUIRE(body["loaded_models"] == nlohmann::json::array({"qwen3.6-27b"}));
    REQUIRE(body["identities"]["selected"] == "qwen3.6-27b");
    REQUIRE(body["identities"]["resident"] == nlohmann::json::array({"qwen3.6-27b"}));
    REQUIRE(body["identities"]["executing"] == nlohmann::json::array());
    REQUIRE(body["residency"].size() == 1);
    REQUIRE(body["residency"][0]["runtime"] == "llama_cpp");
    REQUIRE(body["active_requests"] == 0);
    ts.stop();
}

TEST_CASE("Routes: voice sessions require opaque principal-scoped identity",
          "[routes][voice][security]") {
    httplib::Request first;
    first.remote_addr = "192.0.2.10";
    first.headers.emplace("Authorization", "Bearer principal-a");
    first.headers.emplace("X-InferDeck-Voice-Session", "session-0001");
    httplib::Request second = first;
    second.headers.erase("Authorization");
    second.headers.emplace("Authorization", "Bearer principal-b");
    httplib::Request same_principal = first;
    same_principal.headers.erase("X-InferDeck-Voice-Session");
    same_principal.headers.emplace("X-InferDeck-Voice-Session", "session-0002");

    const auto first_key = request_client_key(first);
    REQUIRE_FALSE(first_key.empty());
    REQUIRE(request_client_key(second) != first_key);
    REQUIRE(request_client_key(same_principal) != first_key);

    httplib::Request missing_session = first;
    missing_session.headers.erase("X-InferDeck-Voice-Session");
    REQUIRE(request_client_key(missing_session).empty());
    httplib::Request missing_principal = first;
    missing_principal.headers.erase("Authorization");
    REQUIRE(request_client_key(missing_principal).empty());
    httplib::Request invalid_session = first;
    invalid_session.headers.erase("X-InferDeck-Voice-Session");
    invalid_session.headers.emplace("X-InferDeck-Voice-Session", "bad session");
    REQUIRE(request_client_key(invalid_session).empty());
}

TEST_CASE("Routes: POST /v1/chat/completions missing model returns 400", "[routes][chat]") {
    TestServer ts;
    REQUIRE(ts.start());
    httplib::Client cli("127.0.0.1", ts.port);
    auto res = cli.Post("/v1/chat/completions",
                        R"({"messages":[{"role":"user","content":"hi"}]})",
                        "application/json");
    REQUIRE(res);
    REQUIRE(res->status == 400);
    const auto error = nlohmann::json::parse(res->body)["error"];
    REQUIRE(error["message"] == "request body must include 'model'");
    REQUIRE(error["type"] == "invalid_request_error");
    REQUIRE(error["param"] == "model");
    REQUIRE(error["code"] == "missing_model");
    ts.stop();
}

TEST_CASE("Routes: malformed chat parameters fail before slot admission",
          "[routes][chat][validation]") {
    TestServer ts;
    ts.registry.register_model(make_info("validation-model"));
    REQUIRE(ts.coordinator.load("validation-model"));
    REQUIRE(ts.start());

    httplib::Client client("127.0.0.1", ts.port);
    auto response = client.Post(
        "/v1/chat/completions",
        R"({"model":"validation-model","stream":"yes","messages":[]})",
        "application/json");
    REQUIRE(response);
    REQUIRE(response->status == 400);
    REQUIRE(nlohmann::json::parse(response->body)["error"]["code"] ==
            "invalid_request_error");
    REQUIRE(ts.coordinator.active_request_count() == 0);

    response = client.Post(
        "/v1/chat/completions",
        R"({"model":"validation-model","temperature":"hot","messages":[]})",
        "application/json");
    REQUIRE(response);
    REQUIRE(response->status == 400);
    REQUIRE(nlohmann::json::parse(response->body)["error"]["code"] ==
            "invalid_request_error");
    REQUIRE(ts.coordinator.active_request_count() == 0);

    response = client.Post(
        "/v1/chat/completions",
        R"({"model":"validation-model","stream":true,"stream_options":{"include_usage":"yes"},"messages":[]})",
        "application/json");
    REQUIRE(response);
    REQUIRE(response->status == 400);
    REQUIRE(nlohmann::json::parse(response->body)["error"]["code"] ==
            "invalid_request_error");
    REQUIRE(ts.coordinator.active_request_count() == 0);

    response = client.Post(
        "/v1/chat/completions",
        R"({"model":"validation-model","stream":true,"stream_options":"usage","messages":[]})",
        "application/json");
    REQUIRE(response);
    REQUIRE(response->status == 400);
    REQUIRE(nlohmann::json::parse(response->body)["error"]["code"] ==
            "invalid_request_error");
    REQUIRE(ts.coordinator.active_request_count() == 0);

    response = client.Post(
        "/v1/chat/completions",
        R"({"model":"validation-model","stream_options":null,"messages":[]})",
        "application/json");
    REQUIRE(response);
    REQUIRE(response->status == 400);
    REQUIRE(nlohmann::json::parse(response->body)["error"]["code"] ==
            "invalid_request_error");
    REQUIRE(ts.coordinator.active_request_count() == 0);
    ts.stop();
}

TEST_CASE("Routes: complete Chat shape validates before model resolution",
          "[routes][chat][validation]") {
    TestServer ts;
    const auto base = [] {
        return nlohmann::json{
            {"model", "missing-model"},
            {"messages", nlohmann::json::array({
                {{"role", "user"}, {"content", "hello"}},
            })},
        };
    };
    std::vector<nlohmann::json> invalid;
    auto add = [&invalid, &base](const std::string& field,
                                 nlohmann::json value) {
        auto body = base();
        body[field] = std::move(value);
        invalid.push_back(std::move(body));
    };
    add("messages", nlohmann::json::array());
    add("temperature", 2.1);
    add("top_p", -0.1);
    add("max_completion_tokens", 0);
    add("seed", 1.5);
    add("parallel_tool_calls", "yes");
    add("stop", nlohmann::json::array({"a", "b", "c", "d", "e"}));
    add("stream_options", {{"include_usage", true}, {"unknown", true}});
    add("tools", nlohmann::json::array({
        {{"type", "function"},
         {"function", {{"name", "lookup"},
                        {"parameters", nlohmann::json::array()}}}},
    }));
    add("tool_choice", {{"type", "function"},
                        {"function", {{"name", ""}}}});
    add("response_format",
        {{"type", "json_schema"},
         {"json_schema", {{"schema", {{"type", "object"}}}}}});
    auto both_limits = base();
    both_limits["max_tokens"] = 8;
    both_limits["max_completion_tokens"] = 8;
    invalid.push_back(std::move(both_limits));
    auto unknown_message = base();
    unknown_message["messages"][0]["reasoning_content"] = "private";
    invalid.push_back(std::move(unknown_message));
    auto missing_content = base();
    missing_content["messages"][0].erase("content");
    invalid.push_back(std::move(missing_content));
    auto responses_text = base();
    responses_text["messages"][0]["content"] = nlohmann::json::array({
        {{"type", "input_text"}, {"text", "wrong protocol"}},
    });
    invalid.push_back(std::move(responses_text));
    auto unknown_content_field = base();
    unknown_content_field["messages"][0]["content"] = nlohmann::json::array({
        {{"type", "text"}, {"text", "hello"}, {"ignored", true}},
    });
    invalid.push_back(std::move(unknown_content_field));
    auto malformed_tool_call = base();
    malformed_tool_call["messages"] = nlohmann::json::array({
        {{"role", "assistant"},
         {"content", nullptr},
         {"tool_calls", nlohmann::json::array({
             {{"id", "call_1"},
              {"type", "function"},
              {"function", {{"name", "lookup"},
                            {"arguments", nlohmann::json::object()}}}},
         })}},
    });
    invalid.push_back(std::move(malformed_tool_call));
    auto missing_tool_call_id = base();
    missing_tool_call_id["messages"][0] = {
        {"role", "tool"}, {"content", "result"},
    };
    invalid.push_back(std::move(missing_tool_call_id));
    add("response_format",
        {{"type", "text"}, {"json_schema", nlohmann::json::object()}});

    for (const auto& body : invalid) {
        httplib::Request request;
        request.body = body.dump();
        httplib::Response response;
        handle_chat_completions(request, response, ts.make_deps());
        INFO(body.dump());
        CHECK(response.status == 400);
        CHECK(ts.coordinator.active_request_count() == 0);
        CHECK(ts.metrics.total_requests() == 0);
    }
}

TEST_CASE("Routes: Chat stream serializers preserve exact OpenAI event ordering",
          "[routes][chat][stream][golden]") {
    REQUIRE(serialize_chat_stream_delta(
        "chatcmpl-test", "model", 123, {{"content", "Hi"}}, true,
        false, "", false) ==
        "data: {\"choices\":[{\"delta\":{\"content\":\"Hi\"},\"finish_reason\":null,\"index\":0}],\"created\":123,\"id\":\"chatcmpl-test\",\"model\":\"model\",\"object\":\"chat.completion.chunk\",\"usage\":null}\n\n");
    REQUIRE(serialize_chat_stream_delta(
        "chatcmpl-test", "model", 123,
        {{"reasoning_content", "thinking"}}, true, true, "", false) ==
        "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"thinking\"},\"finish_reason\":null,\"index\":0}],\"created\":123,\"id\":\"chatcmpl-test\",\"model\":\"model\",\"object\":\"chat.completion.chunk\",\"usage\":null}\n\n");
    REQUIRE(serialize_chat_stream_delta(
        "chatcmpl-test", "model", 123,
        {{"reasoning_content", "thinking"}}, true, false, "", false) ==
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":null,\"index\":0}],\"created\":123,\"id\":\"chatcmpl-test\",\"model\":\"model\",\"object\":\"chat.completion.chunk\",\"usage\":null}\n\n");
    REQUIRE(serialize_chat_stream_delta(
        "chatcmpl-test", "model", 123,
        {{"tool_calls", nlohmann::json::array({{
            {"index", 0}, {"id", "call_1"}, {"type", "function"},
            {"function", {{"name", "f"}, {"arguments", "{}"}}},
        }})}}, true, false, "", false) ==
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"function\":{\"arguments\":\"{}\",\"name\":\"f\"},\"id\":\"call_1\",\"index\":0,\"type\":\"function\"}]},\"finish_reason\":null,\"index\":0}],\"created\":123,\"id\":\"chatcmpl-test\",\"model\":\"model\",\"object\":\"chat.completion.chunk\",\"usage\":null}\n\n");

    InferenceResult result;
    result.prompt_tokens = 8;
    result.cached_prompt_tokens = 3;
    result.completion_tokens = 12;
    REQUIRE(serialize_chat_stream_terminal(
        "chatcmpl-test", "model", 123, "tool_calls", &result, true,
        "", false) ==
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"tool_calls\",\"index\":0}],\"created\":123,\"id\":\"chatcmpl-test\",\"model\":\"model\",\"object\":\"chat.completion.chunk\",\"usage\":null}\n\ndata: {\"choices\":[],\"created\":123,\"id\":\"chatcmpl-test\",\"model\":\"model\",\"object\":\"chat.completion.chunk\",\"usage\":{\"completion_tokens\":12,\"prompt_tokens\":8,\"prompt_tokens_details\":{\"cached_tokens\":3},\"total_tokens\":20}}\n\ndata: [DONE]\n\n");
    REQUIRE(serialize_chat_stream_terminal(
        "chatcmpl-test", "model", 123, "stop", &result, false,
        "", false) ==
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\",\"index\":0}],\"created\":123,\"id\":\"chatcmpl-test\",\"model\":\"model\",\"object\":\"chat.completion.chunk\"}\n\ndata: [DONE]\n\n");

    std::string actual;
    actual += serialize_chat_stream_delta(
        "chatcmpl-contract", "contract-model", 1787171200,
        {{"content", "Hi"}}, true, false, "", false);
    actual += serialize_chat_stream_delta(
        "chatcmpl-contract", "contract-model", 1787171200,
        {{"tool_calls", nlohmann::json::array({{
            {"index", 0}, {"id", "call_1"}, {"type", "function"},
            {"function", {{"name", "lookup"}, {"arguments", ""}}},
        }})}}, true, false, "", false);
    actual += serialize_chat_stream_delta(
        "chatcmpl-contract", "contract-model", 1787171200,
        {{"tool_calls", nlohmann::json::array({{
            {"index", 0}, {"function", {{"arguments", "{}"}}},
        }})}}, true, false, "", false);
    actual += serialize_chat_stream_terminal(
        "chatcmpl-contract", "contract-model", 1787171200,
        "tool_calls", &result, true, "", false);

    const auto fixture_path = std::filesystem::path(INFERDECK_SOURCE_DIR) /
        "tests/fixtures/oai_chat_stream_contract.sse";
    std::ifstream fixture_stream(fixture_path, std::ios::binary);
    REQUIRE(fixture_stream.good());
    const std::string fixture(std::istreambuf_iterator<char>{fixture_stream}, {});
    REQUIRE(actual == fixture + "\n");
}

TEST_CASE("Routes: POST /v1/chat/completions unknown model returns 404", "[routes][chat]") {
    TestServer ts;
    REQUIRE(ts.start());
    httplib::Client cli("127.0.0.1", ts.port);
    auto res = cli.Post("/v1/chat/completions",
                        R"({"model":"missing","messages":[{"role":"user","content":"hi"}]})",
                        "application/json");
    REQUIRE(res);
    REQUIRE(res->status == 404);
    ts.stop();
}

TEST_CASE("Routes: POST /v1/chat/completions unloaded model returns 503+Retry-After",
          "[routes][chat][503]") {
    TestServer ts;
    ts.registry.register_model(make_info("qwen3.6-27b"));
    REQUIRE(ts.start());
    httplib::Client cli("127.0.0.1", ts.port);
    auto res = cli.Post("/v1/chat/completions",
                        R"({"model":"qwen3.6-27b","messages":[{"role":"user","content":"hi"}]})",
                        "application/json");
    REQUIRE(res);
    REQUIRE(res->status == 503);
    REQUIRE(res->get_header_value("Retry-After") == "10");
    auto body = nlohmann::json::parse(res->body);
    REQUIRE(body["error"]["code"] == "model_not_loaded");
    ts.stop();
}

TEST_CASE("Routes: POST /v1/chat/completions loaded model returns 200 with content",
          "[routes][chat]") {
    TestServer ts;
    ts.registry.register_model(make_info("qwen3.6-27b"));
    REQUIRE(ts.coordinator.load("qwen3.6-27b"));
    REQUIRE(ts.start());
    httplib::Client cli("127.0.0.1", ts.port);
    auto res = cli.Post("/v1/chat/completions",
                        R"({"model":"qwen3.6-27b","messages":[{"role":"user","content":"hi"}],"stream":false})",
                        "application/json");
    REQUIRE(res);
    REQUIRE(res->status == 200);
    auto body = nlohmann::json::parse(res->body);
    REQUIRE(body["choices"][0]["message"]["content"] == "Hello from model");
    REQUIRE(body["usage"]["completion_tokens"] == 4);
    REQUIRE(ts.metrics.total_requests() == 1);
    REQUIRE(ts.metrics.total_prompt_tokens() == 3);
    REQUIRE(ts.metrics.total_completion_tokens() == 4);
    auto usage = ts.stats_db.model_usage();
    REQUIRE(usage.size() == 1);
    REQUIRE(usage[0].model == "qwen3.6-27b");
    REQUIRE(usage[0].prompt_tokens == 3);
    REQUIRE(usage[0].completion_tokens == 4);
    const auto* backend = dynamic_cast<const IModelMock*>(
        ts.coordinator.get_backend("qwen3.6-27b"));
    REQUIRE(backend);
    REQUIRE(backend->last_max_tokens.load() == k_max_tokens_use_context_budget);
    ts.stop();
}

TEST_CASE("Routes: typed context errors map without message matching",
          "[routes][chat][stream][errors]") {
    TestServer ts;
    ts.registry.register_model(make_info("context-model"));
    REQUIRE(ts.coordinator.load("context-model"));
    const auto* backend = dynamic_cast<const IModelMock*>(
        ts.coordinator.get_backend("context-model"));
    REQUIRE(backend);
    const_cast<IModelMock*>(backend)->context_error.store(true);
    REQUIRE(ts.start());

    httplib::Client client("127.0.0.1", ts.port);
    auto response = client.Post(
        "/v1/chat/completions",
        R"({"model":"context-model","stream":false,"messages":[{"role":"user","content":"test"}]})",
        "application/json");
    REQUIRE(response);
    REQUIRE(response->status == 400);
    REQUIRE(nlohmann::json::parse(response->body)["error"]["code"] ==
            "context_length_exceeded");

    response = client.Post(
        "/v1/chat/completions",
        R"({"model":"context-model","stream":true,"messages":[{"role":"user","content":"test"}]})",
        "application/json");
    REQUIRE(response);
    REQUIRE(response->status == 200);
    const auto error_line_start = response->body.find("data: ");
    REQUIRE(error_line_start != std::string::npos);
    const auto error_line_end = response->body.find('\n', error_line_start);
    const auto error = nlohmann::json::parse(response->body.substr(
        error_line_start + 6, error_line_end - error_line_start - 6))["error"];
    REQUIRE(error["message"] == "prompt is too long");
    REQUIRE(error["type"] == "invalid_request_error");
    REQUIRE(error["param"].is_null());
    REQUIRE(error["code"] == "context_length_exceeded");
    REQUIRE(response->body ==
        "data: {\"error\":{\"code\":\"context_length_exceeded\",\"message\":\"prompt is too long\",\"param\":null,\"type\":\"invalid_request_error\"}}\n\ndata: [DONE]\n\n");
    ts.stop();
}

TEST_CASE("Routes: POST /v1/chat/completions rejects non-canonical latest alias",
          "[routes][chat][strict]") {
    TestServer ts;
    ts.registry.register_model(make_info("qwen3.6-27b"));
    REQUIRE(ts.coordinator.load("qwen3.6-27b"));
    REQUIRE(ts.start());
    httplib::Client cli("127.0.0.1", ts.port);
    auto res = cli.Post("/v1/chat/completions",
                        R"({"model":"qwen3.6-27b:latest","messages":[{"role":"user","content":"hi"}],"stream":false})",
                        "application/json");
    REQUIRE(res);
    REQUIRE(res->status == 404);
    REQUIRE(nlohmann::json::parse(res->body)["error"]["code"] ==
            "model_not_found");
    CHECK(ts.metrics.total_requests() == 0);
    ts.stop();
}

TEST_CASE("Routes: POST /v1/embeddings returns ordered float vectors", "[routes][embeddings]") {
    TestServer ts;
    auto info = make_info("embed-model");
    info.modality = "embedding";
    info.capabilities = {"embeddings"};
    ts.registry.register_model(info);
    REQUIRE(ts.coordinator.load(info.name));
    REQUIRE(ts.start());

    httplib::Client client("127.0.0.1", ts.port);
    auto response = client.Post("/v1/embeddings", nlohmann::json{
        {"model", info.name},
        {"input", nlohmann::json::array({"one", "two"})},
        {"dimensions", 2},
    }.dump(), "application/json");
    REQUIRE(response);
    REQUIRE(response->status == 200);
    const auto body = nlohmann::json::parse(response->body);
    check_schema(body, "embeddings");
    REQUIRE(body["object"] == "list");
    REQUIRE(body["model"] == info.name);
    REQUIRE(body["data"].size() == 2);
    REQUIRE(body["data"][0]["index"] == 0);
    check_schema(body["data"][0], "embedding");
    check_schema(body["usage"], "embedding_usage");
    REQUIRE(body["data"][0]["embedding"] == nlohmann::json::array({1.0, 1.0}));
    REQUIRE(body["data"][1]["embedding"] == nlohmann::json::array({2.0, 2.0}));
    REQUIRE(body["usage"]["prompt_tokens"] == 6);
    REQUIRE(body["usage"]["total_tokens"] == 6);
    ts.stop();
}

TEST_CASE("Routes: POST /v1/embeddings supports base64 encoding", "[routes][embeddings]") {
    TestServer ts;
    auto info = make_info("embed-model");
    info.modality = "embedding";
    info.capabilities = {"embeddings"};
    ts.registry.register_model(info);
    REQUIRE(ts.coordinator.load(info.name));
    REQUIRE(ts.start());

    httplib::Client client("127.0.0.1", ts.port);
    auto response = client.Post("/v1/embeddings", nlohmann::json{
        {"model", info.name}, {"input", "one"}, {"encoding_format", "base64"},
    }.dump(), "application/json");
    REQUIRE(response);
    REQUIRE(response->status == 200);
    const auto embedding = nlohmann::json::parse(response->body)["data"][0]["embedding"];
    REQUIRE(embedding.is_string());
    REQUIRE(embedding.get<std::string>() == "AACAPwAAgD8AAIA/");
    ts.stop();
}

TEST_CASE("Routes: POST /v1/embeddings preserves token input shapes and order",
          "[routes][embeddings]") {
    TestServer ts;
    auto info = make_info("embed-model");
    info.modality = "embedding";
    info.capabilities = {"embeddings"};
    ts.registry.register_model(info);
    REQUIRE(ts.coordinator.load(info.name));
    REQUIRE(ts.start());

    httplib::Client client("127.0.0.1", ts.port);
    auto vector_response = client.Post("/v1/embeddings", nlohmann::json{
        {"model", info.name}, {"input", nlohmann::json::array({10, 20, 30})},
    }.dump(), "application/json");
    REQUIRE(vector_response);
    REQUIRE(vector_response->status == 200);
    const auto vector_body = nlohmann::json::parse(vector_response->body);
    REQUIRE(vector_body["data"].size() == 1);
    REQUIRE(vector_body["data"][0]["index"] == 0);
    REQUIRE(vector_body["usage"]["prompt_tokens"] == 3);

    auto matrix_response = client.Post("/v1/embeddings", nlohmann::json{
        {"model", info.name},
        {"input", nlohmann::json::array({
            nlohmann::json::array({1, 2}),
            nlohmann::json::array({3, 4, 5}),
        })},
    }.dump(), "application/json");
    REQUIRE(matrix_response);
    REQUIRE(matrix_response->status == 200);
    const auto matrix_body = nlohmann::json::parse(matrix_response->body);
    REQUIRE(matrix_body["data"].size() == 2);
    REQUIRE(matrix_body["data"][0]["index"] == 0);
    REQUIRE(matrix_body["data"][0]["embedding"] ==
            nlohmann::json::array({1.0, 1.0, 1.0}));
    REQUIRE(matrix_body["data"][1]["index"] == 1);
    REQUIRE(matrix_body["data"][1]["embedding"] ==
            nlohmann::json::array({2.0, 2.0, 2.0}));
    REQUIRE(matrix_body["usage"]["prompt_tokens"] == 5);
    ts.stop();
}

TEST_CASE("Routes: embeddings validate strict fields before acquisition",
          "[routes][embeddings][validation]") {
    TestServer server;
    auto info = make_info("embed-model");
    info.modality = "embedding";
    info.capabilities = {"embeddings"};
    server.registry.register_model(info);
    const std::vector<nlohmann::json> invalid{
        {{"model", info.name}, {"input", "one"}, {"priority", 1}},
        {{"model", info.name}, {"input", "one"}, {"unknown", true}},
        {{"model", info.name}, {"input", "one"}, {"dimensions", 0}},
        {{"model", info.name}, {"input", "one"}, {"dimensions", 65537}},
        {{"model", info.name}, {"input", "one"}, {"user", 42}},
        {{"model", info.name}, {"input", "one"},
         {"encoding_format", "hex"}},
        {{"model", info.name}, {"input", nlohmann::json::array()}},
        {{"model", info.name},
         {"input", nlohmann::json::array({"one", 2})}},
        {{"model", info.name},
         {"input", nlohmann::json::array({1, "two"})}},
        {{"model", info.name},
         {"input", nlohmann::json::array({
             nlohmann::json::array({1}), 2})}},
        {{"model", info.name},
         {"input", nlohmann::json::array({-1, 2})}},
        {{"model", info.name},
         {"input", nlohmann::json::array({1.5, 2})}},
        {{"model", info.name},
         {"input", nlohmann::json::array({
             nlohmann::json::array(), nlohmann::json::array({1})})}},
    };
    for (const auto& body : invalid) {
        httplib::Request request;
        request.body = body.dump();
        httplib::Response response;
        handle_embeddings(request, response, server.make_deps());
        INFO(body.dump());
        CHECK(response.status == 400);
    }
    CHECK(server.metrics.total_requests() == 0);
    CHECK(server.coordinator.active_request_count() == 0);
}

TEST_CASE("Routes: embeddings validate input before model resolution",
          "[routes][embeddings][validation]") {
    TestServer server;
    httplib::Request request;
    request.body = nlohmann::json{
        {"model", "missing-model"},
        {"input", nlohmann::json::array({1, "mixed"})},
    }.dump();
    httplib::Response response;
    handle_embeddings(request, response, server.make_deps());
    REQUIRE(response.status == 400);
    REQUIRE(nlohmann::json::parse(response.body)["error"]["code"] ==
            "invalid_input");
    CHECK(server.metrics.total_requests() == 0);
    CHECK(server.coordinator.active_request_count() == 0);
}

TEST_CASE("Routes: POST /v1/embeddings rejects non-embedding model", "[routes][embeddings]") {
    TestServer ts;
    ts.registry.register_model(make_info("chat-model"));
    REQUIRE(ts.start());

    httplib::Client client("127.0.0.1", ts.port);
    auto response = client.Post("/v1/embeddings", nlohmann::json{
        {"model", "chat-model"}, {"input", "one"},
    }.dump(), "application/json");
    REQUIRE(response);
    REQUIRE(response->status == 400);
    REQUIRE(nlohmann::json::parse(response->body)["error"]["code"] == "unsupported_capability");
    ts.stop();
}

TEST_CASE("Routes: POST /v1/responses translates string input and output", "[routes][responses]") {
    TestServer ts;
    ts.registry.register_model(make_info("chat-model"));
    REQUIRE(ts.coordinator.load("chat-model"));
    REQUIRE(ts.start());

    httplib::Client client("127.0.0.1", ts.port);
    auto response = client.Post("/v1/responses", nlohmann::json{
        {"model", "chat-model"}, {"instructions", "Be concise"}, {"input", "Hello"},
    }.dump(), "application/json");
    REQUIRE(response);
    REQUIRE(response->status == 200);
    const auto body = nlohmann::json::parse(response->body);
    check_schema(body, "responses_completed");
    REQUIRE(body["object"] == "response");
    REQUIRE(body["status"] == "completed");
    REQUIRE(body["model"] == "chat-model");
    REQUIRE(body["created_at"].is_number_integer());
    CHECK(body["background"] == false);
    CHECK(body["conversation"].is_null());
    CHECK(body["previous_response_id"].is_null());
    CHECK(body["service_tier"] == "default");
    CHECK(body["store"] == false);
    CHECK(body["truncation"] == "disabled");
    REQUIRE(body["output"].size() == 1);
    REQUIRE(body["output"][0]["type"] == "message");
    check_schema(body["output"][0], "response_message");
    check_schema(body["output"][0]["content"][0], "response_output_text");
    check_schema(body["usage"], "response_usage");
    REQUIRE(body["output"][0]["content"][0]["type"] == "output_text");
    REQUIRE(body["output"][0]["content"][0]["text"] == "Hello from model");
    REQUIRE(body["output_text"] == "Hello from model");
    REQUIRE(body["completed_at"].is_number_integer());
    CHECK(body["completed_at"].get<std::int64_t>() >=
          body["created_at"].get<std::int64_t>());
    REQUIRE(body["usage"]["input_tokens"] == 3);
    REQUIRE(body["usage"]["output_tokens"] == 4);
    ts.stop();
}

TEST_CASE("Routes: reasoning effort precedence and defaults reach inference",
          "[routes][chat][reasoning]") {
    TestServer ts;
    ts.compatibility_profile = CompatibilityProfile::OpenAIDerivative;
    auto info = make_info("reasoning-model");
    info.reasoning.supported = true;
    info.reasoning.efforts = {"low", "medium", "xhigh"};
    info.reasoning.default_effort = "xhigh";
    info.reasoning.none_disables = true;
    info.reasoning.aliases = {{"high", "xhigh"}};
    ts.registry.register_model(info);
    REQUIRE(ts.coordinator.load(info.name));
    REQUIRE(ts.start());

    httplib::Client client("127.0.0.1", ts.port);
    auto response = client.Post("/v1/chat/completions", nlohmann::json{
        {"model", info.name},
        {"messages", nlohmann::json::array({{{"role", "user"}, {"content", "Hello"}}})},
        {"reasoning_effort", "medium"},
        {"chat_template_kwargs", {{"reasoning_effort", "low"}}},
    }.dump(), "application/json");
    REQUIRE(response);
    REQUIRE(response->status == 200);
    auto* model = dynamic_cast<const IModelMock*>(ts.coordinator.get_model(info.name));
    REQUIRE(model != nullptr);
    CHECK(model->last_request.reasoning_effort == "low");

    response = client.Post("/v1/chat/completions", nlohmann::json{
        {"model", info.name},
        {"messages", nlohmann::json::array({{{"role", "user"}, {"content", "Hello"}}})},
    }.dump(), "application/json");
    REQUIRE(response);
    REQUIRE(response->status == 200);
    CHECK(model->last_request.reasoning_effort == "xhigh");
    ts.stop();
}

TEST_CASE("Routes: reasoning effort validation occurs before acquisition",
          "[routes][chat][reasoning][validation]") {
    TestServer ts;
    auto info = make_info("reasoning-model");
    info.reasoning.supported = true;
    info.reasoning.efforts = {"low", "medium", "xhigh"};
    info.reasoning.default_effort = "xhigh";
    ts.registry.register_model(info);
    ts.registry.register_model(make_info("plain-model"));

    for (const auto& body : std::vector<nlohmann::json>{
             {{"model", info.name}, {"messages", nlohmann::json::array()},
              {"reasoning_effort", "maximum"}},
             {{"model", info.name}, {"messages", nlohmann::json::array()},
              {"reasoning_effort", "none"}},
             {{"model", "plain-model"}, {"messages", nlohmann::json::array()},
              {"reasoning_effort", "medium"}},
         }) {
        httplib::Request request;
        request.body = body.dump();
        httplib::Response response;
        handle_chat_completions(request, response, ts.make_deps());
        CHECK(response.status == 400);
    }
    CHECK(ts.metrics.total_requests() == 0);
}

TEST_CASE("Routes: Responses reasoning effort translates for streaming and non-streaming",
          "[routes][responses][reasoning]") {
    TestServer ts;
    auto info = make_info("reasoning-model");
    info.reasoning.supported = true;
    info.reasoning.efforts = {"low", "medium", "xhigh"};
    info.reasoning.default_effort = "xhigh";
    info.reasoning.none_disables = true;
    ts.registry.register_model(info);
    REQUIRE(ts.coordinator.load(info.name));
    REQUIRE(ts.start());

    httplib::Client client("127.0.0.1", ts.port);
    auto response = client.Post("/v1/responses", nlohmann::json{
        {"model", info.name}, {"input", "Hello"},
        {"reasoning", {{"effort", "medium"}}},
    }.dump(), "application/json");
    REQUIRE(response);
    REQUIRE(response->status == 200);
    auto* model = dynamic_cast<const IModelMock*>(ts.coordinator.get_model(info.name));
    REQUIRE(model != nullptr);
    CHECK(model->last_request.reasoning_effort == "medium");

    response = client.Post("/v1/responses", nlohmann::json{
        {"model", info.name}, {"input", "Hello"}, {"stream", true},
        {"reasoning", {{"effort", "none"}}},
    }.dump(), "application/json");
    REQUIRE(response);
    REQUIRE(response->status == 200);
    CHECK(model->last_request.reasoning_effort == "none");
    ts.stop();
}

TEST_CASE("Routes: POST /v1/responses translates function tools and reasoning", "[routes][responses]") {
    TestServer ts;
    ts.registry.register_model(make_info("chat-model"));
    REQUIRE(ts.coordinator.load("chat-model"));
    REQUIRE(ts.start());

    httplib::Client client("127.0.0.1", ts.port);
    auto response = client.Post("/v1/responses", nlohmann::json{
        {"model", "chat-model"}, {"input", "List files"},
        {"tools", nlohmann::json::array({{{"type", "function"},
            {"name", "list_workspace"}, {"parameters", nlohmann::json::object()}}})},
    }.dump(), "application/json");
    REQUIRE(response);
    REQUIRE(response->status == 200);
    const auto output = nlohmann::json::parse(response->body)["output"];
    REQUIRE(output.size() == 2);
    REQUIRE(output[0]["type"] == "reasoning");
    REQUIRE(output[0]["content"][0]["text"] == "need a tool");
    REQUIRE(output[1]["type"] == "function_call");
    REQUIRE(output[1]["call_id"] == "call_test");
    REQUIRE(output[1]["name"] == "list_workspace");
    REQUIRE(output[1]["arguments"] == "{\"path\":\".\"}");
    ts.stop();
}

TEST_CASE("Routes: POST /v1/responses maps structured output format", "[routes][responses]") {
    TestServer ts;
    ts.registry.register_model(make_info("chat-model"));
    REQUIRE(ts.coordinator.load("chat-model"));
    REQUIRE(ts.start());

    httplib::Client client("127.0.0.1", ts.port);
    auto response = client.Post("/v1/responses", nlohmann::json{
        {"model", "chat-model"}, {"input", "Return JSON"},
        {"text", {{"format", {{"type", "json_schema"}, {"name", "answer"},
            {"schema", {{"type", "object"}}}, {"strict", true}}}}},
    }.dump(), "application/json");
    REQUIRE(response);
    REQUIRE(response->status == 200);
    auto* model = dynamic_cast<const IModelMock*>(ts.coordinator.get_model("chat-model"));
    REQUIRE(model != nullptr);
    REQUIRE(model->last_request.output.kind ==
            inference::StructuredOutputKind::JsonSchema);
    REQUIRE(model->last_request.output.name == "answer");
    ts.stop();
}

TEST_CASE("Routes: POST /v1/responses recognizes unavailable stateful fields",
          "[routes][responses]") {
    TestServer ts;
    ts.registry.register_model(make_info("chat-model"));
    REQUIRE(ts.start());

    httplib::Client client("127.0.0.1", ts.port);
    auto response = client.Post("/v1/responses", nlohmann::json{
        {"model", "chat-model"}, {"input", "Hello"}, {"store", true},
    }.dump(), "application/json");
    REQUIRE(response);
    REQUIRE(response->status == 400);
    const auto error = nlohmann::json::parse(response->body)["error"];
    REQUIRE(error["code"] == "unsupported_capability");
    REQUIRE(error["param"] == "store");
    ts.stop();
}

TEST_CASE("Routes: Responses top_logprobs reach inference and exact output",
          "[routes][responses][logprobs]") {
    TestServer ts;
    ts.registry.register_model(make_info("chat-model"));
    REQUIRE(ts.coordinator.load("chat-model"));
    REQUIRE(ts.start());

    httplib::Client client("127.0.0.1", ts.port);
    auto response = client.Post("/v1/responses", nlohmann::json{
        {"model", "chat-model"}, {"input", "Hello"},
        {"top_logprobs", 1},
        {"include", nlohmann::json::array({"message.output_text.logprobs"})},
    }.dump(), "application/json");
    REQUIRE(response);
    REQUIRE(response->status == 200);
    const auto body = nlohmann::json::parse(response->body);
    const auto& values = body["output"][0]["content"][0]["logprobs"];
    REQUIRE(values.size() == 1);
    CHECK(values[0]["token"] == "Hello");
    CHECK(values[0]["logprob"] == Catch::Approx(-0.125));
    CHECK(values[0]["bytes"] == nlohmann::json::array({72, 101, 108, 108, 111}));
    REQUIRE(values[0]["top_logprobs"].size() == 1);
    CHECK(values[0]["top_logprobs"][0]["token"] == "Hi");
    auto* model = dynamic_cast<const IModelMock*>(
        ts.coordinator.get_model("chat-model"));
    REQUIRE(model != nullptr);
    CHECK(model->last_request.logprobs);
    CHECK(model->last_request.top_logprobs == 1);
    ts.stop();
}

TEST_CASE("Routes: Responses include alone requests output logprobs",
          "[routes][responses][logprobs]") {
    TestServer ts;
    ts.registry.register_model(make_info("chat-model"));
    REQUIRE(ts.coordinator.load("chat-model"));
    REQUIRE(ts.start());

    httplib::Client client("127.0.0.1", ts.port);
    auto response = client.Post("/v1/responses", nlohmann::json{
        {"model", "chat-model"}, {"input", "Hello"},
        {"include", nlohmann::json::array({"message.output_text.logprobs"})},
    }.dump(), "application/json");
    REQUIRE(response);
    REQUIRE(response->status == 200);
    const auto body = nlohmann::json::parse(response->body);
    REQUIRE(body["output"][0]["content"][0]["logprobs"].size() == 1);
    auto* model = dynamic_cast<const IModelMock*>(
        ts.coordinator.get_model("chat-model"));
    REQUIRE(model != nullptr);
    CHECK(model->last_request.logprobs);
    CHECK(model->last_request.top_logprobs == 0);
    ts.stop();
}

TEST_CASE("Responses adapter accepts pinned nested history fields",
          "[routes][responses][adapter]") {
    const auto parsed = parse_openai_responses_request(nlohmann::json{
        {"model", "test-model"},
        {"input", nlohmann::json::array({
            {{"type", "message"}, {"role", "assistant"},
             {"phase", "final_answer"}, {"status", "completed"},
             {"content", nlohmann::json::array({
                 {{"type", "output_text"}, {"text", "prior"},
                  {"annotations", nlohmann::json::array()},
                  {"logprobs", nlohmann::json::array()}},
             })}},
        })},
        {"stream", nullptr}, {"stream_options", nullptr},
        {"max_output_tokens", nullptr}, {"temperature", nullptr},
        {"top_p", nullptr}, {"parallel_tool_calls", nullptr},
        {"metadata", nullptr}, {"reasoning", nullptr}, {"text", nullptr},
    }, false);
    REQUIRE(parsed);
    REQUIRE(parsed->generation.messages.size() == 1);
    const auto* text = std::get_if<inference::TextContent>(
        &parsed->generation.messages[0].content[0]);
    REQUIRE(text != nullptr);
    CHECK(text->text == "prior");
}

TEST_CASE("Routes: POST /v1/responses validates contract before acquisition",
          "[routes][responses][validation]") {
    TestServer ts;
    ts.registry.register_model(make_info("chat-model"));

    const std::vector<nlohmann::json> invalid = {
        {{"model", "chat-model"}, {"input", "Hello"}, {"stream", "yes"}},
        {{"model", "chat-model"}, {"input", "Hello"}, {"max_output_tokens", 0}},
        {{"model", "chat-model"}, {"input", "Hello"}, {"temperature", 2.1}},
        {{"model", "chat-model"}, {"input", "Hello"}, {"top_p", -0.1}},
        {{"model", "chat-model"}, {"input", "Hello"}, {"priority", 101}},
        {{"model", "chat-model"}, {"input", "Hello"},
         {"parallel_tool_calls", "yes"}},
        {{"model", "chat-model"}, {"input", "Hello"},
         {"metadata", nlohmann::json::array()}},
        {{"model", "chat-model"},
         {"input", nlohmann::json::array({
             {{"type", "message"}, {"role", "invalid"}, {"content", "Hello"}},
         })}},
        {{"model", "chat-model"},
         {"input", nlohmann::json::array({
             {{"type", "message"}, {"role", "user"},
              {"content", nlohmann::json::array({
                  {{"type", "input_text"}, {"text", 42}},
              })}},
         })}},
        {{"model", "chat-model"},
         {"input", nlohmann::json::array({
             {{"type", "function_call"}, {"call_id", "c1"}, {"name", "run"},
              {"arguments", nlohmann::json::object()}},
         })}},
        {{"model", "chat-model"},
         {"input", nlohmann::json::array({
             {{"type", "function_call_output"}, {"call_id", "c1"},
              {"output", nlohmann::json::object()}},
         })}},
        {{"model", "chat-model"}, {"input", "Hello"},
         {"tools", nlohmann::json::array({
             {{"type", "function"}, {"name", "run"},
              {"parameters", nlohmann::json::array()}},
         })}},
        {{"model", "chat-model"}, {"input", "Hello"},
         {"tool_choice", "sometimes"}},
        {{"model", "chat-model"}, {"input", "Hello"},
         {"text", {{"format", {{"type", "json_schema"},
                                {"schema", nlohmann::json::object()}}}}}},
    };

    for (const auto& body : invalid) {
        httplib::Request request;
        request.body = body.dump();
        httplib::Response response;
        handle_responses(request, response, ts.make_deps());
        CHECK(response.status == 400);
    }
    CHECK(ts.metrics.total_requests() == 0);
}

TEST_CASE("Routes: POST /v1/responses streams typed reasoning and tool events", "[routes][responses]") {
    TestServer ts;
    ts.registry.register_model(make_info("chat-model"));
    REQUIRE(ts.coordinator.load("chat-model"));
    REQUIRE(ts.start());

    httplib::Client client("127.0.0.1", ts.port);
    client.set_read_timeout(std::chrono::seconds(5));
    auto response = client.Post("/v1/responses", nlohmann::json{
        {"model", "chat-model"}, {"input", "List files"}, {"stream", true},
        {"tools", nlohmann::json::array({{{"type", "function"},
            {"name", "list_workspace"}, {"parameters", nlohmann::json::object()}}})},
    }.dump(), "application/json");
    REQUIRE(response);
    REQUIRE(response->status == 200);
    REQUIRE(response->get_header_value_count("Content-Type") == 1);
    CHECK(response->get_header_value("Content-Type") == "text/event-stream");
    REQUIRE(response->body.find("event: response.created") != std::string::npos);
    REQUIRE(response->body.find("event: response.reasoning_text.delta") != std::string::npos);
    REQUIRE(response->body.find("event: response.function_call_arguments.delta") != std::string::npos);
    REQUIRE(response->body.find("event: response.function_call_arguments.done") != std::string::npos);
    REQUIRE(response->body.find("event: response.completed") != std::string::npos);
    REQUIRE(response->body.find("data: [DONE]") == std::string::npos);
    const auto fixture_path = std::filesystem::path(INFERDECK_SOURCE_DIR) /
        "tests/fixtures/openai_adapter_golden.json";
    std::ifstream fixture_input(fixture_path, std::ios::binary);
    REQUIRE(fixture_input.good());
    const auto fixture = nlohmann::json::parse(fixture_input);
    nlohmann::json event_types = nlohmann::json::array();
    std::size_t offset = 0;
    while ((offset = response->body.find("event: ", offset)) != std::string::npos) {
        const auto end = response->body.find('\n', offset);
        REQUIRE(end != std::string::npos);
        event_types.push_back(response->body.substr(offset + 7, end - offset - 7));
        offset = end + 1;
    }
    CHECK(event_types == fixture["responses_event_types"]);
    std::optional<std::int64_t> created_at;
    offset = 0;
    while ((offset = response->body.find("data: ", offset)) !=
           std::string::npos) {
        const auto end = response->body.find('\n', offset);
        REQUIRE(end != std::string::npos);
        const auto data = response->body.substr(
            offset + 6, end - offset - 6);
        offset = end + 1;
        const auto event = nlohmann::json::parse(data);
        if (!event.contains("response")) continue;
        check_schema(event["response"],
                     event["type"] == "response.completed"
                         ? "responses_completed"
                         : "responses_in_progress");
        CHECK(event["response"]["model"] == "chat-model");
        const auto event_created =
            event["response"]["created_at"].get<std::int64_t>();
        if (!created_at) created_at = event_created;
        CHECK(event_created == *created_at);
        CHECK(event["response"]["store"] == false);
        CHECK(event["response"]["truncation"] == "disabled");
        if (event["type"] == "response.completed") {
            CHECK(event["response"]["output_text"].is_string());
            CHECK(event["response"]["completed_at"].is_number_integer());
        } else {
            CHECK_FALSE(event["response"].contains("completed_at"));
        }
    }
    REQUIRE(created_at.has_value());
    ts.stop();
}

TEST_CASE("Routes: POST /v1/images/generations returns base64 images", "[routes][images]") {
    TestServer ts;
    auto info = make_info("image-model");
    info.runtime = "stable_diffusion_cpp";
    info.modality = "image";
    info.capabilities = {"image_generation"};
    ts.registry.register_model(info);
    REQUIRE(ts.coordinator.load("image-model"));
    REQUIRE(ts.start());
    httplib::Client client("127.0.0.1", ts.port);
    auto response = client.Post("/v1/images/generations",
        nlohmann::json{{"model", "image-model"}, {"prompt", "a lighthouse"},
                       {"size", "512x512"}, {"n", 2}}.dump(),
        "application/json");
    REQUIRE(response);
    REQUIRE(response->status == 200);
    CHECK_FALSE(response->has_header("X-InferDeck-Job-Id"));
    const auto body = nlohmann::json::parse(response->body);
    check_schema(body, "images");
    REQUIRE(body["data"].size() == 2);
    CHECK(body["output_format"] == "png");
    CHECK(body["data"][0]["b64_json"] == "iVBORw==");
    check_schema(body["data"][0], "image");
    const auto rows = ts.stats_db.recent_requests(1);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].endpoint == "/v1/images/generations");
    CHECK(rows[0].protocol_profile == "strict_openai");
    CHECK(rows[0].modality == "image_generation");
    CHECK(rows[0].output_image_count == 2);
    CHECK_FALSE(rows[0].request_id.empty());
    ts.stop();
}

TEST_CASE("Strict Images separates unknown fields from model capabilities",
          "[routes][images][profile]") {
    TestServer ts;
    auto info = make_info("image-profile-model");
    info.runtime = "stable_diffusion_cpp";
    info.modality = "image";
    info.capabilities = {"image_generation"};
    ts.registry.register_model(info);
    REQUIRE(ts.coordinator.load("image-profile-model"));
    auto strict_deps = ts.make_deps();
    const std::vector<std::pair<std::string, nlohmann::json>> extensions{
        {"negative_prompt", "fog"},
        {"seed", 42},
        {"steps", 10},
        {"guidance_scale", 5.0},
        {"future_extension", true},
        {"background", "transparent"},
        {"moderation", "low"},
        {"output_compression", 90},
        {"output_format", "jpeg"},
        {"partial_images", 1},
        {"quality", "high"},
        {"response_format", "url"},
        {"stream", true},
        {"style", "natural"},
    };
    for (const auto& [field, value] : extensions) {
        httplib::Request request;
        request.is_connection_closed = [] { return false; };
        nlohmann::json body{{"model", "image-profile-model"},
                            {"prompt", "a lighthouse"},
                            {"size", "512x512"}};
        body[field] = value;
        request.body = body.dump();
        httplib::Response response;
        handle_image_generations(request, response, strict_deps);
        REQUIRE(response.status == 400);
        const bool standard_field = field == "background" ||
            field == "moderation" || field == "output_compression" ||
            field == "output_format" || field == "partial_images" ||
            field == "quality" || field == "response_format" ||
            field == "stream" || field == "style";
        CHECK(nlohmann::json::parse(response.body)["error"]["code"] ==
              (standard_field ? "unsupported_capability"
                              : "unsupported_parameter"));
        CHECK(ts.coordinator.active_request_count() == 0);
    }

    httplib::Request compatible_request;
    compatible_request.is_connection_closed = [] { return false; };
    compatible_request.body = nlohmann::json{
        {"model", "image-profile-model"},
        {"prompt", "a lighthouse"},
        {"size", "512x512"},
        {"background", "auto"},
        {"moderation", "auto"},
        {"output_format", "png"},
        {"partial_images", 0},
        {"quality", "auto"},
        {"response_format", "b64_json"},
        {"stream", false},
        {"user", "local-user"},
    }.dump();
    httplib::Response compatible_response;
    handle_image_generations(compatible_request, compatible_response,
                             strict_deps);
    REQUIRE(compatible_response.status == 200);
    CHECK(nlohmann::json::parse(compatible_response.body)["output_format"] ==
          "png");
    CHECK(ts.coordinator.active_request_count() == 0);

    const std::vector<nlohmann::json> malformed{
        nlohmann::json::array(),
        {{"model", "missing"}, {"prompt", 42}},
        {{"model", "missing"}, {"prompt", "valid"}, {"n", "two"}},
        {{"model", "missing"}, {"prompt", "valid"}, {"size", 512}},
        {{"model", "missing"}, {"prompt", "valid"}, {"stream", "false"}},
        {{"model", "missing"}, {"prompt", "valid"}, {"user", 42}},
    };
    for (const auto& body : malformed) {
        httplib::Request invalid_request;
        invalid_request.body = body.dump();
        httplib::Response invalid_response;
        handle_image_generations(invalid_request, invalid_response,
                                 strict_deps);
        INFO(body.dump());
        CHECK(invalid_response.status == 400);
        CHECK(ts.coordinator.active_request_count() == 0);
    }

    auto derivative_deps = ts.make_deps();
    derivative_deps.compatibility_profile =
        CompatibilityProfile::OpenAIDerivative;
    httplib::Request request;
    request.is_connection_closed = [] { return false; };
    request.body = nlohmann::json{
        {"model", "image-profile-model"},
        {"prompt", "a lighthouse"},
        {"size", "512x512"},
        {"negative_prompt", "fog"},
        {"seed", 42},
        {"steps", 10},
        {"guidance_scale", 5.0},
    }.dump();
    httplib::Response response;
    handle_image_generations(request, response, derivative_deps);
    REQUIRE(response.status == 200);
    CHECK(response.has_header("X-InferDeck-Job-Id"));
    CHECK(ts.coordinator.active_request_count() == 0);
}

TEST_CASE("Image jobs can be cancelled through the shared tracker", "[routes][images][cancel]") {
    TestServer ts;
    auto info = make_info("cancel-image");
    info.runtime = "stable_diffusion_cpp";
    info.modality = "image";
    info.capabilities = {"image_generation"};
    ts.registry.register_model(info);
    REQUIRE(ts.coordinator.load("cancel-image"));
    const auto* backend = dynamic_cast<const IModelMock*>(ts.coordinator.get_backend("cancel-image"));
    REQUIRE(backend);
    const_cast<IModelMock*>(backend)->block_media_until_cancel.store(true);
    REQUIRE(ts.start());
    std::atomic<int> status{0};
    std::thread request([&] {
        httplib::Client client("127.0.0.1", ts.port);
        auto response = client.Post("/v1/images/generations",
            nlohmann::json{{"model", "cancel-image"}, {"prompt", "cancel me"},
                           {"size", "512x512"}}.dump(), "application/json");
        status.store(response ? response->status : -1);
    });
    std::uint64_t job_id = 0;
    for (int attempt = 0; attempt < 100 && job_id == 0; ++attempt) {
        for (const auto& job : media_jobs()) {
            if (job["model"] == "cancel-image" && job["state"] == "running") job_id = job["id"];
        }
        if (job_id == 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(job_id > 0);
    REQUIRE(cancel_media_job(job_id));
    request.join();
    CHECK(status.load() == 408);
    const auto rows = ts.stats_db.recent_requests(1);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].status_code == 499);
    ts.stop();
}

TEST_CASE("Routes: POST /v1/audio/speech returns runtime audio", "[routes][speech]") {
    TestServer ts;
    auto info = make_info("speech-model");
    info.runtime = "sherpa_onnx";
    info.modality = "audio_speech";
    info.capabilities = {"audio_speech"};
    ts.registry.register_model(info);
    REQUIRE(ts.coordinator.load("speech-model"));
    REQUIRE(ts.start());
    httplib::Client client("127.0.0.1", ts.port);
    auto response = client.Post("/v1/audio/speech",
        nlohmann::json{{"model", "speech-model"}, {"input", "hello"},
                       {"voice", "default"}, {"response_format", "wav"},
                       {"speed", 1.0}}.dump(),
        "application/json");
    REQUIRE(response);
    CHECK(response->status == 200);
    CHECK(response->get_header_value("Content-Type") == "audio/wav");
    CHECK(response->body == "RIFF");

    auto* backend = const_cast<IModelMock*>(
        dynamic_cast<const IModelMock*>(
            ts.coordinator.get_backend("speech-model")));
    REQUIRE(backend);
    backend->speech_should_fail.store(true);
    response = client.Post("/v1/audio/speech",
        nlohmann::json{{"model", "speech-model"}, {"input", "hello"},
                       {"voice", "default"}, {"response_format", "wav"},
                       {"speed", 1.0}}.dump(),
        "application/json");
    REQUIRE(response);
    CHECK(response->status == 400);
    CHECK(nlohmann::json::parse(response->body)["error"]["code"] ==
          "speech_generation_failed");
    backend->speech_should_fail.store(false);

    response = client.Post("/v1/audio/speech",
        nlohmann::json{{"model", "speech-model"}, {"input", "hello"},
                       {"voice", "default"}, {"response_format", "mp3"},
                       {"speed", 1.0}}.dump(), "application/json");
    REQUIRE(response);
    CHECK(response->status == 400);
    CHECK(nlohmann::json::parse(response->body)["error"]["code"] ==
          "unsupported_capability");
    const auto usage = ts.stats_db.model_usage();
    REQUIRE(usage.size() == 1);
    CHECK(usage[0].model == "speech-model");
    CHECK(usage[0].requests == 2);
    CHECK(usage[0].successful_requests == 1);
    CHECK(usage[0].input_characters == 5);
    CHECK(usage[0].input_audio_seconds == 0.0);
    const auto rows = ts.stats_db.recent_requests(10);
    const auto successful = std::find_if(rows.begin(), rows.end(),
        [](const auto& row) { return row.status_code == 200; });
    REQUIRE(successful != rows.end());
    CHECK(successful->endpoint == "/v1/audio/speech");
    CHECK(successful->modality == "audio_speech");
    CHECK(successful->input_characters == 5);
    CHECK(successful->output_audio_seconds == Catch::Approx(1.25));
    CHECK_FALSE(successful->request_id.empty());
    ts.stop();
}

TEST_CASE("Routes: speech validates the OpenAI request contract before admission",
          "[routes][speech][validation]") {
    TestServer ts;
    auto info = make_info("speech-validation");
    info.runtime = "sherpa_onnx";
    info.modality = "audio_speech";
    info.capabilities = {"audio_speech"};
    auto chat_info = make_info("not-speech");
    auto cold_info = make_info("cold-speech-validation");
    cold_info.runtime = "sherpa_onnx";
    cold_info.capabilities = {"audio_speech"};
    ts.registry.register_model(info);
    ts.registry.register_model(chat_info);
    ts.registry.register_model(cold_info);
    REQUIRE(ts.coordinator.load(info.name));
    REQUIRE(ts.start());
    httplib::Client client("127.0.0.1", ts.port);
    const auto initial_jobs = media_jobs().size();

    const auto post = [&client](nlohmann::json body) {
        return client.Post("/v1/audio/speech", body.dump(), "application/json");
    };
    const auto expect_error = [&post](nlohmann::json body,
                                      const std::string& code) {
        auto response = post(std::move(body));
        REQUIRE(response);
        REQUIRE(response->status == 400);
        CHECK(nlohmann::json::parse(response->body)["error"]["code"] == code);
    };

    expect_error({{"input", "hello"}, {"voice", "default"}},
                 "invalid_speech_request");
    expect_error({{"model", info.name}, {"voice", "default"}},
                 "invalid_speech_request");
    expect_error({{"model", info.name}, {"input", "hello"}},
                 "invalid_speech_request");
    expect_error({{"model", info.name}, {"input", 7}, {"voice", "default"}},
                 "invalid_speech_request");
    expect_error({{"model", info.name}, {"input", "hello"}, {"voice", 7}},
                 "invalid_speech_request");
    expect_error({{"model", info.name}, {"input", "hello"}, {"voice", "default"},
                  {"speed", "fast"}}, "invalid_speech_request");
    expect_error({{"model", info.name}, {"input", "hello"}, {"voice", "default"},
                  {"speed", 4.01}}, "invalid_speech_request");
    expect_error({{"model", info.name}, {"input", "hello"}, {"voice", "default"},
                  {"instructions", 7}}, "invalid_speech_request");
    expect_error({{"model", info.name}, {"input", "hello"}, {"voice", "default"},
                  {"instructions", "Speak clearly"}}, "unsupported_capability");
    expect_error({{"model", info.name}, {"input", "hello"}, {"voice", "default"},
                  {"response_format", 7}}, "invalid_speech_request");
    expect_error({{"model", info.name}, {"input", "hello"}, {"voice", "default"},
                  {"response_format", "ogg"}}, "unsupported_response_format");
    expect_error({{"model", info.name}, {"input", "hello"}, {"voice", "default"},
                  {"stream_format", 7}}, "invalid_speech_request");
    expect_error({{"model", info.name}, {"input", "hello"}, {"voice", "default"},
                  {"stream_format", "events"}}, "unsupported_stream_format");
    expect_error({{"model", info.name}, {"input", "hello"}, {"voice", "default"},
                   {"stream_format", "sse"}}, "unsupported_capability");
    expect_error({{"model", info.name}, {"input", "hello"}, {"voice", "default"},
                  {"response_format", "wav"}, {"priority", 1}},
                 "unsupported_parameter");
    expect_error({{"model", info.name}, {"input", "hello"},
                  {"voice", {{"id", "voice_custom"}, {"ignored", true}}},
                  {"response_format", "wav"}}, "invalid_speech_request");
    expect_error({{"model", info.name}, {"input", "hello"}, {"voice", "default"}},
                 "unsupported_capability");
    expect_error({{"model", info.name}, {"input", "hello"},
                  {"voice", "not-a-voice"}, {"response_format", "wav"}},
                 "invalid_speech_request");
    expect_error({{"model", info.name}, {"input", "hello"},
                  {"voice", {{"id", "not-a-voice"}}},
                  {"response_format", "wav"}}, "invalid_speech_request");
    expect_error({{"model", cold_info.name}, {"input", "hello"},
                  {"voice", "not-a-voice"}, {"response_format", "wav"}},
                 "invalid_speech_request");
    expect_error({{"model", cold_info.name}, {"input", "hello"},
                  {"voice", "999"}, {"response_format", "wav"}},
                 "invalid_speech_request");
    CHECK_FALSE(ts.coordinator.is_loaded(cold_info.name));
    CHECK(ts.coordinator.active_request_count() == 0);
    CHECK(media_jobs().size() == initial_jobs);

    expect_error({{"model", chat_info.name}, {"input", "hello"},
                  {"voice", "default"}, {"response_format", "wav"}},
                 "unsupported_capability");
    CHECK(ts.coordinator.active_request_count() == 0);
    CHECK(media_jobs().size() == initial_jobs);

    const std::string emoji = "\xf0\x9f\x92\xa1";
    std::string maximum_input;
    for (int index = 0; index < 4096; ++index) maximum_input += emoji;
    auto accepted = post({{"model", info.name}, {"input", maximum_input},
                          {"voice", {{"id", "voice_custom"}}},
                          {"instructions", ""}, {"response_format", "wav"},
                          {"stream_format", "audio"}, {"speed", 0.25}});
    REQUIRE(accepted);
    REQUIRE(accepted->status == 200);

    maximum_input += emoji;
    expect_error({{"model", info.name}, {"input", maximum_input},
                  {"voice", "default"}, {"response_format", "wav"}},
                 "invalid_speech_request");
    CHECK(ts.coordinator.active_request_count() == 0);
    ts.stop();
}

TEST_CASE("Routes: POST /v1/audio/transcriptions accepts request-scoped WAV", "[routes][transcriptions]") {
    TestServer ts;
    auto info = make_info("whisper-model");
    info.runtime = "whisper_cpp";
    info.modality = "audio_transcription";
    info.capabilities = {"audio_transcription"};
    ts.registry.register_model(info);
    REQUIRE(ts.coordinator.load("whisper-model"));
    REQUIRE(ts.start());
    httplib::Client client("127.0.0.1", ts.port);
    const auto transcribe = [&client](const std::string& format) {
        httplib::UploadFormDataItems items{
            {"file", test_wav(), "test.wav", "audio/wav"},
            {"model", "whisper-model", "", ""},
            {"language", "en", "", ""},
            {"response_format", format, "", ""},
        };
        if (format == "verbose_json") {
            items.push_back(
                {"timestamp_granularities[]", "segment", "", ""});
        }
        return client.Post("/v1/audio/transcriptions", items);
    };
    auto response = transcribe("json");
    REQUIRE(response);
    REQUIRE(response->status == 200);
    CHECK(nlohmann::json::parse(response->body)["text"] == "test transcript");
    check_schema(nlohmann::json::parse(response->body), "transcription");

    response = transcribe("text");
    REQUIRE(response);
    REQUIRE(response->status == 200);
    CHECK(response->body == "test transcript");
    CHECK(response->get_header_value("Content-Type") == "text/plain; charset=utf-8");

    response = transcribe("verbose_json");
    REQUIRE(response);
    REQUIRE(response->status == 200);
    const auto verbose = nlohmann::json::parse(response->body);
    check_schema(verbose, "verbose_transcription");
    CHECK(verbose["task"] == "transcribe");
    CHECK(verbose["language"] == "en");
    REQUIRE(verbose["segments"].size() == 1);
    check_schema(verbose["segments"][0], "transcription_segment");
    CHECK(verbose["segments"][0]["tokens"] == nlohmann::json::array({50364, 1234, 50389}));
    CHECK_FALSE(verbose["segments"][0].contains("compression_ratio"));

    response = transcribe("srt");
    REQUIRE(response);
    REQUIRE(response->status == 200);
    CHECK(response->body == "1\n00:00:00,000 --> 00:00:00,500\ntest transcript\n\n");
    CHECK(response->get_header_value("Content-Type") == "application/x-subrip; charset=utf-8");

    response = transcribe("vtt");
    REQUIRE(response);
    REQUIRE(response->status == 200);
    CHECK(response->body == "WEBVTT\n\n00:00:00.000 --> 00:00:00.500\ntest transcript\n\n");
    CHECK(response->get_header_value("Content-Type") == "text/vtt; charset=utf-8");
    const auto usage = ts.stats_db.model_usage();
    REQUIRE(usage.size() == 1);
    CHECK(usage[0].model == "whisper-model");
    CHECK(usage[0].requests == 5);
    CHECK(usage[0].successful_requests == 5);
    CHECK(usage[0].input_audio_seconds == Catch::Approx(0.05));
    CHECK(usage[0].input_characters == 0);
    const auto rows = ts.stats_db.recent_requests(1);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].endpoint == "/v1/audio/transcriptions");
    CHECK(rows[0].modality == "audio_transcription");
    CHECK(rows[0].input_audio_seconds == Catch::Approx(0.01));
    CHECK_FALSE(rows[0].request_id.empty());
    ts.stop();
}

TEST_CASE("Audio cancellation separates HTTP and internal outcomes",
          "[routes][audio][cancel]") {
    TestServer ts;
    auto speech_info = make_info("cancel-speech");
    speech_info.runtime = "sherpa_onnx";
    speech_info.capabilities = {"audio_speech"};
    auto transcription_info = make_info("cancel-transcription");
    transcription_info.runtime = "whisper_cpp";
    transcription_info.capabilities = {"audio_transcription"};
    ts.registry.register_model(speech_info);
    ts.registry.register_model(transcription_info);
    REQUIRE(ts.coordinator.load(speech_info.name));
    REQUIRE(ts.coordinator.load(transcription_info.name));
    auto* speech_backend = const_cast<IModelMock*>(dynamic_cast<const IModelMock*>(
        ts.coordinator.get_backend(speech_info.name)));
    auto* transcription_backend = const_cast<IModelMock*>(
        dynamic_cast<const IModelMock*>(
            ts.coordinator.get_backend(transcription_info.name)));
    REQUIRE(speech_backend);
    REQUIRE(transcription_backend);
    speech_backend->speech_should_cancel.store(true);
    transcription_backend->transcription_should_cancel.store(true);
    REQUIRE(ts.start());
    httplib::Client client("127.0.0.1", ts.port);

    const auto speech = client.Post(
        "/v1/audio/speech",
        nlohmann::json{{"model", speech_info.name}, {"input", "cancel"},
                       {"voice", "default"}, {"response_format", "wav"}}.dump(),
        "application/json");
    REQUIRE(speech);
    CHECK(speech->status == 408);
    CHECK(nlohmann::json::parse(speech->body)["error"]["code"] ==
          "speech_generation_failed");

    const auto transcription = client.Post(
        "/v1/audio/transcriptions", httplib::UploadFormDataItems{
            {"file", test_wav(), "test.wav", "audio/wav"},
            {"model", transcription_info.name, "", ""},
        });
    REQUIRE(transcription);
    CHECK(transcription->status == 408);
    CHECK(nlohmann::json::parse(transcription->body)["error"]["code"] ==
          "transcription_failed");
    const auto rows = ts.stats_db.recent_requests(2);
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].status_code == 499);
    CHECK(rows[1].status_code == 499);
    ts.stop();
}

TEST_CASE("Routes: Open WebUI voice reservation spans STT through TTS",
          "[routes][voice][priority]") {
    TestServer ts;
    ts.registry.register_model(make_info("gemma"));
    auto whisper = make_info("whisper-model");
    whisper.runtime = "whisper_cpp";
    whisper.modality = "audio_transcription";
    whisper.capabilities = {"audio_transcription"};
    whisper.vram_required_mb = 0;
    auto speech = make_info("speech-model");
    speech.runtime = "sherpa_onnx";
    speech.modality = "audio_speech";
    speech.capabilities = {"audio_speech"};
    speech.vram_required_mb = 0;
    ts.registry.register_model(whisper);
    ts.registry.register_model(speech);
    REQUIRE(ts.coordinator.load("whisper-model"));
    REQUIRE(ts.coordinator.load("speech-model"));
    auto* whisper_backend = const_cast<IModelMock*>(
        dynamic_cast<const IModelMock*>(
            ts.coordinator.get_backend("whisper-model")));
    REQUIRE(whisper_backend);
    whisper_backend->transcription_delay_ms.store(20);
    ts.voice_default_model = "gemma";
    ts.voice_session_grace_ms = 5;
    REQUIRE(ts.start());

    httplib::Client client("127.0.0.1", ts.port);
    client.set_default_headers({
        {"Authorization", "Bearer voice-principal"},
        {"X-InferDeck-Voice-Session", "voice-session-0001"},
    });
    const std::string session_key =
        std::string{"voice-principal"} + '\x1f' + "voice-session-0001";
    const auto transcription = client.Post(
        "/v1/audio/transcriptions", httplib::UploadFormDataItems{
            {"file", test_wav(), "test.wav", "audio/wav"},
            {"model", "whisper-model", "", ""},
        });
    REQUIRE(transcription);
    REQUIRE(transcription->status == 200);
    CHECK(ts.coordinator.priority_session_matches(session_key, "gemma"));

    const auto synthesized = client.Post(
        "/v1/audio/speech",
        nlohmann::json{{"model", "speech-model"}, {"input", "hello"},
                       {"voice", "default"}, {"response_format", "wav"}}.dump(),
        "application/json");
    REQUIRE(synthesized);
    REQUIRE(synthesized->status == 200);
    CHECK_FALSE(ts.coordinator.priority_session_matches(session_key, "gemma"));
    ts.stop();
}

TEST_CASE("Routes: POST /v1/audio/transcriptions accepts Open WebUI MP3", "[routes][transcriptions]") {
    TestServer ts;
    auto info = make_info("whisper-model");
    info.runtime = "whisper_cpp";
    info.modality = "audio_transcription";
    info.capabilities = {"audio_transcription"};
    ts.registry.register_model(info);
    REQUIRE(ts.coordinator.load("whisper-model"));
    REQUIRE(ts.start());

    const auto path = std::filesystem::path(INFERDECK_SOURCE_DIR) /
        "libs/third_party/llama.cpp/tools/mtmd/test-2.mp3";
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.good());
    const std::string audio(std::istreambuf_iterator<char>{input}, {});
    REQUIRE_FALSE(audio.empty());

    httplib::Client client("127.0.0.1", ts.port);
    const auto response = client.Post(
        "/v1/audio/transcriptions", httplib::UploadFormDataItems{
            {"file", audio, "voice-mode.mp3", "audio/mpeg"},
            {"model", "whisper-model", "", ""},
            {"language", "en", "", ""},
        });
    REQUIRE(response);
    REQUIRE(response->status == 200);
    CHECK(nlohmann::json::parse(response->body)["text"] == "test transcript");
    ts.stop();
}

TEST_CASE("Media routes reject unsupported request shapes", "[routes][media]") {
    TestServer ts;
    auto transcription_info = make_info("transcription-validation");
    transcription_info.runtime = "whisper_cpp";
    transcription_info.modality = "audio_transcription";
    transcription_info.capabilities = {"audio_transcription"};
    transcription_info.vram_required_mb = 0;
    ts.registry.register_model(transcription_info);
    REQUIRE(ts.coordinator.load("transcription-validation"));
    REQUIRE(ts.start());
    httplib::Client client("127.0.0.1", ts.port);
    auto image = client.Post("/v1/images/generations",
        nlohmann::json{{"model", "x"}, {"prompt", "x"}, {"size", "513x512"}}.dump(),
        "application/json");
    REQUIRE(image);
    CHECK(image->status == 400);
    auto speech = client.Post("/v1/audio/speech",
        nlohmann::json{{"model", "x"}, {"input", "x"}, {"voice", "v"}, {"speed", 8}}.dump(),
        "application/json");
    REQUIRE(speech);
    CHECK(speech->status == 400);
    auto transcription = client.Post("/v1/audio/transcriptions", httplib::UploadFormDataItems{
        {"file", test_wav(), "test.wav", "audio/wav"},
        {"model", "", "", ""},
    });
    REQUIRE(transcription);
    CHECK(transcription->status == 400);
    transcription = client.Post("/v1/audio/transcriptions", httplib::UploadFormDataItems{
        {"file", test_wav(), "test.wav", "audio/wav"},
        {"model", "x", "", ""},
        {"response_format", "mp3", "", ""},
    });
    REQUIRE(transcription);
    CHECK(transcription->status == 400);
    transcription = client.Post("/v1/audio/transcriptions", httplib::UploadFormDataItems{
        {"file", test_wav(), "test.wav", "audio/wav"},
        {"model", "transcription-validation", "", ""},
        {"temperature", "nan", "", ""},
    });
    REQUIRE(transcription);
    CHECK(transcription->status == 400);
    CHECK(nlohmann::json::parse(transcription->body)["error"]["code"] ==
          "invalid_transcription_request");
    const auto unsupported_transcription = [&client](
        const std::string& name, const std::string& value,
        const std::string& format = "json") {
        return client.Post("/v1/audio/transcriptions", httplib::UploadFormDataItems{
            {"file", test_wav(), "test.wav", "audio/wav"},
            {"model", "transcription-validation", "", ""},
            {"response_format", format, "", ""},
            {name, value, "", ""},
        });
    };
    for (const auto& [name, value, format] :
         std::vector<std::tuple<std::string, std::string, std::string>>{
             {"stream", "true", "json"},
             {"include[]", "logprobs", "json"},
             {"chunking_strategy", "auto", "json"},
             {"keywords[]", "InferDeck", "json"},
             {"timestamp_granularities[]", "word", "verbose_json"},
             {"timestamp_granularities[]", "segment", "json"},
             {"future_field", "ignored", "json"},
         }) {
        auto invalid = unsupported_transcription(name, value, format);
        INFO(name);
        REQUIRE(invalid);
        CHECK(invalid->status == 400);
        CHECK(ts.coordinator.active_request_count() == 0);
    }
    auto harmless_stream = unsupported_transcription("stream", "false");
    REQUIRE(harmless_stream);
    CHECK(harmless_stream->status == 200);
    transcription = client.Post("/v1/audio/transcriptions", httplib::UploadFormDataItems{
        {"file", test_float_wav(std::numeric_limits<float>::quiet_NaN()),
         "test.wav", "audio/wav"},
        {"model", "transcription-validation", "", ""},
    });
    REQUIRE(transcription);
    CHECK(transcription->status == 400);
    CHECK(nlohmann::json::parse(transcription->body)["error"]["message"] ==
          "float32 WAVE samples must be finite");
    ts.stop();
}

TEST_CASE("Routes: streaming tool call emits llama-server shaped SSE",
          "[routes][chat][stream][tools]") {
    TestServer ts;
    ts.compatibility_profile = CompatibilityProfile::OpenAIDerivative;
    ts.registry.register_model(make_info("qwen3.6-27b"));
    REQUIRE(ts.coordinator.load("qwen3.6-27b"));
    REQUIRE(ts.start());
    httplib::Client cli("127.0.0.1", ts.port);
    auto res = cli.Post(
        "/v1/chat/completions",
        R"({
          "model":"qwen3.6-27b",
          "stream":true,
          "stream_options":{"include_usage":true},
          "messages":[{"role":"user","content":"review local files"}],
          "tools":[{
            "type":"function",
            "function":{
              "name":"list_workspace",
              "description":"list files",
              "parameters":{"type":"object","properties":{"path":{"type":"string"}}}
            }
          }]
        })",
        "application/json");
    REQUIRE(res);
    REQUIRE(res->status == 200);

    std::vector<nlohmann::json> chunks;
    std::size_t pos = 0;
    while (pos < res->body.size()) {
        auto nl = res->body.find('\n', pos);
        if (nl == std::string::npos) nl = res->body.size();
        std::string line = res->body.substr(pos, nl - pos);
        pos = nl + 1;
        if (line.rfind("data: ", 0) != 0 || line.find("[DONE]") != std::string::npos) continue;
        chunks.push_back(nlohmann::json::parse(line.substr(6)));
    }

    bool saw_reasoning = false;
    bool saw_tool_header = false;
    bool saw_tool_args = false;
    bool saw_tool_finish = false;
    bool saw_usage = false;
    std::int64_t created = 0;
    for (std::size_t index = 0; index < chunks.size(); ++index) {
        const auto& chunk = chunks[index];
        if (created == 0) created = chunk["created"].get<std::int64_t>();
        REQUIRE(chunk["created"] == created);
        REQUIRE(chunk.contains("usage"));
        if (chunk["choices"].empty()) {
            REQUIRE(index + 1 == chunks.size());
            REQUIRE(chunk["usage"]["prompt_tokens"] == 8);
            REQUIRE(chunk["usage"]["completion_tokens"] == 12);
            REQUIRE(chunk["usage"]["total_tokens"] == 20);
            saw_usage = true;
            continue;
        }
        REQUIRE(chunk["usage"].is_null());
        const auto& choice = chunk["choices"][0];
        const auto& delta = choice["delta"];
        if (delta.contains("reasoning_content")) {
            saw_reasoning = true;
        }
        if (delta.contains("tool_calls")) {
            const auto& tc = delta["tool_calls"][0];
            REQUIRE(tc["index"].is_number_unsigned());
            if (tc.contains("id")) {
                REQUIRE(tc["id"].is_string());
                REQUIRE(tc["id"] == "call_test");
                REQUIRE(tc["type"] == "function");
                REQUIRE(tc["function"]["name"] == "list_workspace");
                saw_tool_header = true;
            }
            if (tc.contains("function") && tc["function"].contains("arguments")) {
                REQUIRE_FALSE(tc.contains("id"));
                REQUIRE(tc["function"]["arguments"].is_string());
                saw_tool_args = true;
            }
        }
        if (choice.contains("finish_reason") && choice["finish_reason"] == "tool_calls") {
            saw_tool_finish = true;
        }
    }
    REQUIRE(saw_reasoning);
    REQUIRE(saw_tool_header);
    REQUIRE(saw_tool_args);
    REQUIRE(saw_tool_finish);
    REQUIRE(saw_usage);
    REQUIRE(res->body.ends_with("data: [DONE]\n\n"));
    REQUIRE(ts.metrics.total_requests() == 1);
    auto usage = ts.stats_db.model_usage();
    REQUIRE(usage.size() == 1);
    REQUIRE(usage[0].model == "qwen3.6-27b");
    REQUIRE(usage[0].prompt_tokens == 8);
    REQUIRE(usage[0].completion_tokens == 12);
    ts.stop();
}

TEST_CASE("Routes: streaming holds split UTF-8 and replaces malformed trailing bytes",
          "[routes][chat][stream][utf8]") {
    TestServer ts;
    ts.compatibility_profile = CompatibilityProfile::OpenAIDerivative;
    ts.registry.register_model(make_info("utf8-model"));
    REQUIRE(ts.coordinator.load("utf8-model"));
    const auto* backend = dynamic_cast<const IModelMock*>(ts.coordinator.get_backend("utf8-model"));
    REQUIRE(backend);
    const_cast<IModelMock*>(backend)->split_utf8.store(true);
    REQUIRE(ts.start());

    httplib::Client client("127.0.0.1", ts.port);
    auto response = client.Post(
        "/v1/chat/completions",
        R"({"model":"utf8-model","stream":true,"messages":[{"role":"user","content":"test"}]})",
        "application/json");
    REQUIRE(response);
    REQUIRE(response->status == 200);

    std::string content;
    std::string reasoning;
    std::string arguments;
    std::size_t position = 0;
    while (position < response->body.size()) {
        auto newline = response->body.find('\n', position);
        if (newline == std::string::npos) newline = response->body.size();
        const std::string line = response->body.substr(position, newline - position);
        position = newline + 1;
        if (line.rfind("data: ", 0) != 0 || line == "data: [DONE]") continue;
        const auto chunk = nlohmann::json::parse(line.substr(6));
        const auto& delta = chunk["choices"][0]["delta"];
        content += delta.value("content", "");
        reasoning += delta.value("reasoning_content", "");
        if (delta.contains("tool_calls")) {
            const auto& function = delta["tool_calls"][0].value("function", nlohmann::json::object());
            arguments += function.value("arguments", "");
        }
    }

    REQUIRE(content == std::string("cost \xe2\x82\xac", 8) + "5\xef\xbf\xbd");
    REQUIRE(reasoning == std::string("idea \xf0\x9f\x92\xa1", 9));
    REQUIRE(arguments == std::string("{\"city\":\"M\xc3\xbcnchen\"}", 19));
    ts.stop();
}

TEST_CASE("Routes: text models reject image input before inference",
          "[routes][vision]") {
    TestServer ts;
    ts.registry.register_model(make_info("text-only"));
    REQUIRE(ts.coordinator.load("text-only"));
    REQUIRE(ts.start());

    httplib::Client client("127.0.0.1", ts.port);
    auto chat = client.Post(
        "/v1/chat/completions",
        R"({"model":"text-only","messages":[{"role":"user","content":[{"type":"text","text":"describe"},{"type":"image_url","image_url":{"url":"data:image/png;base64,AA=="}}]}]})",
        "application/json");
    REQUIRE(chat);
    REQUIRE(chat->status == 400);
    CHECK(nlohmann::json::parse(chat->body)["error"]["code"] ==
          "unsupported_capability");

    auto responses = client.Post(
        "/v1/responses",
        R"({"model":"text-only","input":[{"type":"message","role":"user","content":[{"type":"input_text","text":"describe"},{"type":"input_image","image_url":"data:image/png;base64,AA=="}]}]})",
        "application/json");
    REQUIRE(responses);
    REQUIRE(responses->status == 400);
    CHECK(nlohmann::json::parse(responses->body)["error"]["code"] ==
          "unsupported_capability");

    CHECK(ts.metrics.total_requests() == 0);
    ts.stop();
}

TEST_CASE("Routes: vision models admit image input and preserve the payload",
          "[routes][vision]") {
    TestServer ts;
    auto info = make_info("vision-model");
    info.has_vision = true;
    info.mmproj_path = "mmproj.gguf";
    ts.registry.register_model(std::move(info));
    REQUIRE(ts.coordinator.load("vision-model"));
    REQUIRE(ts.start());

    httplib::Client client("127.0.0.1", ts.port);
    auto response = client.Post(
        "/v1/chat/completions",
        R"({"model":"vision-model","messages":[{"role":"user","content":[{"type":"text","text":"describe"},{"type":"image_url","image_url":{"url":"data:image/png;base64,AA=="}}]}]})",
        "application/json");
    REQUIRE(response);
    REQUIRE(response->status == 200);

    const auto* model = dynamic_cast<const IModelMock*>(
        ts.coordinator.get_backend("vision-model"));
    REQUIRE(model);
    REQUIRE(model->last_request.messages.size() == 1);
    REQUIRE(model->last_request.messages[0].content.size() == 2);
    const auto* image = std::get_if<inference::ImageContent>(
        &model->last_request.messages[0].content[1]);
    REQUIRE(image);
    CHECK(image->bytes == std::vector<std::byte>{std::byte{0}});
    CHECK(ts.metrics.total_requests() == 1);
    ts.stop();
}

TEST_CASE("Routes: chat stream applies producer backpressure until disconnect",
          "[routes][chat][stream][backpressure]") {
    TestServer ts;
    ts.registry.register_model(make_info("chat-pressure"));
    REQUIRE(ts.coordinator.load("chat-pressure"));
    auto* backend = const_cast<IModelMock*>(
        dynamic_cast<const IModelMock*>(
            ts.coordinator.get_backend("chat-pressure")));
    REQUIRE(backend);
    backend->stream_delta_count.store(1000);

    httplib::Request request;
    request.is_connection_closed = [] { return false; };
    request.body =
        R"({"model":"chat-pressure","stream":true,"stream_options":{"include_obfuscation":false},"messages":[{"role":"user","content":"test"}]})";
    httplib::Response response;
    handle_chat_completions(request, response, ts.make_deps());
    REQUIRE(response.content_provider_);
    REQUIRE(wait_for_count(backend->stream_deltas_emitted, 8));
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    CHECK(backend->stream_deltas_emitted.load() < 1000);

    std::string emitted;
    httplib::DataSink sink;
    sink.write = [&emitted](const char* data, std::size_t size) {
        emitted.append(data, size);
        return false;
    };
    sink.is_writable = [] { return true; };
    sink.done = [] {};
    REQUIRE_FALSE(response.content_provider_(0, 0, sink));
    const auto frame_end = emitted.find("\n\n");
    REQUIRE(frame_end + 2 == emitted.size());
    const auto chunk = nlohmann::json::parse(
        emitted.substr(6, frame_end - 6));
    REQUIRE(emitted == serialize_chat_stream_delta(
        chunk["id"].get<std::string>(), chunk["model"].get<std::string>(),
        chunk["created"].get<std::int64_t>(),
        {{"content", std::string(64 * 1024, 'x')}}, false,
        false, "", false));
    CHECK(emitted.find("finish_reason\":\"stop") == std::string::npos);
    CHECK(emitted.find("\"usage\"") == std::string::npos);
    CHECK(emitted.find("[DONE]") == std::string::npos);
    REQUIRE(response.content_provider_resource_releaser_);
    response.content_provider_resource_releaser_(false);
    CHECK(backend->stream_deltas_emitted.load() < 1000);
    CHECK(ts.coordinator.active_request_count() == 0);
}

TEST_CASE("Routes: speech stream applies byte backpressure until disconnect",
          "[routes][speech][stream][backpressure]") {
    TestServer ts;
    auto info = make_info("speech-pressure");
    info.runtime = "sherpa_onnx";
    info.modality = "audio";
    info.capabilities = {"audio_speech"};
    ts.registry.register_model(info);
    REQUIRE(ts.coordinator.load("speech-pressure"));
    auto* backend = const_cast<IModelMock*>(
        dynamic_cast<const IModelMock*>(
            ts.coordinator.get_backend("speech-pressure")));
    REQUIRE(backend);
    backend->speech_chunk_count.store(1000);

    httplib::Request request;
    request.is_connection_closed = [] { return false; };
    request.body =
        R"({"model":"speech-pressure","input":"test","voice":"default","response_format":"pcm"})";
    httplib::Response response;
    handle_audio_speech(request, response, ts.make_deps());
    REQUIRE(response.content_provider_);
    REQUIRE(wait_for_count(backend->speech_chunks_emitted, 8));
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    CHECK(backend->speech_chunks_emitted.load() < 1000);

    REQUIRE(response.content_provider_resource_releaser_);
    response.content_provider_resource_releaser_(false);
    CHECK(backend->speech_chunks_emitted.load() < 1000);
    CHECK(ts.coordinator.active_request_count() == 0);
}

TEST_CASE("ensure_model_loaded: already-loaded model returns ok immediately", "[routes][swap]") {
    ModelRegistry registry;
    registry.set_factory([](const ModelInfo& info) {
        return std::make_unique<IModelMock>(info);
    });
    registry.register_model(make_info("a"));
    BackendCoordinator coordinator(registry);
    REQUIRE(coordinator.load("a"));
    SwapTracker tracker;
    GatewayDeps deps{coordinator, "10"};
    deps.auto_swap = true;
    deps.swap_tracker = &tracker;

    auto r = ensure_model_loaded(deps, "a");
    REQUIRE(r.ok);
    REQUIRE(r.status == 200);
}

TEST_CASE("ensure_model_loaded: auto_swap disabled yields 503 for a cold model", "[routes][swap]") {
    ModelRegistry registry;
    registry.set_factory([](const ModelInfo& info) {
        return std::make_unique<IModelMock>(info);
    });
    registry.register_model(make_info("a"));
    BackendCoordinator coordinator(registry);
    SwapTracker tracker;
    GatewayDeps deps{coordinator, "10"};
    deps.auto_swap = false;
    deps.swap_tracker = &tracker;

    auto r = ensure_model_loaded(deps, "a");
    REQUIRE_FALSE(r.ok);
    REQUIRE(r.status == 503);
    REQUIRE(r.code == "model_not_loaded");
}

TEST_CASE("ensure_model_loaded: missing swap executor fails before starting work",
          "[routes][swap]") {
    ModelRegistry registry;
    registry.set_factory([](const ModelInfo& info) {
        return std::make_unique<IModelMock>(info);
    });
    registry.register_model(make_info("a"));
    BackendCoordinator coordinator(registry);
    GatewayDeps deps{coordinator, "10"};
    deps.auto_swap = true;

    const auto result = ensure_model_loaded(deps, "a");

    REQUIRE_FALSE(result.ok);
    REQUIRE(result.status == 503);
    REQUIRE(result.code == "swap_executor_unavailable");
    REQUIRE_FALSE(coordinator.swap_in_progress());
}

TEST_CASE("ensure_model_loaded: unavailable runtime is returned without waiting",
          "[routes][swap]") {
    ModelRegistry registry;
    registry.set_factory([](const ModelInfo& info) {
        return std::make_unique<IModelMock>(info);
    });
    auto info = make_info("speech");
    info.runtime = "missing_runtime";
    registry.register_model(std::move(info));
    BackendCoordinator coordinator(registry);
    SwapTracker tracker;
    GatewayDeps deps{coordinator, "10"};
    deps.auto_swap = true;
    deps.swap_tracker = &tracker;
    const auto started = std::chrono::steady_clock::now();

    const auto result = ensure_model_loaded(deps, "speech");
    const auto elapsed = std::chrono::steady_clock::now() - started;

    REQUIRE_FALSE(result.ok);
    REQUIRE(result.status == 503);
    REQUIRE(result.code == "runtime_unavailable");
    REQUIRE(elapsed < std::chrono::milliseconds{250});
}

TEST_CASE("ensure_model_loaded: cancellation has a distinct outcome",
          "[routes][swap]") {
    ModelRegistry registry;
    registry.set_factory([](const ModelInfo& info) {
        return std::make_unique<IModelMock>(info);
    });
    registry.register_model(make_info("a"));
    BackendCoordinator coordinator(registry);
    SwapTracker tracker;
    GatewayDeps deps{coordinator, "10"};
    deps.auto_swap = true;
    deps.swap_tracker = &tracker;
    coordinator.request_swap_cancel();

    const auto result = ensure_model_loaded(deps, "a");

    REQUIRE_FALSE(result.ok);
    REQUIRE(result.status == 503);
    REQUIRE(result.code == "swap_cancelled");
    REQUIRE_FALSE(coordinator.is_loaded("a"));
    REQUIRE(tracker.snapshot().last_cancelled);
}

TEST_CASE("ensure_model_loaded: load failure is returned after completion",
          "[routes][swap]") {
    ModelRegistry registry;
    registry.set_factory([](const ModelInfo& info) {
        auto model = std::make_unique<IModelMock>(info);
        model->load_should_fail.store(true);
        return model;
    });
    registry.register_model(make_info("a"));
    BackendCoordinator coordinator(registry);
    SwapTracker tracker;
    GatewayDeps deps{coordinator, "10"};
    deps.auto_swap = true;
    deps.swap_tracker = &tracker;

    const auto result = ensure_model_loaded(deps, "a");

    REQUIRE_FALSE(result.ok);
    REQUIRE(result.status == 503);
    REQUIRE(result.code == "swap_failed");
    REQUIRE(result.message == "model load failed: mock load failed");
}

TEST_CASE("ensure_model_loaded: active GPU capacity is reported as transient",
          "[routes][swap][residency]") {
    ModelRegistry registry;
    registry.set_factory([](const ModelInfo& info) {
        auto model = std::make_unique<IModelMock>(info);
        model->vram_mb.store(info.vram_required_mb);
        return model;
    });
    auto active = make_info("gemma");
    active.vram_required_mb = 5000;
    auto waiting = make_info("qwen");
    waiting.vram_required_mb = 5000;
    registry.register_model(active);
    registry.register_model(waiting);
    BackendCoordinator coordinator(registry);
    coordinator.set_vram_budget(9000, 0);
    REQUIRE(coordinator.load("gemma"));
    auto slot = coordinator.acquire_slot("gemma");
    REQUIRE(slot);
    SwapTracker tracker;
    observability::Metrics metrics;
    foundation::EventBus events;
    auto subscription = events.subscribe();
    GatewayDeps deps{coordinator, "10"};
    deps.auto_swap = true;
    deps.swap_tracker = &tracker;
    deps.metrics = &metrics;
    deps.events = &events;

    const auto result = ensure_model_loaded(deps, "qwen");

    REQUIRE_FALSE(result.ok);
    REQUIRE(result.status == 503);
    REQUIRE(result.code == "capacity_busy");
    REQUIRE(result.error_code == ErrorCode::ResourceBusy);
    CHECK(tracker.snapshot().last_deferred);
    CHECK(metrics.total_swaps() == 0);
    const auto swapping = subscription->wait_for(std::chrono::milliseconds{100});
    const auto waiting_event = subscription->wait_for(std::chrono::milliseconds{100});
    REQUIRE(swapping);
    REQUIRE(waiting_event);
    CHECK(nlohmann::json::parse(swapping->data)["state"] == "swapping");
    CHECK(nlohmann::json::parse(waiting_event->data)["state"] == "waiting");
    REQUIRE(coordinator.release_slot("gemma", *slot));
}

TEST_CASE("ensure_model_loaded: caller deadline bounds a slow swap wait",
          "[routes][swap][deadline]") {
    ModelRegistry registry;
    registry.set_factory([](const ModelInfo& info) {
        auto model = std::make_unique<IModelMock>(info);
        model->load_delay_ms.store(500);
        return model;
    });
    registry.register_model(make_info("a"));
    BackendCoordinator coordinator(registry);
    SwapTracker tracker;
    GatewayDeps deps{coordinator, "10"};
    deps.auto_swap = true;
    deps.swap_tracker = &tracker;
    const auto started = std::chrono::steady_clock::now();

    const auto result = ensure_model_loaded(
        deps, "a", started + std::chrono::milliseconds{40}, {});
    const auto elapsed = std::chrono::steady_clock::now() - started;

    REQUIRE_FALSE(result.ok);
    CHECK(result.error_code == ErrorCode::Timeout);
    CHECK(elapsed < std::chrono::milliseconds{200});
    tracker.join();
}

TEST_CASE("request observation uses one canonical record for every sink",
          "[routes][observability][canonical]") {
    observability::Metrics metrics;
    observability::StatsDb stats(":memory:");
    foundation::EventBus events;
    auto subscription = events.subscribe();
    model::InferenceResult result;
    result.prompt_tokens = 100;
    result.cached_prompt_tokens = 60;
    result.completion_tokens = 20;
    result.reasoning_tokens = 7;
    result.prompt_duration_ms = 20.0f;
    result.generation_duration_ms = 10.0f;
    result.duration_ms = 35.0f;
    result.tokens_per_second = 1.0f;
    result.finish_reason = "stop";
    RequestObservation observation;
    observation.request_id = "req-sink-agreement";
    observation.principal_class = "openai_data_plane";
    observation.endpoint = "/v1/chat/completions";
    observation.protocol_profile = "strict_openai";
    observation.modality = "text";
    observation.stream = true;
    observation.queue_duration_ms = 3.0;
    observation.swap_load_duration_ms = 4.0;
    observation.first_token_duration_ms = 5.0;
    observation.output_audio_seconds = 1.5;
    observation.input_image_count = 2;
    observation.output_image_count = 1;

    record_request(&metrics, &stats, &events, "alias", result, 200, 2,
                   0.0, 0, "real", observation);

    const auto rows = stats.recent_requests(1);
    REQUIRE(rows.size() == 1);
    const auto event = subscription->wait_for(std::chrono::milliseconds{100});
    REQUIRE(event);
    const auto payload = nlohmann::json::parse(event->data);
    const auto snapshot = metrics.snapshot_for("alias");
    CHECK(rows[0].request_id == "req-sink-agreement");
    CHECK(payload["requestId"] == rows[0].request_id);
    CHECK(payload["endpoint"] == rows[0].endpoint);
    CHECK(payload["protocolProfile"] == rows[0].protocol_profile);
    CHECK(payload["cacheWriteTokens"] == rows[0].cache_write_tokens);
    CHECK(payload["reasoningTokens"] == rows[0].reasoning_tokens);
    CHECK(payload["queueDurationMs"] == rows[0].queue_duration_ms);
    CHECK(payload["swapLoadDurationMs"] == rows[0].swap_load_duration_ms);
    CHECK(payload["firstTokenDurationMs"] == rows[0].first_token_duration_ms);
    CHECK(payload["outputAudioSeconds"] == rows[0].output_audio_seconds);
    CHECK(payload["inputImageCount"] == rows[0].input_image_count);
    CHECK(payload["outputImageCount"] == rows[0].output_image_count);
    CHECK(rows[0].cache_write_tokens == 40);
    CHECK(rows[0].tokens_per_second == Catch::Approx(2000.0));
    CHECK(payload["tokensPerSecond"].get<double>() ==
          Catch::Approx(rows[0].tokens_per_second));
    CHECK(snapshot.last_tokens_per_second ==
          Catch::Approx(rows[0].tokens_per_second));
}

TEST_CASE("ensure_model_loaded: caller cancellation interrupts a slow swap wait",
          "[routes][swap][deadline]") {
    ModelRegistry registry;
    registry.set_factory([](const ModelInfo& info) {
        auto model = std::make_unique<IModelMock>(info);
        model->load_delay_ms.store(500);
        return model;
    });
    registry.register_model(make_info("a"));
    BackendCoordinator coordinator(registry);
    SwapTracker tracker;
    GatewayDeps deps{coordinator, "10"};
    deps.auto_swap = true;
    deps.swap_tracker = &tracker;
    std::atomic<bool> cancelled{false};
    std::jthread cancel([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        cancelled.store(true);
    });
    const auto started = std::chrono::steady_clock::now();

    const auto result = ensure_model_loaded(
        deps, "a", started + std::chrono::seconds{1},
        [&] { return cancelled.load(); });
    const auto elapsed = std::chrono::steady_clock::now() - started;

    REQUIRE_FALSE(result.ok);
    CHECK(result.error_code == ErrorCode::Cancelled);
    CHECK(elapsed < std::chrono::milliseconds{200});
    tracker.join();
}

TEST_CASE("ensure_model_loaded: cold voice sidecar bypasses an active GPU swap",
          "[routes][swap][voice]") {
    ModelRegistry registry;
    const auto factory = [](const ModelInfo& info) -> std::unique_ptr<IBackend> {
        auto model = std::make_unique<IModelMock>(info);
        if (info.name == "qwen") model->load_delay_ms.store(300);
        return model;
    };
    registry.set_factory(factory);
    registry.register_factory("sherpa_onnx", factory);
    auto qwen = make_info("qwen");
    auto voice = make_info("voice");
    voice.runtime = "sherpa_onnx";
    voice.modality = "audio_speech";
    voice.vram_required_mb = 0;
    registry.register_model(qwen);
    registry.register_model(voice);
    BackendCoordinator coordinator(registry);
    SwapTracker tracker;
    GatewayDeps deps{coordinator, "10"};
    deps.auto_swap = true;
    deps.swap_tracker = &tracker;
    REQUIRE(start_swap_async(deps, "qwen").status == 202);
    IModelMock* qwen_backend = nullptr;
    for (int attempt = 0; attempt < 500; ++attempt) {
        qwen_backend = const_cast<IModelMock*>(
            dynamic_cast<const IModelMock*>(coordinator.get_backend("qwen")));
        if (qwen_backend && qwen_backend->load_started.load() &&
            !qwen_backend->loaded.load()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    REQUIRE(qwen_backend);
    REQUIRE(qwen_backend->load_started.load());
    REQUIRE_FALSE(qwen_backend->loaded.load());
    const auto started = std::chrono::steady_clock::now();

    const auto result = ensure_model_loaded(
        deps, "voice", started + std::chrono::milliseconds{100}, {});
    const auto elapsed = std::chrono::steady_clock::now() - started;

    REQUIRE(result.ok);
    CHECK(elapsed < std::chrono::milliseconds{100});
    CHECK(coordinator.is_loaded("voice"));
    tracker.join();
    CHECK(coordinator.is_loaded("qwen"));
}

TEST_CASE("SwapTracker owns the worker and joins it at destruction",
          "[routes][swap]") {
    ModelRegistry registry;
    registry.set_factory([](const ModelInfo& info) {
        auto model = std::make_unique<IModelMock>(info);
        model->load_delay_ms.store(80);
        return model;
    });
    registry.register_model(make_info("a"));
    BackendCoordinator coordinator(registry);

    {
        SwapTracker tracker;
        GatewayDeps deps{coordinator, "10"};
        deps.swap_tracker = &tracker;
        const auto result = start_swap_async(deps, "a");
        REQUIRE(result.status == 202);
    }

    REQUIRE(coordinator.is_loaded("a"));
    REQUIRE_FALSE(coordinator.swap_in_progress());
}

TEST_CASE("Async swap uses the gateway model-load deadline",
          "[routes][swap][deadline]") {
    ModelRegistry registry;
    registry.set_factory([](const ModelInfo& info) {
        auto model = std::make_unique<IModelMock>(info);
        model->load_delay_ms.store(80);
        return model;
    });
    registry.register_model(make_info("a"));
    BackendCoordinator coordinator(registry);
    SwapTracker tracker;
    GatewayDeps deps{coordinator, "10"};
    CHECK(deps.swap_timeout == std::chrono::minutes{5});
    deps.swap_timeout = std::chrono::milliseconds{20};
    deps.swap_tracker = &tracker;

    REQUIRE(start_swap_async(deps, "a").status == 202);
    tracker.join();

    CHECK_FALSE(coordinator.is_loaded("a"));
    CHECK(tracker.snapshot().last_error_code == ErrorCode::Timeout);
}

TEST_CASE("SwapTracker completion wait is bounded", "[routes][swap]") {
    SwapTracker tracker;
    std::atomic<bool> release{false};
    std::string error;
    const auto started = tracker.start(
        "", "a", 1,
        [&] {
            while (!release.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            tracker.end(true, "");
        },
        error);
    REQUIRE(started == SwapTracker::StartResult::Started);

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds{30};
    REQUIRE_FALSE(tracker.wait_until_idle(deadline));
    release.store(true);
    tracker.join();
    REQUIRE_FALSE(tracker.snapshot().swapping);
}

// Regression for issue #19: two requests racing on a cold model (e.g. OpenCode's
// title + main calls) must both succeed; the second waits out the in-progress
// swap instead of getting 503 swap_in_progress.
TEST_CASE("ensure_model_loaded: concurrent callers wait out the swap, no 503", "[routes][swap]") {
    ModelRegistry registry;
    registry.set_factory([](const ModelInfo& info) {
        auto m = std::make_unique<IModelMock>(info);
        m->load_delay_ms.store(300);  // make the swap slow enough to overlap
        return m;
    });
    registry.register_model(make_info("a"));
    BackendCoordinator coordinator(registry);
    SwapTracker tracker;
    GatewayDeps deps{coordinator, "10"};
    deps.auto_swap = true;
    deps.swap_tracker = &tracker;

    REQUIRE_FALSE(coordinator.is_loaded("a"));

    EnsureLoadedResult r0, r1;
    std::thread t0([&]() { r0 = ensure_model_loaded(deps, "a"); });
    std::thread t1([&]() { r1 = ensure_model_loaded(deps, "a"); });
    t0.join();
    t1.join();

    REQUIRE(r0.ok);
    REQUIRE(r1.ok);
    REQUIRE(r0.status == 200);
    REQUIRE(r1.status == 200);
    REQUIRE(coordinator.is_loaded("a"));
}

// Regression for issue #28: an in-flight predict_stream must stop promptly when
// the cancel flag is set (forwarded coordinator -> model), so an aborted turn
// releases its slot instead of hanging.
TEST_CASE("predict_stream honors the cancel flag and returns promptly", "[routes][cancel]") {
    ModelRegistry registry;
    registry.set_factory([](const ModelInfo& info) {
        auto m = std::make_unique<IModelMock>(info);
        m->block_until_cancel.store(true);  // hang until cancelled
        return m;
    });
    registry.register_model(make_info("a"));
    BackendCoordinator coordinator(registry);
    REQUIRE(coordinator.load("a"));
    auto slot = coordinator.acquire_slot("a");
    REQUIRE(slot);

    std::atomic<bool> cancel{false};
    std::atomic<bool> done{false};
    InferenceRequest req;
    std::thread worker([&]() {
        (void)coordinator.predict_stream(
            "a", *slot, req,
            [](const InferenceDelta&) { return true; }, &cancel);
        done.store(true);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds{60});
    REQUIRE_FALSE(done.load());  // still running without a cancel

    cancel.store(true);
    worker.join();
    REQUIRE(done.load());
    REQUIRE(coordinator.release_slot("a", *slot));
}
