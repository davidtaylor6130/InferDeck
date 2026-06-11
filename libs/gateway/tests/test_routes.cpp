#include <catch2/catch_test_macros.hpp>

#include "foundation/result.hpp"
#include "gateway/auth.hpp"
#include "gateway/cors.hpp"
#include "gateway/routes.hpp"
#include "httplib.h"
#include "model/backend_coordinator.hpp"
#include "model/imodel.hpp"
#include "model/model_registry.hpp"
#include "observability/metrics.hpp"
#include "observability/stats_db.hpp"

#include <atomic>
#include <chrono>
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

class IModelMock : public IModel {
public:
    ModelInfo model_info{};
    std::atomic<bool> loaded{false};
    std::atomic<int> vram_mb{4096};
    std::atomic<int> max_slots{2};
    std::vector<int> busy_slots;
    mutable std::mutex mtx;
    ChatTemplateMeta chat_meta_{};

    explicit IModelMock(ModelInfo info) : model_info(std::move(info)) {
        busy_slots.assign(max_slots.load(), 0);
    }

    const ModelInfo& info() const override { return model_info; }
    const ChatTemplateMeta& chat_template_meta() const override { return chat_meta_; }

    Result<void> load() override { loaded.store(true); return Ok(); }
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

    Result<InferenceResult> predict(int, const InferenceRequest&) override {
        InferenceResult r;
        r.text = "Hello from model";
        r.prompt_tokens = 3;
        r.completion_tokens = 4;
        return Ok(std::move(r));
    }

    Result<InferenceResult> predict_stream(
        int, const InferenceRequest&, const TokenCallback& callback) override {
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
