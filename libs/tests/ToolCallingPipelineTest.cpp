#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>
#include "routes/ChatCompletions.hpp"
#include "llama_cpp/LlamaEngine.hpp"
#include <nlohmann/json.hpp>
#include <string>

using json = nlohmann::json;
using namespace inferdeck::gateway::routes;

// ============================================================================
// Helper to create a response for a given model output and tools
// ============================================================================
static json MakeResponse(const std::string& text, const json& tools = json::array()) {
    return BuildChatCompletionResponseForTest("chatcmpl-test", "test-model", text, tools);
}

static json MakeResponseWithReasoning(const std::string& text, const std::string& reasoning, const json& tools = json::array()) {
    return BuildChatCompletionResponseForTest("chatcmpl-test", "test-model", text, reasoning, tools);
}

static std::string FinishReason(const json& resp) {
    if (!resp.contains("choices") || !resp["choices"].is_array() || resp["choices"].empty()) return "none";
    return resp["choices"][0].value("finish_reason", "none");
}

static json ToolCalls(const json& resp) {
    if (!resp.contains("choices") || !resp["choices"].is_array() || resp["choices"].empty()) return json::array();
    auto& msg = resp["choices"][0]["message"];
    return msg.contains("tool_calls") ? msg["tool_calls"] : json::array();
}

static std::string AssistantContent(const json& resp) {
    if (!resp.contains("choices") || !resp["choices"].is_array() || resp["choices"].empty()) return "";
    auto& msg = resp["choices"][0]["message"];
    return msg.value("content", "");
}

// ============================================================================
// Test 1: Graceful Degradation - Never returns finish_reason:error
// ============================================================================
TEST_CASE("Graceful degradation on extraction failure", "[tool][pipeline][graceful]") {
    json tools = json::array({json{{"type", "function"}, {"function", {{"name", "read"}, {"parameters", json::object()}}}}});

    SECTION("Structured call succeeds") {
        auto resp = MakeResponse(
            "<tool_call>{\"name\":\"read\",\"arguments\":{\"filePath\":\"test.txt\"}}</tool_call>",
            tools
        );
        REQUIRE(FinishReason(resp) == "tool_calls");
        REQUIRE(ToolCalls(resp).size() == 1);
    }

    SECTION("Empty response with tools returns stop") {
        auto resp = MakeResponse("", tools);
        REQUIRE(FinishReason(resp) == "stop");
        REQUIRE(ToolCalls(resp).empty());
    }

    SECTION("Gibberish text with tools returns stop, not error") {
        auto resp = MakeResponse("I will now examine this code", tools);
        REQUIRE(FinishReason(resp) == "stop");
        REQUIRE(ToolCalls(resp).empty());
        REQUIRE_FALSE(AssistantContent(resp).empty());
    }

    SECTION("Failed narrated intent returns stop with visible text") {
        auto resp = MakeResponse("I'll check the file to see what's there. The implementation looks fine.", tools);
        REQUIRE(FinishReason(resp) == "stop");
        REQUIRE(ToolCalls(resp).empty());
        REQUIRE(AssistantContent(resp).find("I'll check") != std::string::npos);
    }

    SECTION("Response with no tools never errors") {
        auto resp = MakeResponse("Hello, how can I help?");
        REQUIRE(FinishReason(resp) == "stop");
    }

    SECTION("Response with only reasoning_content") {
        auto resp = MakeResponseWithReasoning(
            "Let me look at this.",
            "I should check the file first to understand the code.",
            tools
        );
        REQUIRE(FinishReason(resp) == "stop");
        REQUIRE_FALSE(AssistantContent(resp).empty());
    }
}

