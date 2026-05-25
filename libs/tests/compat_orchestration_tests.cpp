#include <catch2/catch_test_macros.hpp>
#include "RuntimeActivity.hpp"
#include "routes/ChatCompletions.hpp"
#include "routes/OllamaCompat.hpp"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

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

TEST_CASE("Plain OpenCode stream requests use true streaming backend", "[compat][opencode]") {
    json request = {
        {"stream", true},
        {"messages", json::array({{{"role", "user"}, {"content", "inspect files and make a plan"}}})}
    };

    REQUIRE(inferdeck::gateway::routes::ShouldUseStreamingBackend(request));
    REQUIRE(inferdeck::gateway::routes::ShouldUseStreamingBackendForClient(request, "OpenCode"));
    REQUIRE(inferdeck::gateway::routes::ShouldUseStreamingBackendForClient(request, "OpenAI compatible"));

    request["tools"] = json::array({{{"type", "function"}, {"function", {{"name", "read"}}}}});
    REQUIRE_FALSE(inferdeck::gateway::routes::ShouldUseStreamingBackend(request));
    REQUIRE_FALSE(inferdeck::gateway::routes::ShouldUseStreamingBackendForClient(request, "OpenCode"));
    REQUIRE(inferdeck::gateway::routes::ShouldForceNonStreamingBackend(request));

    request["stream"] = false;
    REQUIRE_FALSE(inferdeck::gateway::routes::ShouldUseStreamingBackend(request));
    REQUIRE_FALSE(inferdeck::gateway::routes::ShouldUseStreamingBackendForClient(request, "OpenAI compatible"));
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

TEST_CASE("OpenAI tool-call compatibility normalizes arguments and strips empty calls", "[compat][opencode][tools]") {
    json empty = json::array();
    REQUIRE(inferdeck::gateway::routes::NormalizeOpenAiToolCalls(empty).empty());

    json calls = json::array({
        {
            {"id", "call_1"},
            {"type", "function"},
            {"function", {{"name", "read"}, {"arguments", {{"filePath", "/tmp/App.tsx"}}}}}
        },
        {
            {"id", "bad"},
            {"type", "function"},
            {"function", {{"arguments", "{}"}}}
        }
    });

    auto normalized = inferdeck::gateway::routes::NormalizeOpenAiToolCalls(calls);
    REQUIRE(normalized.size() == 1);
    REQUIRE(normalized[0]["function"]["name"] == "read");
    REQUIRE(normalized[0]["function"]["arguments"].is_string());
    REQUIRE(normalized[0]["function"]["arguments"] == R"({"filePath":"/tmp/App.tsx"})");
}

TEST_CASE("Synthetic OpenAI SSE converts raw Qwen tool-call text into structured tool calls", "[compat][opencode][tools]") {
    json response = {
        {"id", "chatcmpl-test"},
        {"model", "qwen3.6-35b-a3b"},
        {"choices", json::array({
            {
                {"index", 0},
                {"message", {
                    {"role", "assistant"},
                    {"content", R"(<tool_call>{"name":"read","arguments":{"filePath":"/tmp/a/App.tsx"}}</tool_call>)"}
                }},
                {"finish_reason", "tool_calls"}
            }
        })}
    };

    auto stream = inferdeck::gateway::routes::BuildSyntheticChatCompletionStream(response);

    REQUIRE(stream.find("\"tool_calls\"") != std::string::npos);
    REQUIRE(stream.find("\"arguments\":\"{\\\"filePath\\\":\\\"/tmp/a/App.tsx\\\"}\"") != std::string::npos);
    REQUIRE(stream.find("<tool_call>") == std::string::npos);
    REQUIRE(stream.find("\"finish_reason\":\"tool_calls\"") != std::string::npos);
}

TEST_CASE("Malformed raw Qwen tool-call text is not emitted as partial tool calls", "[compat][opencode][tools]") {
    json response = {
        {"id", "chatcmpl-test"},
        {"model", "qwen3.6-35b-a3b"},
        {"choices", json::array({
            {
                {"index", 0},
                {"message", {
                    {"role", "assistant"},
                    {"content", R"(Before <tool_call>{"name":"read","arguments": After)"}
                }},
                {"finish_reason", "stop"}
            }
        })}
    };

    auto stream = inferdeck::gateway::routes::BuildSyntheticChatCompletionStream(response);

    REQUIRE(stream.find("\"tool_calls\"") == std::string::npos);
    REQUIRE(stream.find("<tool_call>") == std::string::npos);
    REQUIRE(stream.find("\"finish_reason\":\"stop\"") != std::string::npos);
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
    REQUIRE(SanitizeAssistantContent("<|start|>assistantinternal-ok") == "internal-ok");
    REQUIRE(SanitizeAssistantContent("<|channel|>analysis") == "");
    REQUIRE(SanitizeAssistantContent("<|channel|>analysis<|message|>notes<|end|><|channel|>finalstream-ok") == "stream-ok");
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

TEST_CASE("Synthetic tool-call SSE emits indexed tool deltas before terminal chunk", "[compat][opencode]") {
    json response = {
        {"id", "chatcmpl-test"},
        {"model", "qwen3.6-35b-a3b"},
        {"choices", json::array({
            {
                {"index", 0},
                {"message", {
                    {"role", "assistant"},
                    {"content", "<think>inspect files</think>Reading several files."},
                    {"tool_calls", json::array({
                        {
                            {"id", "call_1"},
                            {"type", "function"},
                            {"function", {{"name", "read"}, {"arguments", R"({"filePath":"/tmp/a/package.json"})"}}}
                        },
                        {
                            {"id", "call_2"},
                            {"type", "function"},
                            {"function", {{"name", "read"}, {"arguments", R"({"filePath":"/tmp/b/README.md"})"}}}
                        }
                    })}
                }},
                {"finish_reason", "tool_calls"}
            }
        })}
    };

    auto stream = inferdeck::gateway::routes::BuildSyntheticChatCompletionStream(response);
    auto first_tool = stream.find("\"id\":\"call_1\"");
    auto second_tool = stream.find("\"id\":\"call_2\"");
    auto terminal = stream.find("\"finish_reason\":\"tool_calls\"");

    REQUIRE(first_tool != std::string::npos);
    REQUIRE(second_tool != std::string::npos);
    REQUIRE(terminal != std::string::npos);
    REQUIRE(first_tool < terminal);
    REQUIRE(second_tool < terminal);
    REQUIRE(stream.find("\"index\":0") != std::string::npos);
    REQUIRE(stream.find("\"index\":1") != std::string::npos);
    REQUIRE(stream.find("data: [DONE]") > terminal);
}

TEST_CASE("Runtime observability exposes OpenCode response metadata and request age", "[compat][opencode][observability]") {
    auto& activity = inferdeck::gateway::RuntimeActivity::Get();
    auto job_id = activity.StartJob(
        "tool_chat.completion",
        "OpenCode",
        "qwen3.6-35b-a3b:latest",
        {{"stream", true}, {"tools", 11}, {"messages", 8}, {"forcedNonStreamingBackend", true}},
        80
    );

    inferdeck::core::InferenceResult result;
    result.http_status = 200;
    result.prompt_tokens = 12000;
    result.completion_tokens = 320;
    result.total_tokens = 12320;
    result.duration_ms = 2500.0f;

    activity.CompleteJob(job_id, result, {
        {"requestProtocol", "/v1/chat/completions"},
        {"responseProtocol", "openai.sse"},
        {"responseMode", "synthetic-sse"},
        {"sseChunks", 5},
        {"ndjsonChunks", 0},
        {"heartbeatChunks", 1},
        {"responseBytes", 2048},
        {"finishReason", "tool_calls"},
        {"toolCallCount", 7},
        {"waitingOnBackendOrToolFormatting", false}
    });

    auto obs = activity.ObservabilityJson();
    REQUIRE(obs["lastOpenCodeRequest"]["id"] == job_id);
    REQUIRE(obs["lastOpenCodeRequest"]["requestProtocol"] == "/v1/chat/completions");
    REQUIRE(obs["lastOpenCodeRequest"]["responseProtocol"] == "openai.sse");
    REQUIRE(obs["lastOpenCodeRequest"]["responseMode"] == "synthetic-sse");
    REQUIRE(obs["lastOpenCodeRequest"]["sseChunks"] == 5);
    REQUIRE(obs["lastOpenCodeRequest"]["ndjsonChunks"] == 0);
    REQUIRE(obs["lastOpenCodeRequest"]["heartbeatChunks"] == 1);
    REQUIRE(obs["lastOpenCodeRequest"]["responseBytes"] == 2048);
    REQUIRE(obs["lastOpenCodeRequest"]["finishReason"] == "tool_calls");
    REQUIRE(obs["lastOpenCodeRequest"]["toolCallCount"] == 7);
    REQUIRE(obs["lastOpenCodeRequest"]["phase"] == "completed");
    REQUIRE(obs["lastOpenCodeRequest"]["requestAgeSeconds"].get<double>() >= 0.0);
    REQUIRE_FALSE(obs["openCodeWaitingOnBackendOrToolFormatting"]);
}

TEST_CASE("Runtime observability exposes active OpenCode backend formatting waits", "[compat][opencode][observability]") {
    auto& activity = inferdeck::gateway::RuntimeActivity::Get();
    auto job_id = activity.StartJob(
        "tool_chat.completion",
        "OpenCode",
        "qwen3.6-35b-a3b:latest",
        {{"requestProtocol", "/v1/chat/completions"}, {"stream", true}, {"tools", 4}, {"messages", 6}, {"forcedNonStreamingBackend", true}},
        80
    );
    activity.MergeJobResult(job_id, {
        {"requestProtocol", "/v1/chat/completions"},
        {"responseProtocol", "openai.sse"},
        {"responseMode", "guarded-synthetic-sse"},
        {"waitingOnBackendOrToolFormatting", true}
    });

    auto obs = activity.ObservabilityJson();
    REQUIRE(obs["openCodeWaitingOnBackendOrToolFormatting"] == true);
    REQUIRE(obs["openCodeWaitingJob"]["id"] == job_id);

    activity.FailJob(job_id, "test cleanup", 499);
}

TEST_CASE("Runtime activity can fail stranded OpenCode jobs after backend abort", "[compat][opencode][observability]") {
    auto& activity = inferdeck::gateway::RuntimeActivity::Get();
    auto stranded = activity.StartJob(
        "tool_chat.completion",
        "OpenCode",
        "qwen3.6-35b-a3b:latest",
        {{"stream", true}, {"tools", 10}, {"messages", 2}, {"forcedNonStreamingBackend", true}},
        80
    );
    auto survivor = activity.StartJob(
        "chat.completion",
        "Open WebUI",
        "qwen3.6-35b-a3b:latest",
        {{"stream", false}},
        50
    );

    activity.FailRunningClientJobs("OpenCode", "backendAborted: test", 504);

    auto failed = activity.JobJson(stranded);
    REQUIRE(failed["status"] == "failed");
    REQUIRE(failed["phase"] == "failed");
    REQUIRE(failed["error"] == "backendAborted: test");

    auto untouched = activity.JobJson(survivor);
    REQUIRE(untouched["status"] == "running");
    activity.CancelJob(survivor);
}

TEST_CASE("Backend HTTP 0 and timeout failures leave no running GPU lock", "[compat][opencode][observability]") {
    auto& activity = inferdeck::gateway::RuntimeActivity::Get();
    auto http0 = activity.StartJob(
        "tool_chat.completion",
        "OpenCode",
        "qwen3.6-35b-a3b:latest",
        {{"requestProtocol", "/v1/chat/completions"}, {"stream", true}, {"tools", 2}, {"messages", 4}, {"forcedNonStreamingBackend", true}},
        80
    );

    inferdeck::core::InferenceResult result;
    result.http_status = 502;
    result.error_message = "llama-server returned HTTP 0";
    activity.CompleteJob(http0, result, {
        {"requestProtocol", "/v1/chat/completions"},
        {"responseProtocol", "openai.sse"},
        {"responseMode", "guarded-synthetic-sse-error"},
        {"finishReason", "error"},
        {"terminalCause", "backendError"},
        {"backendAbortState", "backend-error"},
        {"waitingOnBackendOrToolFormatting", false}
    });

    auto failed_http0 = activity.JobJson(http0);
    REQUIRE(failed_http0["phase"] == "failed");
    REQUIRE(failed_http0["httpStatus"] == 502);
    REQUIRE(failed_http0["terminalCause"] == "backendError");

    auto timeout = activity.StartJob(
        "tool_chat.completion",
        "OpenCode",
        "qwen3.6-35b-a3b:latest",
        {{"requestProtocol", "/v1/chat/completions"}, {"stream", true}, {"tools", 2}, {"messages", 4}, {"forcedNonStreamingBackend", true}},
        80
    );
    activity.MergeJobResult(timeout, {
        {"requestProtocol", "/v1/chat/completions"},
        {"responseProtocol", "openai.sse"},
        {"responseMode", "guarded-synthetic-sse-error"},
        {"finishReason", "error"},
        {"terminalCause", "timeout"},
        {"backendAbortState", "aborted"},
        {"waitingOnBackendOrToolFormatting", false}
    });
    activity.FailJob(timeout, "OpenCode tool request totalTimeout", 504);

    auto queue = activity.QueueJson();
    REQUIRE(queue["running"] == 0);
    REQUIRE(queue["gpuLocked"] == false);
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

TEST_CASE("Ollama chat translation bounds Open WebUI generations by default", "[compat][ollama]") {
    json ollama = {
        {"model", "qwen3.6-35b-a3b:latest"},
        {"stream", false},
        {"messages", json::array({{{"role", "user"}, {"content", "write a longer answer"}}})}
    };

    auto openai = inferdeck::gateway::routes::BuildOpenAiChatBodyFromOllama(ollama);

    REQUIRE(openai["model"] == "qwen3.6-35b-a3b:latest");
    REQUIRE(openai["stream"] == false);
    REQUIRE(openai["max_tokens"] == 4096);
}

TEST_CASE("Ollama chat translation preserves explicit num_predict", "[compat][ollama]") {
    json ollama = {
        {"model", "qwen3.6-35b-a3b:latest"},
        {"stream", true},
        {"messages", json::array({{{"role", "user"}, {"content", "write a longer answer"}}})},
        {"options", {{"num_predict", 8192}, {"temperature", 0.2}, {"top_p", 0.95}}}
    };

    auto openai = inferdeck::gateway::routes::BuildOpenAiChatBodyFromOllama(ollama);

    REQUIRE(openai["max_tokens"] == 8192);
    REQUIRE(openai["temperature"] == 0.2);
    REQUIRE(openai["top_p"] == 0.95);
}

TEST_CASE("Ollama compatibility requests can tag Open WebUI for observability", "[compat][ollama][observability]") {
    httplib::Request req;
    req.headers.emplace("User-Agent", "Mozilla/5.0 WindowsPowerShell/5.1");
    req.headers.emplace("X-InferDeck-Client", "Open WebUI");

    REQUIRE(inferdeck::gateway::routes::DetectChatClientName(req) == "Open WebUI");
}

TEST_CASE("Ollama chat stream translation emits newline JSON with thinking content and done", "[compat][ollama][opencode]") {
    std::string openai_sse;
    openai_sse += R"(data: {"choices":[{"delta":{"role":"assistant"},"finish_reason":null,"index":0}],"model":"qwen3.6-35b-a3b","object":"chat.completion.chunk"})";
    openai_sse += "\n\n";
    openai_sse += R"(data: {"choices":[{"delta":{"reasoning_content":"plan nodes"},"finish_reason":null,"index":0}],"model":"qwen3.6-35b-a3b","object":"chat.completion.chunk"})";
    openai_sse += "\n\n";
    openai_sse += R"(data: {"choices":[{"delta":{"content":"Here is the class."},"finish_reason":null,"index":0}],"model":"qwen3.6-35b-a3b","object":"chat.completion.chunk"})";
    openai_sse += "\n\n";
    openai_sse += R"(data: {"choices":[{"delta":{},"finish_reason":"stop","index":0}],"model":"qwen3.6-35b-a3b","object":"chat.completion.chunk"})";
    openai_sse += "\n\n";
    openai_sse += "data: [DONE]\n\n";

    auto ollama = inferdeck::gateway::routes::BuildOllamaChatStreamFromOpenAiSse(openai_sse, "qwen3.6-35b-a3b:latest");

    REQUIRE(ollama.find("\"thinking\":\"plan nodes\"") != std::string::npos);
    REQUIRE(ollama.find("\"content\":\"Here is the class.\"") != std::string::npos);
    REQUIRE(ollama.find("\"done_reason\":\"stop\"") != std::string::npos);
    REQUIRE(ollama.find("\"done\":true") != std::string::npos);
    REQUIRE(ollama.find("data: ") == std::string::npos);
}

TEST_CASE("Ollama chat stream translation preserves tool calls", "[compat][ollama][opencode]") {
    std::string openai_sse;
    openai_sse += R"(data: {"choices":[{"delta":{"tool_calls":[{"function":{"name":"write","arguments":"{\"path\":\"index.html\"}"},"id":"call_1","index":0,"type":"function"}]},"finish_reason":null,"index":0}],"model":"qwen3.6-35b-a3b","object":"chat.completion.chunk"})";
    openai_sse += "\n\n";
    openai_sse += R"(data: {"choices":[{"delta":{},"finish_reason":"tool_calls","index":0}],"model":"qwen3.6-35b-a3b","object":"chat.completion.chunk"})";
    openai_sse += "\n\n";
    openai_sse += "data: [DONE]\n\n";

    auto ollama = inferdeck::gateway::routes::BuildOllamaChatStreamFromOpenAiSse(openai_sse, "qwen3.6-35b-a3b:latest");

    REQUIRE(ollama.find("\"tool_calls\"") != std::string::npos);
    REQUIRE(ollama.find("\"arguments\":{\"path\":\"index.html\"}") != std::string::npos);
    REQUIRE(ollama.find("\"finish_reason\"") == std::string::npos);
    REQUIRE(ollama.find("\"done_reason\":\"tool_calls\"") != std::string::npos);
    REQUIRE(ollama.find("\"done\":true") != std::string::npos);
}

