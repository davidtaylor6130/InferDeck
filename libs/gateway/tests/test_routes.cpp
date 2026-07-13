#include <catch2/catch_test_macros.hpp>

#include "foundation/result.hpp"
#include "gateway/auth.hpp"
#include "gateway/cors.hpp"
#include "gateway/openai_routes.hpp"
#include "gateway/media_routes.hpp"
#include "gateway/routes.hpp"
#include "httplib.h"
#include "model/backend_coordinator.hpp"
#include "model/imodel.hpp"
#include "model/model_registry.hpp"
#include "observability/metrics.hpp"
#include "observability/stats_db.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <nlohmann/json.hpp>
#include <thread>

using namespace inferdeck;
using namespace inferdeck::model;
using namespace inferdeck::gateway;

using inferdeck::foundation::ErrorCode;
using inferdeck::foundation::Ok;
using inferdeck::foundation::Result;

namespace {

class IModelMock : public IModel, public IEmbeddingBackend, public IImageBackend,
                   public ISpeechBackend, public ITranscriptionBackend {
public:
    ModelInfo model_info{};
    std::atomic<bool> loaded{false};
    std::atomic<int> vram_mb{4096};
    std::atomic<int> max_slots{2};
    std::atomic<int> load_delay_ms{0};
    std::atomic<bool> block_until_cancel{false};
    std::atomic<bool> block_media_until_cancel{false};
    std::vector<int> busy_slots;
    mutable std::mutex mtx;
    std::string last_request_json;
    ChatTemplateMeta chat_meta_{};

    explicit IModelMock(ModelInfo info) : model_info(std::move(info)) {
        busy_slots.assign(max_slots.load(), 0);
    }

    const ModelInfo& info() const override { return model_info; }
    const ChatTemplateMeta& chat_template_meta() const override { return chat_meta_; }

    Result<void> load() override {
        const int delay = load_delay_ms.load();
        if (delay > 0) std::this_thread::sleep_for(std::chrono::milliseconds{delay});
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
        {
            std::lock_guard<std::mutex> lock(mtx);
            last_request_json = request.openai_body_json;
        }
        InferenceResult r;
        const auto body = request.openai_body_json.empty()
            ? nlohmann::json::object() : nlohmann::json::parse(request.openai_body_json);
        if (body.contains("tools") && !body["tools"].empty()) {
            r.reasoning_text = "need a tool";
            ToolCall call;
            call.id = "call_test";
            call.type = "function";
            call.function_name = "list_workspace";
            call.function_arguments = "{\"path\":\".\"}";
            r.tool_calls.push_back(std::move(call));
        } else {
            r.text = "Hello from model";
        }
        r.prompt_tokens = 3;
        r.completion_tokens = 4;
        return Ok(std::move(r));
    }

    Result<InferenceResult> predict_stream(
        int, const InferenceRequest&, const TokenCallback& callback,
        const std::atomic<bool>* cancel = nullptr) override {
        if (block_until_cancel.load()) {
            // Simulate a long generation that only ends when cancelled.
            while (!(cancel && cancel->load())) {
                std::this_thread::sleep_for(std::chrono::milliseconds{5});
            }
            return Ok(InferenceResult{});
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
            result.prompt_tokens += static_cast<int>(request.inputs[i].size());
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
        AudioResult result;
        result.bytes = {std::byte{0x52}, std::byte{0x49}, std::byte{0x46}, std::byte{0x46}};
        result.content_type = request.format == "wav" ? "audio/wav" : "audio/mpeg";
        result.duration_ms = 8;
        if (stream && !stream(result.bytes.data(), result.bytes.size())) {
            return inferdeck::foundation::Err<AudioResult>(ErrorCode::Cancelled, "cancelled");
        }
        return Ok(std::move(result));
    }

    Result<TranscriptionResult> transcribe(
        int, const TranscriptionRequest& request,
        const std::function<bool(int)>& progress = {}) override {
        if (progress && !progress(75)) return inferdeck::foundation::Err<TranscriptionResult>(ErrorCode::Cancelled, "cancelled");
        TranscriptionResult result;
        result.text = "test transcript";
        result.language = request.language.empty() ? "en" : request.language;
        result.duration_seconds = static_cast<float>(request.pcm.size()) / request.sample_rate;
        result.inference_ms = 7;
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

struct TestServer {
    ModelRegistry registry;
    BackendCoordinator coordinator;
    SwapTracker swap_tracker;
    observability::Metrics metrics;
    observability::StatsDb stats_db{":memory:"};
    httplib::Server server;
    std::thread th;
    int port{0};

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
        server.Post("/v1/swap/to/:name",
                    [this](const httplib::Request& req, httplib::Response& resp) {
                        handle_swap_to(req, resp, make_deps(), req.path_params.at("name"));
                    });
        server.Get("/v1/swap/status", [this](const httplib::Request& req, httplib::Response& resp) {
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

} // namespace

TEST_CASE("Routes: GET /v1/models lists registered models", "[routes][models]") {
    TestServer ts;
    ts.registry.register_model(make_info("qwen3.6-27b"));
    ts.registry.register_model(make_info("qwen3-coder-next"));
    REQUIRE(ts.start());

    httplib::Client cli("127.0.0.1", ts.port);
    auto res = cli.Get("/v1/models");
    REQUIRE(res);
    REQUIRE(res->status == 200);

    auto body = nlohmann::json::parse(res->body);
    REQUIRE(body["object"] == "list");
    REQUIRE(body["data"].is_array());
    REQUIRE(body["data"].size() == 2);
    REQUIRE(body["data"][0]["context_size"] == 65536);
    REQUIRE(body["data"][0]["context_length"] == 65536);
    REQUIRE(body["data"][0]["max_context_length"] == 65536);
    REQUIRE(body["data"][0]["limit"]["context"] == 65536);
    REQUIRE(body["data"][0]["object"] == "model");
    REQUIRE(body["data"][0]["runtime"] == "llama_cpp");
    REQUIRE(body["data"][0]["modality"] == "text");
    REQUIRE(body["data"][0]["inferdeck"]["capabilities"].is_array());
    REQUIRE(body["data"][0]["inferdeck"]["resources"]["configured_slots"] == 2);
    ts.stop();
}

TEST_CASE("Routes: GET /v1/models marks loaded model", "[routes][models]") {
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
    REQUIRE(body["data"][0]["loaded"] == true);
    REQUIRE(body["data"][0]["inferdeck"]["residency"]["primary"] == true);
    REQUIRE(body["data"][0]["inferdeck"]["residency"]["free_slots"] == 2);
    ts.stop();
}

TEST_CASE("Routes: POST /v1/swap/to/missing returns 404", "[routes][swap]") {
    TestServer ts;
    REQUIRE(ts.start());
    httplib::Client cli("127.0.0.1", ts.port);
    auto res = cli.Post("/v1/swap/to/missing", "", "application/json");
    REQUIRE(res);
    REQUIRE(res->status == 404);
    ts.stop();
}

TEST_CASE("Routes: POST /v1/swap/to/loaded returns 200", "[routes][swap]") {
    TestServer ts;
    ts.registry.register_model(make_info("qwen3.6-27b"));
    REQUIRE(ts.coordinator.load("qwen3.6-27b"));
    REQUIRE(ts.start());
    httplib::Client cli("127.0.0.1", ts.port);
    auto res = cli.Post("/v1/swap/to/qwen3.6-27b", "", "application/json");
    REQUIRE(res);
    REQUIRE(res->status == 200);
    auto body = nlohmann::json::parse(res->body);
    REQUIRE(body["status"] == "ready");
    ts.stop();
}

TEST_CASE("Routes: GET /v1/swap/status returns 200 with model info", "[routes][swap]") {
    TestServer ts;
    ts.registry.register_model(make_info("qwen3.6-27b"));
    REQUIRE(ts.coordinator.load("qwen3.6-27b"));
    REQUIRE(ts.start());
    httplib::Client cli("127.0.0.1", ts.port);
    auto res = cli.Get("/v1/swap/status");
    REQUIRE(res);
    REQUIRE(res->status == 200);
    auto body = nlohmann::json::parse(res->body);
    REQUIRE(body["loaded_model"] == "qwen3.6-27b");
    REQUIRE(body["loaded_models"] == nlohmann::json::array({"qwen3.6-27b"}));
    REQUIRE(body["residency"].size() == 1);
    REQUIRE(body["residency"][0]["runtime"] == "llama_cpp");
    REQUIRE(body["active_requests"] == 0);
    ts.stop();
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
    ts.stop();
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
    ts.stop();
}

TEST_CASE("Routes: POST /v1/chat/completions strips :latest suffix", "[routes][chat]") {
    TestServer ts;
    ts.registry.register_model(make_info("qwen3.6-27b"));
    REQUIRE(ts.coordinator.load("qwen3.6-27b"));
    REQUIRE(ts.start());
    httplib::Client cli("127.0.0.1", ts.port);
    auto res = cli.Post("/v1/chat/completions",
                        R"({"model":"qwen3.6-27b:latest","messages":[{"role":"user","content":"hi"}],"stream":false})",
                        "application/json");
    REQUIRE(res);
    REQUIRE(res->status == 200);
    REQUIRE(nlohmann::json::parse(res->body)["model"] == "qwen3.6-27b");
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
    REQUIRE(body["object"] == "list");
    REQUIRE(body["model"] == info.name);
    REQUIRE(body["data"].size() == 2);
    REQUIRE(body["data"][0]["index"] == 0);
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
    REQUIRE_FALSE(embedding.get<std::string>().empty());
    ts.stop();
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
    REQUIRE(body["object"] == "response");
    REQUIRE(body["status"] == "completed");
    REQUIRE(body["model"] == "chat-model");
    REQUIRE(body["output"].size() == 1);
    REQUIRE(body["output"][0]["type"] == "message");
    REQUIRE(body["output"][0]["content"][0]["type"] == "output_text");
    REQUIRE(body["output"][0]["content"][0]["text"] == "Hello from model");
    REQUIRE(body["usage"]["input_tokens"] == 3);
    REQUIRE(body["usage"]["output_tokens"] == 4);
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
    const auto translated = nlohmann::json::parse(model->last_request_json);
    REQUIRE(translated["response_format"]["type"] == "json_schema");
    REQUIRE(translated["response_format"]["json_schema"]["name"] == "answer");
    ts.stop();
}

TEST_CASE("Routes: POST /v1/responses rejects stateful fields", "[routes][responses]") {
    TestServer ts;
    ts.registry.register_model(make_info("chat-model"));
    REQUIRE(ts.start());

    httplib::Client client("127.0.0.1", ts.port);
    auto response = client.Post("/v1/responses", nlohmann::json{
        {"model", "chat-model"}, {"input", "Hello"}, {"store", true},
    }.dump(), "application/json");
    REQUIRE(response);
    REQUIRE(response->status == 400);
    REQUIRE(nlohmann::json::parse(response->body)["error"]["code"] == "unsupported_parameter");
    ts.stop();
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
    REQUIRE(response->body.find("event: response.created") != std::string::npos);
    REQUIRE(response->body.find("event: response.reasoning_text.delta") != std::string::npos);
    REQUIRE(response->body.find("event: response.function_call_arguments.delta") != std::string::npos);
    REQUIRE(response->body.find("event: response.function_call_arguments.done") != std::string::npos);
    REQUIRE(response->body.find("event: response.completed") != std::string::npos);
    REQUIRE(response->body.find("data: [DONE]") == std::string::npos);
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
                       {"size", "512x512"}, {"n", 2}, {"seed", 42}}.dump(),
        "application/json");
    REQUIRE(response);
    REQUIRE(response->status == 200);
    const auto body = nlohmann::json::parse(response->body);
    REQUIRE(body["data"].size() == 2);
    CHECK(body["data"][0]["b64_json"] == "iVBORw==");
    ts.stop();
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
    CHECK(status.load() == 499);
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
                       {"speed", 1.0}}.dump(), "application/json");
    REQUIRE(response);
    CHECK(response->status == 200);
    CHECK(response->get_header_value("Content-Type") == "audio/wav");
    CHECK(response->body == "RIFF");
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
    httplib::UploadFormDataItems items{
        {"file", test_wav(), "test.wav", "audio/wav"},
        {"model", "whisper-model", "", ""},
        {"language", "en", "", ""},
        {"response_format", "json", "", ""},
    };
    auto response = client.Post("/v1/audio/transcriptions", items);
    REQUIRE(response);
    REQUIRE(response->status == 200);
    CHECK(nlohmann::json::parse(response->body)["text"] == "test transcript");
    ts.stop();
}

TEST_CASE("Media routes reject unsupported request shapes", "[routes][media]") {
    TestServer ts;
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
    ts.stop();
}

TEST_CASE("Routes: streaming tool call emits llama-server shaped SSE",
          "[routes][chat][stream][tools]") {
    TestServer ts;
    ts.registry.register_model(make_info("qwen3.6-27b"));
    REQUIRE(ts.coordinator.load("qwen3.6-27b"));
    REQUIRE(ts.start());
    httplib::Client cli("127.0.0.1", ts.port);
    auto res = cli.Post(
        "/v1/chat/completions",
        R"({
          "model":"qwen3.6-27b",
          "stream":true,
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
    for (const auto& chunk : chunks) {
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
    REQUIRE(ts.metrics.total_requests() == 1);
    auto usage = ts.stats_db.model_usage();
    REQUIRE(usage.size() == 1);
    REQUIRE(usage[0].model == "qwen3.6-27b");
    REQUIRE(usage[0].prompt_tokens == 8);
    REQUIRE(usage[0].completion_tokens == 12);
    ts.stop();
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