// ============================================================================
// Test 2: Qwen XML Extraction - whitespace-tolerant
// ============================================================================
TEST_CASE("Qwen XML tool call extraction tolerates whitespace variations", "[tool][qwen][resilient]") {
    json tools = json::array({json{{"type", "function"}, {"function", {{"name", "read"}, {"parameters", json::object()}}}}});

    SECTION("Standard format works") {
        auto resp = MakeResponse(
            "<tool_call>{\"name\":\"read\",\"arguments\":{\"filePath\":\"test.txt\"}}</tool_call>",
            tools
        );
        REQUIRE(FinishReason(resp) == "tool_calls");
        REQUIRE(ToolCalls(resp).size() == 1);
    }

    SECTION("Extra whitespace inside tag") {
        auto resp = MakeResponse(
            "<tool_call >{\"name\":\"read\",\"arguments\":{\"filePath\":\"test.txt\"}}</tool_call>",
            tools
        );
        REQUIRE(FinishReason(resp) == "tool_calls");
        REQUIRE(ToolCalls(resp).size() == 1);
    }

    SECTION("Tool call embedded in think block") {
        auto resp = MakeResponse(
            "<think>I should read the file first.</think>\n"
            "<tool_call>{\"name\":\"read\",\"arguments\":{\"filePath\":\"test.txt\"}}</tool_call>",
            tools
        );
        REQUIRE(FinishReason(resp) == "tool_calls");
        REQUIRE(ToolCalls(resp).size() == 1);
    }

    SECTION("Tool call before thinking") {
        auto resp = MakeResponse(
            "<tool_call>{\"name\":\"read\",\"arguments\":{\"filePath\":\"test.txt\"}}</tool_call>\n"
            "<think>Now I have the file content</think>",
            tools
        );
        REQUIRE(FinishReason(resp) == "tool_calls");
    }

    SECTION("Multiple tool calls") {
        auto resp = MakeResponse(
            "<tool_call>{\"name\":\"read\",\"arguments\":{\"filePath\":\"a.txt\"}}</tool_call>\n"
            "<tool_call>{\"name\":\"read\",\"arguments\":{\"filePath\":\"b.txt\"}}</tool_call>",
            tools
        );
        REQUIRE(FinishReason(resp) == "tool_calls");
        REQUIRE(ToolCalls(resp).size() == 2);
    }

    SECTION("Text before and after tool call") {
        auto resp = MakeResponse(
            "Let me check.\n<tool_call>{\"name\":\"read\",\"arguments\":{\"filePath\":\"test.txt\"}}</tool_call>\nDone.",
            tools
        );
        REQUIRE(FinishReason(resp) == "tool_calls");
        REQUIRE(ToolCalls(resp).size() == 1);
        REQUIRE(AssistantContent(resp).find("Let me check") != std::string::npos);
    }
}

// ============================================================================
// Test 3: Bare JSON fallback
// ============================================================================
TEST_CASE("Bare JSON tool call without XML tags", "[tool][bare_json]") {
    json tools = json::array({json{{"type", "function"}, {"function", {{"name", "read"}, {"parameters", json::object()}}}}});
    json tools_multi = json::array({
        json{{"type", "function"}, {"function", {{"name", "read"}, {"parameters", json::object()}}}},
        json{{"type", "function"}, {"function", {{"name", "glob"}, {"parameters", json::object()}}}}
    });

    SECTION("Bare JSON object with name and arguments") {
        auto resp = MakeResponse(
            "{\"name\":\"read\",\"arguments\":{\"filePath\":\"test.txt\"}}",
            tools
        );
        REQUIRE(ToolCalls(resp).size() == 1);
        REQUIRE(ToolCalls(resp)[0]["function"]["name"] == "read");
    }

    SECTION("Bare JSON with prose around it") {
        auto resp = MakeResponse(
            "Looking at the file: {\"name\":\"read\",\"arguments\":{\"filePath\":\"test.txt\"}}",
            tools
        );
        REQUIRE(ToolCalls(resp).size() == 1);
        REQUIRE(ToolCalls(resp)[0]["function"]["name"] == "read");
    }
}

