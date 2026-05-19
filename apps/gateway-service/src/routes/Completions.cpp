#include "routes/Completions.hpp"
#include "llama_cpp/LlamaEngine.hpp"
#include "core/Logger.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
using json = nlohmann::json;

namespace inferdeck::gateway::routes {

static std::string MakeId() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    return "cmpl-" + std::to_string(ms);
}

void HandleCompletions(const httplib::Request& req, httplib::Response& resp) {
    auto& engine = inferdeck::core::LlamaEngine::Get();
    if (!engine.IsInitialized()) {
        resp.status = 503;
        resp.set_content(R"({"error":{"message":"Engine not initialized","type":"service_error","code":503}})", "application/json");
        return;
    }
    try {
        auto body = json::parse(req.body);
        std::string prompt = body.value("prompt", "");

        inferdeck::core::InferenceParams params;
        if (body.contains("max_tokens")) params.max_tokens = body["max_tokens"].get<int>();
        if (body.contains("temperature")) params.temperature = body["temperature"].get<float>();
        if (body.contains("top_p")) params.top_p = body["top_p"].get<float>();

        inferdeck::core::ChatMessage msg{inferdeck::core::MessageRole::User, prompt};
        auto result = engine.Predict({msg}, params);

        json response;
        response["id"] = MakeId();
        response["object"] = "text_completion";
        response["created"] = std::time(nullptr);
        response["model"] = engine.GetModelName();
        response["choices"] = json::array({{{"index", 0}, {"text", result.text}, {"finish_reason", "stop"}}});
        response["usage"] = {{"prompt_tokens", result.prompt_tokens}, {"completion_tokens", result.completion_tokens}, {"total_tokens", result.total_tokens}};
        resp.set_content(response.dump(), "application/json");
    } catch (const std::exception& e) {
        resp.status = 400;
        resp.set_content(json({{"error", {{"message", e.what()}, {"type", "invalid_request_error"}, {"code", 400}}}}).dump(), "application/json");
    }
}

void HandleCompletionsStream(const httplib::Request& req, httplib::Response& resp) {
    resp.status = 501;
    resp.set_content(R"({"error":{"message":"Streaming not yet implemented","type":"not_implemented","code":501}})", "application/json");
}

} // namespace