TEST_CASE("Ollama JSON response is native-shaped for stream false tool calls", "[compat][ollama][tools]") {
    json openai = {
        {"model", "qwen3.6-35b-a3b"},
        {"choices", json::array({
            {
                {"index", 0},
                {"message", {
                    {"role", "assistant"},
                    {"content", ""},
                    {"tool_calls", json::array({
                        {
                            {"id", "call_1"},
                            {"type", "function"},
                            {"function", {{"name", "read"}, {"arguments", R"({"filePath":"/tmp/App.tsx"})"}}}
                        }
                    })}
                }},
                {"finish_reason", "tool_calls"}
            }
        })},
        {"usage", {{"prompt_tokens", 1}, {"completion_tokens", 2}, {"total_tokens", 3}}}
    };

    auto ollama = inferdeck::gateway::routes::BuildOllamaChatResponseFromOpenAi(openai, "qwen3.6-35b-a3b:latest");

    REQUIRE(ollama["message"]["tool_calls"].is_array());
    REQUIRE(ollama["message"]["tool_calls"][0]["function"]["name"] == "read");
    REQUIRE(ollama["message"]["tool_calls"][0]["function"]["arguments"]["filePath"] == "/tmp/App.tsx");
    REQUIRE(ollama["done_reason"] == "tool_calls");
    REQUIRE(ollama["done"] == true);
}