// ============================================================================
// Test 4: Tool call in think block
// ============================================================================
TEST_CASE("Tool call extraction from think block", "[tool][think_block]") {
    json tools = json::array({json{{"type", "function"}, {"function", {{"name", "read"}, {"parameters", json::object()}}}}});

    SECTION("Tool call only in think block") {
        auto resp = MakeResponse(
            "<think>I need to read the file first. <tool_call>{\"name\":\"read\",\"arguments\":{\"filePath\":\"test.txt\"}}</tool_call></think>",
            tools
        );
        REQUIRE(FinishReason(resp) == "tool_calls");
        REQUIRE(ToolCalls(resp).size() == 1);
    }

    SECTION("Tool call in reasoning (non-think) block") {
        auto resp = MakeResponse(
            "<|channel|>analysis<|message|>I need to check the source file first<|end|>\n"
            "<tool_call>{\"name\":\"read\",\"arguments\":{\"filePath\":\"main.cpp\"}}</tool_call>",
            tools
        );
        REQUIRE(FinishReason(resp) == "tool_calls");
        REQUIRE(ToolCalls(resp).size() == 1);
    }
}

// ============================================================================
// Test 5: Harmony/GPT-OSS format extraction
// ============================================================================
TEST_CASE("Harmony format extraction", "[tool][harmony]") {
    json tools = json::array({json{{"type", "function"}, {"function", {{"name", "read"}, {"parameters", json::object()}}}}});

    SECTION("Standard harmony call") {
        auto resp = MakeResponse(
            "<|channel|>commentary to=tool.read <|constrain|>json<|message|>{\"filePath\":\"test.txt\"}<|call|>",
            tools
        );
        REQUIRE(ToolCalls(resp).size() == 1);
        REQUIRE(ToolCalls(resp)[0]["function"]["name"] == "read");
    }

    SECTION("Harmony call with reasoning channel") {
        auto resp = MakeResponse(
            "<|channel|>analysis<|message|>I need to read the file.<|end|>\n"
            "<|channel|>commentary to=tool.read <|constrain|>json<|message|>{\"filePath\":\"test.txt\"}<|call|>",
            tools
        );
        REQUIRE(ToolCalls(resp).size() == 1);
    }
}

// ============================================================================
// Test 6: Labeled JSON payload (assistant_tool_calls_json:, tool_calls:)
// ============================================================================
TEST_CASE("Labeled JSON payload extraction", "[tool][labeled_json]") {
    json tools = json::array({json{{"type", "function"}, {"function", {{"name", "read"}, {"parameters", json::object()}}}}});

    SECTION("assistant_tool_calls_json prefix") {
        auto resp = MakeResponse(
            "assistant_tool_calls_json: {\"name\":\"read\",\"arguments\":{\"filePath\":\"test.txt\"}}",
            tools
        );
        REQUIRE(ToolCalls(resp).size() == 1);
    }

    SECTION("tool_calls: prefix") {
        auto resp = MakeResponse(
            "tool_calls: {\"name\":\"read\",\"arguments\":{\"filePath\":\"test.txt\"}}",
            tools
        );
        REQUIRE(ToolCalls(resp).size() == 1);
    }

    SECTION("tool_call: singular prefix") {
        auto resp = MakeResponse(
            "tool_call: {\"name\":\"read\",\"arguments\":{\"filePath\":\"test.txt\"}}",
            tools
        );
        REQUIRE(ToolCalls(resp).size() == 1);
    }
}

