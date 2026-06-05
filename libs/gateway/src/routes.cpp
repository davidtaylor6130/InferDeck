#include "gateway/routes.hpp"

#include "engine/token_sequence.hpp"
#include "gateway/streaming_sanitizer.hpp"
#include "foundation/logging.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <random>
#include <thread>

namespace inferdeck::gateway {

using namespace inferdeck::foundation;

namespace {

std::string make_id() {
    static std::mutex mtx;
    static std::mt19937_64 rng{std::random_device{}()};
    std::lock_guard<std::mutex> lock(mtx);
    return "chatcmpl-" + std::to_string(rng());
}

std::string sse_chunk(const std::string& id, const std::string& model,
                      const std::string& delta_json) {
    nlohmann::json chunk = {
        {"id", id},
        {"object", "chat.completion.chunk"},
        {"created", std::time(nullptr)},
        {"model", model},
        {"choices", nlohmann::json::array({
            {
                {"index", 0},
                {"delta", nlohmann::json::parse(delta_json)},
                {"finish_reason", nullptr},
            }
        })},
    };
    return "data: " + chunk.dump() + "\n\n";
}

std::string sse_done(const std::string& id, const std::string& model,
                     const std::string& finish_reason = "stop") {
    nlohmann::json chunk = {
        {"id", id},
        {"object", "chat.completion.chunk"},
        {"created", std::time(nullptr)},
        {"model", model},
        {"choices", nlohmann::json::array({
            {
                {"index", 0},
                {"delta", nlohmann::json::object()},
                {"finish_reason", finish_reason},
            }
        })},
    };
    return "data: " + chunk.dump() + "\n\ndata: [DONE]\n\n";
}

} // namespace

void write_json(httplib::Response& resp, int status,
                const nlohmann::json& body) {
    resp.status = status;
    resp.set_content(body.dump(), "application/json");
}

void write_error(httplib::Response& resp, int status, const std::string& code,
                 const std::string& message) {
    nlohmann::json body = {
        {"error", {{"code", code}, {"message", message}}},
    };
    write_json(resp, status, body);
}

std::string header_value(const httplib::Request& req, const std::string& name) {
    auto it = req.headers.find(name);
    if (it == req.headers.end()) return {};
    return it->second;
}

void handle_models(const httplib::Request& req, httplib::Response& resp,
                   const GatewayDeps& deps) {
    (void)req;
    nlohmann::json data = nlohmann::json::array();
    auto loaded = deps.coordinator.get_loaded_model();
    for (const auto& name : deps.coordinator.registry().list()) {
        const auto& info = deps.coordinator.registry().get_info(name);
        nlohmann::json entry = {
            {"id", name},
            {"object", "model"},
            {"created", std::time(nullptr)},
            {"owned_by", "inferdeck"},
            {"vram_required_mb", info.vram_required_mb},
            {"context_size", info.context_size},
            {"n_slots", info.n_slots},
            {"has_vision", info.has_vision},
            {"loaded", loaded && *loaded == name},
        };
        data.push_back(entry);
    }
    nlohmann::json body = {
        {"object", "list"},
        {"data", data},
    };
    write_json(resp, 200, body);
}

void handle_swap_to(const httplib::Request& req, httplib::Response& resp,
                    const GatewayDeps& deps, const std::string& model_name) {
    (void)req;
    if (!deps.coordinator.registry().has(model_name)) {
        write_error(resp, 404, "model_not_found",
                    "model not registered: " + model_name);
        return;
    }
    auto current = deps.coordinator.get_loaded_model();
    if (current && *current == model_name) {
        nlohmann::json body = {
            {"status", "ready"},
            {"model", model_name},
            {"message", "model already loaded"},
        };
        write_json(resp, 200, body);
        return;
    }
    // Synchronous swap for debugging
    LOG_INFO("swap_start", "model={}", model_name);
    auto swap_result = deps.coordinator.swap_to(model_name);
    LOG_INFO("swap_complete", "model={} success={}", model_name, swap_result.has_value());
    if (!swap_result) {
        LOG_ERROR("swap_failed", "model={} error={}", model_name, swap_result.error().message);
        write_error(resp, 500, "swap_failed", swap_result.error().message);
        return;
    }
    nlohmann::json body = {
        {"status", "ready"},
        {"model", model_name},
        {"message", "model loaded successfully"},
    };
    write_json(resp, 200, body);
}

void handle_swap_status(const httplib::Request& req, httplib::Response& resp,
                        const GatewayDeps& deps) {
    (void)req;
    auto current = deps.coordinator.get_loaded_model();
    nlohmann::json body = {
        {"loaded_model", current ? *current : ""},
        {"vram_usage_mb", deps.coordinator.get_vram_usage()},
        {"active_requests", deps.coordinator.active_request_count()},
    };
    write_json(resp, 200, body);
}

void handle_chat_completions(const httplib::Request& req, httplib::Response& resp,
                             const GatewayDeps& deps) {
    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req.body);
    } catch (const std::exception& e) {
        write_error(resp, 400, "invalid_json", e.what());
        return;
    }
    if (!body.contains("model") || !body["model"].is_string()) {
        write_error(resp, 400, "missing_model", "request body must include 'model'");
        return;
    }
    std::string model_name = body["model"].get<std::string>();
    if (model_name.size() > 7 && model_name.compare(model_name.size() - 7, 7, ":latest") == 0) {
        model_name.resize(model_name.size() - 7);
    }

    if (!deps.coordinator.registry().has(model_name)) {
        write_error(resp, 404, "model_not_found",
                    "model not registered: " + model_name);
        return;
    }
    if (!deps.coordinator.is_loaded(model_name)) {
        resp.status = 503;
        resp.set_header("Retry-After", deps.default_swap_timeout_s);
        write_error(resp, 503, "model_not_loaded",
                    "model not loaded; POST /v1/swap/to/" + model_name +
                    " then retry");
        return;
    }

    bool stream = body.value("stream", false);

    int slot_id = -1;
    {
        model::AcquireSlotOptions opts;
        opts.timeout = std::chrono::milliseconds{30000};
        opts.block = true;
        auto sr = deps.coordinator.acquire_slot(model_name, opts);
        if (!sr) {
            int status = 503;
            std::string code = "no_slots";
            if (sr.error().code == foundation::ErrorCode::Timeout) {
                status = 503;
                code = "slot_timeout";
            } else if (sr.error().code == foundation::ErrorCode::Cancelled) {
                status = 503;
                code = "cancelled";
            } else if (sr.error().code == foundation::ErrorCode::NotFound) {
                status = 404;
                code = "model_not_loaded";
            }
            resp.set_header("Retry-After", "1");
            write_error(resp, status, code, sr.error().message);
            return;
        }
        slot_id = *sr;
    }

    const std::string id = make_id();
    const std::string stream_model = model_name;

    std::function<void()> release_slot = [slot_id, &deps, model_name]() noexcept {
        if (slot_id >= 0) {
            (void)deps.coordinator.release_slot(model_name, slot_id);
        }
    };
    struct ReleaseGuard {
        std::function<void()>* fn;
        bool armed{true};
        void disarm() { armed = false; }
        ~ReleaseGuard() { if (armed && fn) (*fn)(); }
    } guard{&release_slot};

    if (!stream) {
        std::string prompt = "stub-prompt";
        if (body.contains("messages") && body["messages"].is_array() &&
            !body["messages"].empty()) {
            const auto& last = body["messages"].back();
            if (last.is_object() && last.contains("content") &&
                last["content"].is_string()) {
                prompt = last["content"].get<std::string>();
            }
        }
        model::InferenceRequest ir;
        ir.prompt = prompt;
        ir.max_tokens = body.value("max_tokens", 256);
        ir.temperature = static_cast<float>(body.value("temperature", 0.7));
        ir.top_p = static_cast<float>(body.value("top_p", 0.95));
        ir.top_k = body.value("top_k", 40);

        auto pr = deps.coordinator.predict(model_name, slot_id, ir);
        if (!pr) {
            write_error(resp, 500, "inference_error", pr.error().message);
            return;
        }

        nlohmann::json resp_body = {
            {"id", id},
            {"object", "chat.completion"},
            {"created", std::time(nullptr)},
            {"model", stream_model},
            {"choices", nlohmann::json::array({
                {
                    {"index", 0},
                    {"message", {
                        {"role", "assistant"},
                        {"content", pr->text},
                    }},
                    {"finish_reason", "stop"},
                }
            })},
            {"usage", {
                {"prompt_tokens", pr->prompt_tokens},
                {"completion_tokens", pr->completion_tokens},
                {"total_tokens", pr->prompt_tokens + pr->completion_tokens},
            }},
        };
        guard.disarm();
        write_json(resp, 200, resp_body);
        return;
    }

    resp.set_header("Content-Type", "text/event-stream");
    resp.set_header("Cache-Control", "no-cache");
    resp.set_header("Connection", "keep-alive");

    model::InferenceRequest ir;
    ir.prompt = "stub-stream-prompt";
    ir.max_tokens = body.value("max_tokens", 256);
    ir.temperature = static_cast<float>(body.value("temperature", 0.7));
    ir.top_p = static_cast<float>(body.value("top_p", 0.95));
    ir.top_k = body.value("top_k", 40);

    auto pr = deps.coordinator.predict(model_name, slot_id, ir);
    if (!pr) {
        resp.set_chunked_content_provider(
            "text/event-stream",
            [id, stream_model, msg = pr.error().message](
                std::size_t, httplib::DataSink& sink) {
                std::string out;
                nlohmann::json err_chunk = {
                    {"id", id},
                    {"object", "chat.completion.chunk"},
                    {"created", std::time(nullptr)},
                    {"model", stream_model},
                    {"choices", nlohmann::json::array({
                        {{"index", 0},
                         {"delta", nlohmann::json::object()},
                         {"finish_reason", "error"}}
                    })},
                    {"error", {{"message", msg}}},
                };
                out += "data: " + err_chunk.dump() + "\n\n";
                out += "data: [DONE]\n\n";
                sink.write(out.data(), out.size());
                sink.done();
                return true;
            });
        return;
    }

    const std::string text = pr->text;
    const std::string sanitized_full = [&]() {
        StreamingSanitizer s;
        auto out = s.on_token(text);
        auto tail = s.finish();
        return out + tail;
    }();
    (void)sanitized_full;

    resp.set_chunked_content_provider(
        "text/event-stream",
        [id, stream_model, text, slot_release = release_slot](
            std::size_t, httplib::DataSink& sink) mutable {
            static std::atomic<bool> started{false};
            bool expected = false;
            if (!started.compare_exchange_strong(expected, true)) {
                sink.done();
                return false;
            }
            std::string out;
            out += sse_chunk(id, stream_model, R"({"role":"assistant"})");
            StreamingSanitizer sanitizer;
            for (char c : text) {
                std::string tok(1, c);
                auto clean = sanitizer.on_token(tok);
                if (!clean.empty()) {
                    nlohmann::json delta = {{"content", clean}};
                    out += sse_chunk(id, stream_model, delta.dump());
                }
            }
            auto tail = sanitizer.finish();
            if (!tail.empty()) {
                nlohmann::json delta = {{"content", tail}};
                out += sse_chunk(id, stream_model, delta.dump());
            }
            out += sse_done(id, stream_model, "stop");
            sink.write(out.data(), out.size());
            sink.done();
            slot_release();
            return true;
        });
}

} // namespace inferdeck::gateway
