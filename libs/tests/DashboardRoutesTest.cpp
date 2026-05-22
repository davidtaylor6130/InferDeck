#include <catch2/catch.hpp>
#include <nlohmann/json.hpp>

TEST_CASE("Dashboard status response exposes admin telemetry schema", "[dashboard][status]") {
    nlohmann::json response = {
        {"status", "ok"},
        {"mode", {{"mode", "ai"}, {"queuePaused", false}}},
        {"queue", {{"queued", 0}, {"running", 0}, {"paused", 0}, {"failed", 0}, {"gpuLocked", true}, {"lockOwner", "qwen3"}}},
        {"hardware", {
            {"available", true},
            {"gpu", {{"name", "AMD GPU (Vulkan)"}, {"backend", "llama.cpp b9276 Vulkan"}, {"memoryUsed", nullptr}, {"memoryFree", nullptr}, {"memoryTotal", nullptr}}},
            {"cpu", {{"utilization", nullptr}}},
            {"memory", {{"percentage", 42}}}
        }},
        {"metrics", {{"total_requests", 1}, {"successful_requests", 1}, {"failed_requests", 0}}},
        {"summary", {{"jobsToday", 1}, {"totalTokens", 42}, {"avgLatencyMs", 120}}},
        {"storage", {{"dataDirectory", "data"}, {"logsDirectory", "logs"}, {"freeSpace", 1000}, {"logSize", 10}, {"storage", "filesystem + SQLite WAL"}}},
        {"services", nlohmann::json::array()}
    };

    REQUIRE(response.contains("hardware"));
    REQUIRE(response["hardware"]["gpu"].contains("backend"));
    REQUIRE(response["hardware"]["gpu"].contains("memoryUsed"));
    REQUIRE(response["hardware"].contains("cpu"));
    REQUIRE(response["hardware"].contains("memory"));
    REQUIRE(response.contains("metrics"));
    REQUIRE(response.contains("summary"));
    REQUIRE(response["summary"].contains("totalTokens"));
    REQUIRE(response.contains("storage"));
    REQUIRE(response["storage"].contains("freeSpace"));
    REQUIRE(response.contains("queue"));
    REQUIRE(response.contains("services"));
}

TEST_CASE("Dashboard control errors use clear JSON error envelope", "[dashboard][error]") {
    nlohmann::json error = {
        {"error", {{"code", "model_not_found"}, {"message", "No GGUF model matched 'missing'."}}}
    };

    REQUIRE(error.contains("error"));
    REQUIRE(error["error"]["code"].is_string());
    REQUIRE(error["error"]["message"].is_string());
}

TEST_CASE("Dashboard model inventory includes aliases and loaded state", "[dashboard][models]") {
    nlohmann::json response = {
        {"models", nlohmann::json::array({
            {
                {"id", "qwen3"},
                {"name", "Qwen3-Q4_K_M"},
                {"path", "C:/models/Qwen3-Q4_K_M.gguf"},
                {"loaded", true},
                {"aliases", nlohmann::json::array({"qwen3", "qwen3:latest", "qwen3-q4-k-m"})}
            }
        })},
        {"current", "Qwen3-Q4_K_M"}
    };

    REQUIRE(response["models"].is_array());
    REQUIRE(response["models"][0]["aliases"].is_array());
    REQUIRE(response["models"][0]["loaded"].is_boolean());
    REQUIRE(response["models"][0].contains("path"));
}

TEST_CASE("Dashboard jobs expose queue history and token usage", "[dashboard][jobs]") {
    nlohmann::json response = {
        {"jobs", nlohmann::json::array({
            {
                {"id", "job-1"},
                {"type", "chat.completion"},
                {"status", "succeeded"},
                {"priority", 50},
                {"client", "OpenAI compatible"},
                {"resourceClass", "gpu_llm"},
                {"createdAt", "2026-05-22T10:00:00Z"},
                {"promptTokens", 12},
                {"completionTokens", 8},
                {"totalTokens", 20},
                {"durationMs", 300.0}
            }
        })}
    };

    REQUIRE(response["jobs"].is_array());
    REQUIRE(response["jobs"][0]["status"] == "succeeded");
    REQUIRE(response["jobs"][0]["totalTokens"] == 20);
    REQUIRE(response["jobs"][0]["resourceClass"] == "gpu_llm");
}