// ============================================================================
// Test 7: Content sanitization
// ============================================================================
TEST_CASE("Assistant content sanitization removes special tokens", "[tool][sanitize]") {
    SECTION("Think blocks are removed") {
        auto output = SanitizeAssistantContent(
            "I'll check the file <think>I should be methodical here</think> and see what it does."
        );
        REQUIRE(output.find("<think>") == std::string::npos);
        REQUIRE(output.find("I'll check the file") != std::string::npos);
        REQUIRE(output.find("and see what it does") != std::string::npos);
    }

    SECTION("Harmony tokens are stripped") {
        auto output = SanitizeAssistantContent(
            "<|channel|>analysis<|message|>reasoning<|end|>Final answer."
        );
        REQUIRE(output.find("<|channel|>") == std::string::npos);
        REQUIRE(output.find("<|end|>") == std::string::npos);
        REQUIRE(output.find("Final answer.") != std::string::npos);
    }

    SECTION("Tool call blocks in content are left intact by sanitize") {
        auto output = SanitizeAssistantContent(
            "Here is the file.\n<tool_call>{\"name\":\"read\"}</tool_call>"
        );
        REQUIRE(output.find("<tool_call>") != std::string::npos);
        REQUIRE(output.find("Here is the file.") != std::string::npos);
    }

    SECTION("Only tool call in content still has it after sanitize") {
        auto output = SanitizeAssistantContent("<tool_call>{\"name\":\"read\"}</tool_call>");
        REQUIRE(output.find("<tool_call>") != std::string::npos);
    }
}

// ============================================================================
// Test 8: Reasoning content extraction
// ============================================================================
TEST_CASE("Reasoning content extraction", "[tool][reasoning]") {
    SECTION("Think block reasoning") {
        auto reasoning = ExtractAssistantReasoningContent(
            "<think>I should check the imports first</think>\nNow I'll read the file."
        );
        REQUIRE(reasoning.find("check the imports") != std::string::npos);
    }

    SECTION("Harmony analysis reasoning") {
        auto reasoning = ExtractAssistantReasoningContent(
            "<|channel|>analysis<|message|>Let me trace through the logic<|end|>\nFinal result."
        );
        REQUIRE(reasoning.find("trace through the logic") != std::string::npos);
    }

    SECTION("Multiple reasoning blocks") {
        auto reasoning = ExtractAssistantReasoningContent(
            "<think>First thought</think>\n<|channel|>analysis<|message|>Second analysis<|end|>"
        );
        REQUIRE(reasoning.find("First thought") != std::string::npos);
        REQUIRE(reasoning.find("Second analysis") != std::string::npos);
    }
}

// ============================================================================
// Test 9: Full pipeline - response building with various inputs
// ============================================================================
TEST_CASE("Full pipeline: BuildChatCompletionResponseForTest", "[tool][pipeline][integration]") {
    json tools = json::array({json{{"type", "function"}, {"function", {{"name", "read"}, {"parameters", json::object()}}}}});

    SECTION("Content-only response has correct structure") {
        auto resp = MakeResponse("Hello world");
        REQUIRE(resp["id"] == "chatcmpl-test");
        REQUIRE(resp["object"] == "chat.completion");
        REQUIRE(resp["choices"][0]["finish_reason"] == "stop");
        REQUIRE(resp["choices"][0]["message"]["content"] == "Hello world");
        REQUIRE(resp.contains("usage"));
    }

    SECTION("Tool call response has correct finish reason") {
        auto resp = MakeResponse(
            "<tool_call>{\"name\":\"read\",\"arguments\":{\"filePath\":\"x.txt\"}}</tool_call>",
            tools
        );
        REQUIRE(FinishReason(resp) == "tool_calls");
        REQUIRE(ToolCalls(resp).size() == 1);
        REQUIRE(ToolCalls(resp)[0]["type"] == "function");
        REQUIRE(ToolCalls(resp)[0]["function"]["name"] == "read");
        REQUIRE_FALSE(ToolCalls(resp)[0]["id"].get<std::string>().empty());
    }

    SECTION("Response includes reasoning_content when present") {
        auto resp = MakeResponseWithReasoning(
            "Final answer.",
            "I reasoned about this.",
            tools
        );
        REQUIRE(resp["choices"][0]["message"].contains("reasoning_content"));
        REQUIRE(resp["choices"][0]["message"]["reasoning_content"].get<std::string>().find("reasoned") != std::string::npos);
    }

    SECTION("Usage tokens are populated") {
        auto resp = MakeResponse("Hello");
        REQUIRE(resp["usage"]["prompt_tokens"] >= 0);
        REQUIRE(resp["usage"]["completion_tokens"] >= 0);
        REQUIRE(resp["usage"]["total_tokens"] >= 0);
    }
}

