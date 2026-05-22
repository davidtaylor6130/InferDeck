#include <catch2/catch_test_macros.hpp>
#include "routes/ChatCompletions.hpp"
#include "routes/OllamaCompat.hpp"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <string>

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

TEST_CASE("Assistant content parser splits Qwen think blocks and GPT OSS channel blocks", "[compat][opencode]") {
    using inferdeck::gateway::routes::SanitizeAssistantContent;
    using inferdeck::gateway::routes::ExtractAssistantReasoningContent;

    REQUIRE(SanitizeAssistantContent("<think>private notes</think>\nA clean answer.") == "A clean answer.");
    REQUIRE(ExtractAssistantReasoningContent("<think>private notes</think>\nA clean answer.") == "private notes");
    REQUIRE(ExtractAssistantReasoningContent("<think>private notes") == "private notes");
    REQUIRE(SanitizeAssistantContent("<|channel|>analysis<|message|>private notes<|end|>I am OpenCode.") == "I am OpenCode.");
    REQUIRE(ExtractAssistantReasoningContent("<|channel|>analysis<|message|>private notes<|end|>I am OpenCode.") == "private notes");
    REQUIRE(SanitizeAssistantContent("<|channel|>analysis<|message|>notes<|end|><|channel|>final<|message|>Final text<|end|>") == "Final text");
    REQUIRE(ExtractAssistantReasoningContent("<|channel|>analysis<|message|>notes<|end|><|channel|>final<|message|>Final text<|end|>") == "notes");
}

TEST_CASE("Synthetic OpenAI SSE preserves reasoning separately from final content", "[compat][opencode]") {
    json response = {
        {"id", "chatcmpl-test"},
        {"object", "chat.completion"},
        {"created", 1700000000},
        {"model", "openai-gpt-oss-20b"},
        {"choices", json::array({
            {
                {"index", 0},
                {"message", {
                    {"role", "assistant"},
                    {"reasoning_content", "hidden chain of thought"},
                    {"content", "<|channel|>analysis<|message|>hidden notes<|end|><|channel|>final<|message|>Visible answer<|end|>"}
                }},
                {"finish_reason", "stop"}
            }
        })}
    };

    auto stream = inferdeck::gateway::routes::BuildSyntheticChatCompletionStream(response);

    REQUIRE(stream.find("\"role\":\"assistant\"") != std::string::npos);
    REQUIRE(stream.find("\"content\":\"Visible answer\"") != std::string::npos);
    REQUIRE(stream.find("\"reasoning_content\":\"hidden chain of thought\\nhidden notes\"") != std::string::npos);
    REQUIRE(stream.find("<|channel|>") == std::string::npos);
    REQUIRE(stream.find("<think>") == std::string::npos);
    REQUIRE(stream.find("data: [DONE]") != std::string::npos);
}

TEST_CASE("Synthetic OpenAI SSE preserves tool calls when reasoning is present", "[compat][opencode]") {
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
                    {"content", "<think>need a tool</think>I will call a tool."},
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

    REQUIRE(stream.find("\"reasoning_content\":\"need a tool\"") != std::string::npos);
    REQUIRE(stream.find("\"content\":\"I will call a tool.\"") != std::string::npos);
    REQUIRE(stream.find("\"tool_calls\"") != std::string::npos);
    REQUIRE(stream.find("\"finish_reason\":\"tool_calls\"") != std::string::npos);
    REQUIRE(stream.find("data: [DONE]") != std::string::npos);
}

TEST_CASE("OpenCode tool workflow message shape validates", "[compat][opencode]") {
    json request = {
        {"model", "qwen3.6-35b-a3b:latest"},
        {"stream", true},
        {"messages", json::array({
            {{"role", "user"}, {"content", "build a small site"}},
            {{"role", "assistant"}, {"content", nullptr}, {"tool_calls", json::array({
                {
                    {"id", "call_1"},
                    {"type", "function"},
                    {"function", {{"name", "write"}, {"arguments", R"({"path":"index.html","content":"<h1>Hello</h1>"})"}}}
                }
            })}},
            {{"role", "tool"}, {"tool_call_id", "call_1"}, {"name", "write"}, {"content", R"({"ok":true,"path":"index.html"})"}},
            {{"role", "user"}, {"content", json::array({
                {{"type", "text"}, {"text", "now add CSS"}},
                " and keep going"
            })}}
        })},
        {"tools", json::array({
            {
                {"type", "function"},
                {"function", {
                    {"name", "write"},
                    {"description", "write a file"},
                    {"parameters", {
                        {"type", "object"},
                        {"properties", {
                            {"path", {{"type", "string"}}},
                            {"content", {{"type", "string"}}}
                        }},
                        {"required", json::array({"path", "content"})}
                    }}
                }}
            }
        })}
    };

    REQUIRE(inferdeck::gateway::routes::ValidateChatRequest(request.dump()).empty());
    REQUIRE(inferdeck::gateway::routes::ShouldForceNonStreamingBackend(request));
}

TEST_CASE("Synthetic OpenAI SSE remains valid for long OpenCode responses", "[compat][opencode]") {
    std::string long_content = "<think>plan the files</think>";
    for (int i = 0; i < 256; ++i) {
        long_content += "section-" + std::to_string(i) + " ";
    }
    long_content += "<|channel|>analysis<|message|>review generated files<|end|><|channel|>final<|message|>done<|end|>";

    json response = {
        {"id", "chatcmpl-test"},
        {"object", "chat.completion"},
        {"created", 1700000000},
        {"model", "openai-gpt-oss-20b"},
        {"choices", json::array({
            {
                {"index", 0},
                {"message", {
                    {"role", "assistant"},
                    {"content", long_content}
                }},
                {"finish_reason", "stop"}
            }
        })}
    };

    auto stream = inferdeck::gateway::routes::BuildSyntheticChatCompletionStream(response);

    REQUIRE(stream.find("\"reasoning_content\":\"plan the files\\nreview generated files\"") != std::string::npos);
    REQUIRE(stream.find("section-255") != std::string::npos);
    REQUIRE(stream.find("done") != std::string::npos);
    REQUIRE(stream.find("<|channel|>") == std::string::npos);
    REQUIRE(stream.find("<think>") == std::string::npos);
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
