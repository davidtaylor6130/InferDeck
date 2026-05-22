#include <catch2/catch_test_macros.hpp>
#include "routes/ChatCompletions.hpp"
#include "routes/OllamaCompat.hpp"
#include <httplib.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

TEST_CASE("OpenCode tool requests force non-streaming backend", "[compat][opencode]") {
    json request = {
        {"stream", true},
        {"messages", json::array({{{"role", "user"}, {"content", "write a file"}}})},
        {"tools", json::array({{{"type", "function"}, {"function", {{"name", "write"}}}}})}
    };

    REQUIRE(inferdeck::gateway::routes::ShouldForceNonStreamingBackend(request));

    request.erase("tools");
    REQUIRE_FALSE(inferdeck::gateway::routes::ShouldForceNonStreamingBackend(request));

    request["tools"] = json::array();
    REQUIRE_FALSE(inferdeck::gateway::routes::ShouldForceNonStreamingBackend(request));
}

TEST_CASE("Synthetic OpenAI SSE preserves content tool calls finish reason and done", "[compat][opencode]") {
    json response = {
        {"id", "chatcmpl-test"},
        {"object", "chat.completion"},
        {"created", 1700000000},
        {"model", "qwen3.6-35b-a3b"},
        {"choices", json::array({
            {
                {"index", 0},
                {"message", {
                    {"role", "assistant"},
                    {"content", "I will call a tool."},
                    {"tool_calls", json::array({
                        {
                            {"id", "call_1"},
                            {"type", "function"},
                            {"function", {{"name", "write"}, {"arguments", R"({"path":"hello.txt"})"}}}
                        }
                    })}
                }},
                {"finish_reason", "tool_calls"}
            }
        })}
    };

    auto stream = inferdeck::gateway::routes::BuildSyntheticChatCompletionStream(response);

    REQUIRE(stream.find("\"role\":\"assistant\"") != std::string::npos);
    REQUIRE(stream.find("\"content\":\"I will call a tool.\"") != std::string::npos);
    REQUIRE(stream.find("\"tool_calls\"") != std::string::npos);
    REQUIRE(stream.find("\"finish_reason\":\"tool_calls\"") != std::string::npos);
    REQUIRE(stream.find("data: [DONE]") != std::string::npos);
}

TEST_CASE("Chat request validation rejects malformed OpenAI requests", "[compat][openai]") {
    REQUIRE(inferdeck::gateway::routes::ValidateChatRequest("").find("body") != std::string::npos);
    REQUIRE(inferdeck::gateway::routes::ValidateChatRequest(R"({"model":"x"})").find("messages") != std::string::npos);
    REQUIRE(inferdeck::gateway::routes::ValidateChatRequest(R"({"messages":[{"role":"alien","content":"x"}]})").find("role") != std::string::npos);
    REQUIRE(inferdeck::gateway::routes::ValidateChatRequest(R"({"messages":[{"role":"user","content":"x"}]})").empty());
}

TEST_CASE("Ollama version route exposes InferDeck Vulkan identity", "[compat][ollama]") {
    httplib::Request req;
    httplib::Response resp;

    inferdeck::gateway::routes::HandleOllamaVersion(req, resp);

    REQUIRE(resp.status == 200);
    auto body = json::parse(resp.body);
    REQUIRE(body["version"].get<std::string>().find("b9276-vulkan") != std::string::npos);
}