// ============================================================================
// Test 10: Narration detection false positive prevention
// ============================================================================
TEST_CASE("Narration detection avoids false positives", "[tool][narration]") {
    json tools = json::array({json{{"type", "function"}, {"function", {{"name", "read"}, {"parameters", json::object()}}}}});

    SECTION("Short utterance under 20 chars is not narrated intent") {
        auto resp = MakeResponse("I'll check.", tools);
        REQUIRE(FinishReason(resp) == "stop");
    }

    SECTION("Complete sentence with period is not narrated intent") {
        auto resp = MakeResponse("I'll read the file to understand the code structure.", tools);
        REQUIRE(FinishReason(resp) == "stop");
    }

    SECTION("Final answer with findings is not narrated intent") {
        auto resp = MakeResponse(
            "Based on my findings, I recommend using a different approach.",
            tools
        );
        REQUIRE(FinishReason(resp) == "stop");
    }

    SECTION("Long text over 400 chars is not narrated intent") {
        std::string long_text(500, 'a');
        long_text += "I'll read the file";
        auto resp = MakeResponse(long_text, tools);
        REQUIRE(FinishReason(resp) == "stop");
    }
}

// ============================================================================
// Test 11: Multiple tools and name resolution
// ============================================================================
TEST_CASE("Tool name resolution from registry", "[tool][registry]") {
    json tools = json::array({
        json{{"type", "function"}, {"function", {{"name", "bash"}, {"parameters", json::object()}}}},
        json{{"type", "function"}, {"function", {{"name", "read"}, {"parameters", json::object()}}}},
        json{{"type", "function"}, {"function", {{"name", "glob"}, {"parameters", json::object()}}}},
        json{{"type", "function"}, {"function", {{"name", "grep"}, {"parameters", json::object()}}}}
    });

    SECTION("Exact name match") {
        auto resp = MakeResponse(
            "<tool_call>{\"name\":\"read\",\"arguments\":{\"filePath\":\"test.txt\"}}</tool_call>",
            tools
        );
        REQUIRE(ToolCalls(resp).size() == 1);
        REQUIRE(ToolCalls(resp)[0]["function"]["name"] == "read");
    }

    SECTION("Alias 'shell' resolves to bash") {
        auto resp = MakeResponse(
            "<tool_call>{\"name\":\"shell\",\"arguments\":{\"command\":\"ls\"}}</tool_call>",
            tools
        );
        REQUIRE(ToolCalls(resp).size() == 1);
        REQUIRE(ToolCalls(resp)[0]["function"]["name"] == "bash");
    }

    SECTION("Command arguments resolve to bash") {
        auto resp = MakeResponse(
            "<tool_call>{\"name\":\"execute\",\"arguments\":{\"command\":\"ls -la\"}}</tool_call>",
            tools
        );
        REQUIRE(ToolCalls(resp).size() == 1);
        REQUIRE(ToolCalls(resp)[0]["function"]["name"] == "bash");
    }

    SECTION("Alias 'search' resolves to grep") {
        auto resp = MakeResponse(
            "<tool_call>{\"name\":\"search\",\"arguments\":{\"pattern\":\"foo\",\"path\":\".\"}}</tool_call>",
            tools
        );
        REQUIRE(ToolCalls(resp).size() == 1);
        REQUIRE(ToolCalls(resp)[0]["function"]["name"] == "grep");
    }
}