TEST_CASE("Ollama stream true tool response emits valid NDJSON with native tool calls", "[compat][ollama][tools]") {
    json openai = {
        {"model", "qwen3.6-35b-a3b"},
        {"choices", json::array({
            {
                {"index", 0},
                {"message", {
                    {"role", "assistant"},
                    {"content", ""},
                    {"tool_calls", json::array({
                        {
                            {"id", "call_1"},
                            {"type", "function"},
                            {"function", {{"name", "read"}, {"arguments", R"({"filePath":"/tmp/App.tsx"})"}}}
                        },
                        {
                            {"id", "call_2"},
                            {"type", "function"},
                            {"function", {{"name", "read"}, {"arguments", R"({"filePath":"/tmp/Panel.jsx"})"}}}
                        }
                    })}
                }},
                {"finish_reason", "tool_calls"}
            }
        })}
    };

    auto ndjson = inferdeck::gateway::routes::BuildOllamaChatStreamFromOpenAiResponse(openai, "qwen3.6-35b-a3b:latest");
    REQUIRE(ndjson.find("data: ") == std::string::npos);

    std::vector<json> lines;
    std::size_t pos = 0;
    while (pos < ndjson.size()) {
        auto end = ndjson.find('\n', pos);
        auto line = ndjson.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        pos = end == std::string::npos ? ndjson.size() : end + 1;
        if (!line.empty()) lines.push_back(json::parse(line));
    }

    REQUIRE(lines.size() == 2);
    REQUIRE(lines[0]["done"] == false);
    REQUIRE(lines[0]["message"]["tool_calls"].size() == 2);
    REQUIRE(lines[0]["message"]["tool_calls"][0]["function"]["arguments"]["filePath"] == "/tmp/App.tsx");
    REQUIRE(lines[1]["done"] == true);
    REQUIRE(lines[1]["done_reason"] == "tool_calls");
}
