#include <benchmark/benchmark.h>
#include "llama_cpp/LlamaEngine.hpp"
#include "llama_cpp/GGUFParser.hpp"
#include "core/Metrics.hpp"
#include <nlohmann/json.hpp>

// Benchmark GGUF header parsing
static void BM_GGUFParser_ReadHeader(benchmark::State& state) {
    // Create mock GGUF data
    std::vector<uint8_t> mock_gguf(36);
    memcpy(mock_gguf.data(), "ggml", 4);  // Magic
    // ... rest would be set up

    for (auto _ : state) {
        // Simulate reading GGUF header
        auto magic = reinterpret_cast<char*>(mock_gguf.data());
        benchmark::DoNotOptimize(magic);
    }
}

// Benchmark JSON parsing for chat completion requests
static void BM_JsonParse_ChatCompletion(benchmark::State& state) {
    std::string json_str = R"({
        "model": "default",
        "messages": [
            {"role": "system", "content": "You are a helpful assistant."},
            {"role": "user", "content": "Hello, world!"}
        ],
        "max_tokens": 256,
        "temperature": 0.7,
        "top_p": 0.9,
        "stream": false
    })";

    for (auto _ : state) {
        auto j = nlohmann::json::parse(json_str);
        benchmark::DoNotOptimize(j);
    }
}

// Benchmark JSON serialization for chat completion responses
static void BM_JsonSerialize_ChatCompletion(benchmark::State& state) {
    nlohmann::json j;
    j["id"] = "chatcmpl-123";
    j["object"] = "chat.completion";
    j["created"] = 1234567890;
    j["model"] = "default";
    j["choices"] = nlohmann::json::array({
        {
            {"index", 0},
            {"message", nlohmann::json{{"role", "assistant"}, {"content", "Hello!"}}},
            {"finish_reason", "stop"}
        }
    });
    j["usage"] = {{"prompt_tokens", 10}, {"completion_tokens", 5}, {"total_tokens", 15}};

    for (auto _ : state) {
        std::string dumped = j.dump(2);
        benchmark::DoNotOptimize(dumped);
    }
}

// Benchmark InferenceParams validation
static void BM_InferenceParams_Validation(benchmark::State& state) {
    inferdeck::core::InferenceParams params;
    params.temperature = 0.7f;
    params.top_k = 40;
    params.top_p = 0.9f;
    params.max_tokens = 256;

    for (auto _ : state) {
        auto valid = params.Validate();
        benchmark::DoNotOptimize(valid);
    }
}

// Benchmark MetricsStore operations
static void BM_MetricsStore_Increment(benchmark::State& state) {
    auto& store = inferdeck::core::MetricsStore::Get();
    store.Reset();

    for (auto _ : state) {
        store.IncrementCounter("benchmark.counter", 1);
    }
}

// Register benchmarks
BENCHMARK(BM_GGUFParser_ReadHeader);
BENCHMARK(BM_JsonParse_ChatCompletion);
BENCHMARK(BM_JsonSerialize_ChatCompletion);
BENCHMARK(BM_InferenceParams_Validation);
BENCHMARK(BM_MetricsStore_Increment);

BENCHMARK_MAIN();