// ============================================================================
// Test 12: Tool call deduplication
// ============================================================================
TEST_CASE("Duplicate tool calls are deduplicated", "[tool][dedup]") {
    json tools = json::array({json{{"type", "function"}, {"function", {{"name", "read"}, {"parameters", json::object()}}}}});

    SECTION("Identical calls are deduplicated") {
        auto resp = MakeResponse(
            "<tool_call>{\"name\":\"read\",\"arguments\":{\"filePath\":\"test.txt\"}}</tool_call>\n"
            "<tool_call>{\"name\":\"read\",\"arguments\":{\"filePath\":\"test.txt\"}}</tool_call>",
            tools
        );
        // Should dedupe to 1
        REQUIRE(ToolCalls(resp).size() == 1);
    }

    SECTION("Different calls are preserved") {
        auto resp = MakeResponse(
            "<tool_call>{\"name\":\"read\",\"arguments\":{\"filePath\":\"a.txt\"}}</tool_call>\n"
            "<tool_call>{\"name\":\"read\",\"arguments\":{\"filePath\":\"b.txt\"}}</tool_call>",
            tools
        );
        REQUIRE(ToolCalls(resp).size() == 2);
    }
}

// ============================================================================
// Test 13: Streaming synthetic response
// ============================================================================
TEST_CASE("BuildSyntheticChatCompletionStream produces valid SSE", "[tool][stream]") {
    json tools = json::array({json{{"type", "function"}, {"function", {{"name", "read"}, {"parameters", json::object()}}}}});

    SECTION("Tool call in streaming produces valid chunks") {
        json response = {
            {"id", "chatcmpl-test"},
            {"model", "qwen3-coder"},
            {"choices", json::array({
                {
                    {"index", 0},
                    {"message", {
                        {"role", "assistant"},
                        {"tool_calls", json::array({
                            {
                                {"id", "call_1"},
                                {"type", "function"},
                                {"function", {{"name", "read"}, {"arguments", R"({"filePath":"test.txt"})"}}}
                            }
                        })}
                    }},
                    {"finish_reason", "tool_calls"}
                }
            })}
        };

        std::string stream = BuildSyntheticChatCompletionStream(response, tools);
        REQUIRE(stream.find("data: ") == 0);
        REQUIRE(stream.find("[DONE]") != std::string::npos);
        REQUIRE(stream.find("\"tool_calls\"") != std::string::npos);
    }

    SECTION("Stream respects tool registry and resolves names") {
        json response = {
            {"id", "chatcmpl-test"},
            {"model", "model"},
            {"choices", json::array({
                {
                    {"index", 0},
                    {"message", {
                        {"role", "assistant"},
                        {"tool_calls", json::array({
                            {
                                {"id", "call_1"},
                                {"type", "function"},
                                {"function", {{"name", "shell"}, {"arguments", R"({"command":"ls"})"}}}
                            }
                        })}
                    }},
                    {"finish_reason", "tool_calls"}
                }
            })}
        };

        std::string stream = BuildSyntheticChatCompletionStream(response, tools);
        REQUIRE(stream.find("\"shell\"") == std::string::npos);
    }
}

// ============================================================================
// Test 14: Edge cases - empty, null, invalid
// ============================================================================
TEST_CASE("Edge cases in tool extraction", "[tool][edge]") {
    json tools = json::array({json{{"type", "function"}, {"function", {{"name", "read"}, {"parameters", json::object()}}}}});

    SECTION("Empty string returns stop") {
        auto resp = MakeResponse("", tools);
        REQUIRE(FinishReason(resp) == "stop");
    }

    SECTION("Only whitespace") {
        auto resp = MakeResponse("   \n  \t  ", tools);
        REQUIRE(FinishReason(resp) == "stop");
    }

    SECTION("Malformed JSON inside tag is skipped gracefully") {
        auto resp = MakeResponse(
            "<tool_call>{bad json here}</tool_call>\n"
            "<tool_call>{\"name\":\"read\",\"arguments\":{\"filePath\":\"test.txt\"}}</tool_call>",
            tools
        );
        REQUIRE(ToolCalls(resp).size() == 1);
        REQUIRE(FinishReason(resp) == "tool_calls");
    }

    SECTION("Unclosed tag") {
        auto resp = MakeResponse(
            "<tool_call>{\"name\":\"read\"",
            tools
        );
        REQUIRE(FinishReason(resp) == "stop");
        REQUIRE(ToolCalls(resp).empty());
    }

    SECTION("Tokens-only response with tools") {
        auto resp = MakeResponse(
            "I'll use the read function to look at the file and then summarize my findings.",
            tools
        );
        REQUIRE(FinishReason(resp) == "stop");
    }

    SECTION("Only tool_call tag reference without actual call") {
        auto resp = MakeResponse(
            "I should use tool_call to call a function.",
            tools
        );
        REQUIRE(FinishReason(resp) == "stop");
    }
}

// ============================================================================
// Test 15: JSON argument handling
// ============================================================================
TEST_CASE("Tool call arguments are properly serialized", "[tool][arguments]") {
    json tools = json::array({json{{"type", "function"}, {"function", {{"name", "read"}, {"parameters", json::object()}}}}});

    SECTION("String arguments") {
        auto resp = MakeResponse(
            "<tool_call>{\"name\":\"read\",\"arguments\":{\"filePath\":\"test.txt\"}}</tool_call>",
            tools
        );
        REQUIRE(ToolCalls(resp).size() == 1);
        auto args = ToolCalls(resp)[0]["function"]["arguments"];
        REQUIRE(args.is_string());
        auto parsed = json::parse(args.get<std::string>());
        REQUIRE(parsed["filePath"] == "test.txt");
    }

    SECTION("Numeric arguments") {
        auto resp = MakeResponse(
            "<tool_call>{\"name\":\"read\",\"arguments\":{\"lineCount\":50,\"startLine\":1}}</tool_call>",
            tools
        );
        REQUIRE(ToolCalls(resp).size() == 1);
        auto parsed = json::parse(ToolCalls(resp)[0]["function"]["arguments"].get<std::string>());
        REQUIRE(parsed["lineCount"] == 50);
    }

    SECTION("Complex nested arguments") {
        auto resp = MakeResponse(
            "<tool_call>{\"name\":\"read\",\"arguments\":{\"filePath\":\"src/main.cpp\",\"options\":{\"encoding\":\"utf-8\",\"maxLines\":100}}}</tool_call>",
            tools
        );
        REQUIRE(ToolCalls(resp).size() == 1);
        auto parsed = json::parse(ToolCalls(resp)[0]["function"]["arguments"].get<std::string>());
        REQUIRE(parsed["filePath"] == "src/main.cpp");
        REQUIRE(parsed["options"]["encoding"] == "utf-8");
    }

    SECTION("Array arguments") {
        auto resp = MakeResponse(
            "<tool_call>{\"name\":\"read\",\"arguments\":{\"paths\":[\"a.txt\",\"b.txt\"]}}</tool_call>",
            tools
        );
        REQUIRE(ToolCalls(resp).size() == 1);
        auto parsed = json::parse(ToolCalls(resp)[0]["function"]["arguments"].get<std::string>());
        REQUIRE(parsed["paths"].size() == 2);
    }
}

// ============================================================================
// Test 16: Model family detection
// ============================================================================
TEST_CASE("Model family detection functions", "[tool][model_family]") {
    auto& engine = inferdeck::core::LlamaEngine::Get();

    SECTION("GetModelFamily returns a string") {
        auto family = engine.GetModelFamily();
        REQUIRE_FALSE(family.empty());
    }
}

// ============================================================================
// Test 17: NormalizeOpenAiToolCalls
// ============================================================================
TEST_CASE("NormalizeOpenAiToolCalls utility", "[tool][normalize]") {
    SECTION("Empty array returns empty") {
        auto result = NormalizeOpenAiToolCalls(json::array());
        REQUIRE(result.empty());
    }

    SECTION("Null input returns empty") {
        auto result = NormalizeOpenAiToolCalls(json::array());
        REQUIRE(result.empty());
    }

    SECTION("Valid single call is normalized") {
        json input = json::array({
            {
                {"id", "call_1"},
                {"type", "function"},
                {"function", {{"name", "read"}, {"arguments", "{}"}}}
            }
        });
        auto result = NormalizeOpenAiToolCalls(input);
        REQUIRE(result.size() == 1);
    }
}

// ============================================================================
// Test 18: CompactToolResultContentForTest
// ============================================================================
TEST_CASE("Tool result content compaction", "[tool][compact]") {
    SECTION("Short content is not truncated") {
        auto result = CompactToolResultContentForTest("Hello world");
        REQUIRE(result == "Hello world");
    }

    SECTION("Long content is truncated with marker") {
        std::string long_content(10000, 'x');
        auto result = CompactToolResultContentForTest(long_content);
        REQUIRE(result.size() < long_content.size());
        REQUIRE(result.find("inferdeck_tool_result_truncated") != std::string::npos);
    }
}

// ============================================================================
// Test 19: Loose command tool call extraction
// ============================================================================
TEST_CASE("Loose command tool call extraction", "[tool][loose]") {
    json tools = json::array({json{{"type", "function"}, {"function", {{"name", "bash"}, {"parameters", json::object()}}}}});

    SECTION("tool_calls: with command field") {
        auto resp = MakeResponse(
            "tool_calls: {command: ls -la, description: list files}",
            tools
        );
        REQUIRE(FinishReason(resp) == "tool_calls");
    }
}

// ============================================================================
// Test 20: Pipeline with all formats combined
// ============================================================================
TEST_CASE("Complex mixed-format response", "[tool][mixed][integration]") {
    json tools = json::array({
        json{{"type", "function"}, {"function", {{"name", "bash"}, {"parameters", json::object()}}}},
        json{{"type", "function"}, {"function", {{"name", "read"}, {"parameters", json::object()}}}},
        json{{"type", "function"}, {"function", {{"name", "glob"}, {"parameters", json::object()}}}}
    });

    SECTION("Qwen XML with think blocks") {
        auto resp = MakeResponse(
            "<think>I should look at the project structure first</think>\n"
            "<tool_call>{\"name\":\"glob\",\"arguments\":{\"pattern\":\"**/*.cpp\"}}</tool_call>",
            tools
        );
        REQUIRE(FinishReason(resp) == "tool_calls");
        REQUIRE(ToolCalls(resp).size() == 1);
        REQUIRE(ToolCalls(resp)[0]["function"]["name"] == "glob");
        REQUIRE(AssistantContent(resp).empty());
    }

    SECTION("Qwen XML before think block") {
        auto resp = MakeResponse(
            "<tool_call>{\"name\":\"glob\",\"arguments\":{\"pattern\":\"**/*.cpp\"}}</tool_call>\n"
            "<think>Now let me read the files</think>",
            tools
        );
        REQUIRE(FinishReason(resp) == "tool_calls");
    }

    SECTION("Multiple XML calls with think block") {
        auto resp = MakeResponse(
            "<think>First find files</think>\n"
            "<tool_call>{\"name\":\"glob\",\"arguments\":{\"pattern\":\"**/*.cpp\"}}</tool_call>\n"
            "<think>Then read the main file</think>\n"
            "<tool_call>{\"name\":\"read\",\"arguments\":{\"filePath\":\"main.cpp\"}}</tool_call>",
            tools
        );
        REQUIRE(FinishReason(resp) == "tool_calls");
        REQUIRE(ToolCalls(resp).size() == 2);
        REQUIRE(ToolCalls(resp)[0]["function"]["name"] == "glob");
        REQUIRE(ToolCalls(resp)[1]["function"]["name"] == "read");
    }
}
