// Google Benchmarks for InferDeck backend components
// Run with: ./backend_benchmarks --benchmark_min_time=0.5s
// Measures: GGUF parsing, VRAM arbitration, VectorStore CRUD, JSON serialization, backend init/shutdown

#include <benchmark/benchmark.h>
#include <cstdint>
#include <string>
#include <vector>
#include <chrono>
#include <random>
#include <nlohmann/json.hpp>
#include "llama_cpp/GGUFParser.hpp"
#include "llama_cpp/LlamaEngine.hpp"
#include "core/Metrics.hpp"
#include "backends/GpuResourceManager.hpp"
#include "backends/BackendRegistry.hpp"
#include "vector_store/VectorStore.hpp"

namespace bm = benchmark;

// =============================================================================
// GGUF Parser Benchmarks
// =============================================================================

static std::vector<uint8_t> CreateMockGGUF(size_t metadata_count, size_t tensor_count) {
    std::vector<uint8_t> buf(36); // header size
    memcpy(buf.data(), "gguf", 4);
    buf[4] = 0x03; buf[5] = 0x00; buf[6] = 0x00; buf[7] = 0x00; // byte order LE
    uint32_t val32 = static_cast<uint32_t>(metadata_count);
    memcpy(buf.data() + 8, &val32, 4);
    memcpy(buf.data() + 12, &val32, 4);
    size_t offset = 36;
    for (uint32_t i = 0; i < metadata_count; ++i) {
        buf.resize(offset + 1); buf[offset++] = 3; // key type
        uint32_t key_len = 16;
        buf.resize(offset + 4); memcpy(buf.data() + offset, &key_len, 4); offset += 4;
        for (char c : std::string("metadata_key")) {
            buf.resize(offset + 1); buf[offset++] = static_cast<uint8_t>(c);
        }
        buf.resize(offset + 4); memcpy(buf.data() + offset, &val32, 4); offset += 4;
    }
    val32 = static_cast<uint32_t>(tensor_count);
    for (uint32_t i = 0; i < tensor_count; ++i) {
        buf.resize(offset + 4); memcpy(buf.data() + offset, &val32, 4); offset += 4;
        uint32_t name_len = 12;
        buf.resize(offset + 4); memcpy(buf.data() + offset, &name_len, 4); offset += 4;
        for (char c : std::string("tensor_name")) {
            buf.resize(offset + 1); buf[offset++] = static_cast<uint8_t>(c);
        }
        buf.resize(offset + 8); uint64_t dims[4] = {1024, 2048, 0, 0}; memcpy(buf.data() + offset, dims, 8); offset += 8;
        buf.resize(offset + 4); uint32_t ttype = 1; memcpy(buf.data() + offset, &ttype, 4); offset += 4;
    }
    return buf;
}

static void BM_GGUF_ParseHeader(bm::State& state) {
    auto data = CreateMockGGUF(64, 100);
    for (auto _ : state) {
        auto magic = reinterpret_cast<char*>(data.data());
        auto byte_order = *reinterpret_cast<uint32_t*>(data.data() + 4);
        auto metadata_count = *reinterpret_cast<uint32_t*>(data.data() + 8);
        auto tensor_count = *reinterpret_cast<uint32_t*>(data.data() + 12);
        bm::DoNotOptimize(magic);
        bm::DoNotOptimize(byte_order);
        bm::DoNotOptimize(metadata_count);
        bm::DoNotOptimize(tensor_count);
        bm::ClobberMemory();
    }
    state.SetBytesProcessed(state.iterations() * data.size());
}
BENCHMARK(BM_GGUF_ParseHeader);

static void BM_GGUF_ParseMetadataKV(bm::State& state) {
    auto data = CreateMockGGUF(256, 100);
    for (auto _ : state) {
        uint32_t meta_count = *reinterpret_cast<uint32_t*>(data.data() + 8);
        size_t offset = 36;
        for (uint32_t i = 0; i < meta_count; ++i) {
            uint32_t key_len = *reinterpret_cast<uint32_t*>(data.data() + offset);
            offset += 4;
            offset += key_len;
            offset += 4; // value
        }
        bm::DoNotOptimize(offset);
        bm::ClobberMemory();
    }
    state.SetBytesProcessed(state.iterations() * data.size());
}
BENCHMARK(BM_GGUF_ParseMetadataKV);

static void BM_GGUF_ParseTensors(bm::State& state) {
    auto data = CreateMockGGUF(64, 500);
    for (auto _ : state) {
        uint32_t tensor_count = *reinterpret_cast<uint32_t*>(data.data() + 12);
        size_t offset = 36;
        uint32_t meta_count = *reinterpret_cast<uint32_t*>(data.data() + 8);
        for (uint32_t i = 0; i < meta_count; ++i) {
            uint32_t key_len = *reinterpret_cast<uint32_t*>(data.data() + offset);
            offset += 4; offset += key_len; offset += 4;
        }
        for (uint32_t i = 0; i < tensor_count; ++i) {
            uint32_t name_len = *reinterpret_cast<uint32_t*>(data.data() + offset);
            offset += 4; offset += name_len;
            uint64_t dims[4]; memcpy(dims, data.data() + offset, 8); offset += 8;
            uint32_t ttype = *reinterpret_cast<uint32_t*>(data.data() + offset); offset += 4;
            bm::DoNotOptimize(dims[0]); bm::DoNotOptimize(ttype);
        }
        bm::ClobberMemory();
    }
    state.SetBytesProcessed(state.iterations() * data.size());
}
BENCHMARK(BM_GGUF_ParseTensors);

// =============================================================================
// JSON Serialization Benchmarks
// =============================================================================

static const char* LARGE_CHAT_JSON = R"({
    "model": "llama-3.1-8b-instruct",
    "messages": [
        {"role": "system", "content": "You are a helpful AI assistant with extensive knowledge across multiple domains. You should always provide accurate, well-structured responses."},
        {"role": "user", "content": "Explain the concept of transformer architectures in machine learning, including attention mechanisms, positional encoding, and the encoder-decoder structure. Provide concrete examples of how these components interact."},
        {"role": "assistant", "content": "Transformers use self-attention to process sequences in parallel. The key components are: (1) Multi-head attention which computes weighted averages of values, (2) Positional encoding to preserve sequence order, (3) Feed-forward layers for feature transformation. The encoder processes input while the decoder generates output autoregressively."},
        {"role": "user", "content": "Compare this approach to RNNs and LSTMs. What are the practical advantages and disadvantages?"},
        {"role": "assistant", "content": "Compared to RNNs, transformers offer: (1) Parallel training - no sequential dependency, (2) Better long-range dependencies via direct attention paths, (3) Improved scalability across GPUs. Disadvantages include: (1) O(n^2) attention complexity, (2) Large memory requirements for context windows, (3) Less efficient for streaming/inference at very long sequences."},
        {"role": "user", "content": "What about recent developments like FlashAttention and linear attention variants?"}
    ],
    "max_tokens": 2048,
    "temperature": 0.7,
    "top_p": 0.9,
    "stream": false,
    "frequency_penalty": 0.0,
    "presence_penalty": 0.0,
    "stop": ["\n"],
    "logit_bias": {}
})";

static void BM_JSON_ParseChatCompletion(bm::State& state) {
    nlohmann::json::parse(LARGE_CHAT_JSON);
    for (auto _ : state) {
        auto j = nlohmann::json::parse(LARGE_CHAT_JSON);
        bm::DoNotOptimize(j);
        bm::ClobberMemory();
    }
    state.SetBytesProcessed(state.iterations() * strlen(LARGE_CHAT_JSON));
}
BENCHMARK(BM_JSON_ParseChatCompletion);

static void BM_JSON_SerializeChatResponse(bm::State& state) {
    nlohmann::json result;
    result["id"] = "chatcmpl-123";
    result["object"] = "chat.completion";
    result["created"] = 1234567890;
    result["model"] = "llama-3.1-8b-instruct";
    result["choices"] = nlohmann::json::array({
        {
            "index", 0,
            "message", nlohmann::json{{"role", "assistant"}, {"content", "Transformers revolutionized NLP by enabling parallel processing of sequences. Unlike RNNs, attention mechanisms allow direct computation of dependencies regardless of distance."}},
            "finish_reason", "stop"
        }
    });
    result["usage"] = nlohmann::json{{"prompt_tokens", 512}, {"completion_tokens", 256}, {"total_tokens", 768}};
    std::string serialized;
    for (auto _ : state) {
        serialized = result.dump(2);
        bm::DoNotOptimize(serialized);
        bm::ClobberMemory();
    }
    state.SetBytesProcessed(state.iterations() * serialized.size());
}
BENCHMARK(BM_JSON_SerializeChatResponse);

static void BM_JSON_ParseEmbeddingRequest(bm::State& state) {
    const char* req = R"({"model": "gte-small", "input": ["This is a test sentence for embedding generation. It should be long enough to measure the parsing overhead accurately."], "encoding_format": "float", "dimensions": 384})";
    for (auto _ : state) {
        auto j = nlohmann::json::parse(req);
        bm::DoNotOptimize(j);
        bm::ClobberMemory();
    }
    state.SetBytesProcessed(state.iterations() * strlen(req));
}
BENCHMARK(BM_JSON_ParseEmbeddingRequest);

static void BM_JSON_SerializeEmbeddingResponse(bm::State& state) {
    nlohmann::json response;
    response["object"] = "list";
    response["model"] = "gte-small";
    response["usage"] = nlohmann::json{{"prompt_tokens", 10}, {"total_tokens", 10}};
    std::vector<float> embedding(384, 0.01f);
    response["data"] = nlohmann::json::array({
        {
            "object", "embedding",
            "index", 0,
            "embedding", embedding
        }
    });
    std::string serialized;
    for (auto _ : state) {
        serialized = response.dump(2);
        bm::DoNotOptimize(serialized);
        bm::ClobberMemory();
    }
    state.SetBytesProcessed(state.iterations() * serialized.size());
}
BENCHMARK(BM_JSON_SerializeEmbeddingResponse);

// =============================================================================
// MetricsStore Benchmarks
// =============================================================================

static void BM_Metrics_IncrementCounter(bm::State& state) {
    auto& store = inferdeck::core::MetricsStore::Get();
    store.Reset();
    for (auto _ : state) {
        store.IncrementCounter("test.counter", 1);
        bm::ClobberMemory();
    }
}
BENCHMARK(BM_Metrics_IncrementCounter);

static void BM_Metrics_SetGauge(bm::State& state) {
    auto& store = inferdeck::core::MetricsStore::Get();
    store.Reset();
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<> dis(0, 100);
    for (auto _ : state) {
        store.SetGauge("test.gauge", dis(gen));
        bm::ClobberMemory();
    }
}
BENCHMARK(BM_Metrics_SetGauge);

static void BM_Metrics_RecordHistogram(bm::State& state) {
    auto& store = inferdeck::core::MetricsStore::Get();
    store.Reset();
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<> dis(1.0, 1000.0);
    for (auto _ : state) {
        store.RecordHistogram("test.latency", dis(gen));
        bm::ClobberMemory();
    }
}
BENCHMARK(BM_Metrics_RecordHistogram);

static void BM_Metrics_ToJson(bm::State& state) {
    auto& store = inferdeck::core::MetricsStore::Get();
    store.Reset();
    store.IncrementCounter("http.requests.total", 1000);
    store.IncrementCounter("http.errors.total", 50);
    store.SetGauge("system.cpu.usage", 45.2);
    store.SetGauge("gpu.vram.usage", 67.8);
    store.RecordHistogram("inference.latency.ms", 150.5);
    store.RecordHistogram("inference.latency.ms", 250.3);
    store.RecordHistogram("inference.latency.ms", 75.1);
    for (auto _ : state) {
        auto j = store.ToJson();
        bm::DoNotOptimize(j);
        bm::ClobberMemory();
    }
}
BENCHMARK(BM_Metrics_ToJson);

static void BM_Metrics_PrintPrometheus(bm::State& state) {
    auto& store = inferdeck::core::MetricsStore::Get();
    store.Reset();
    store.IncrementCounter("http.requests.total", 1000);
    store.SetGauge("system.cpu.usage", 45.2);
    store.RecordHistogram("inference.latency.ms", 150.5);
    std::string output;
    for (auto _ : state) {
        store.PrintPrometheus(output);
        bm::DoNotOptimize(output);
        bm::ClobberMemory();
    }
}
BENCHMARK(BM_Metrics_PrintPrometheus);

// =============================================================================
// VectorStore Benchmarks
// =============================================================================

static void BM_VectorStore_AddDocument(bm::State& state) {
    inferdeck::vector_store::VectorStore store;
    store.Initialize(":memory:");
    std::string content(state.range(0), 'x');
    inferdeck::vector_store::DocumentRecord doc;
    doc.id = "doc_0";
    doc.title = "Test Document";
    doc.content = content;
    doc.embedding = std::vector<float>(384, 0.01f);
    doc.version = 1;
    doc.created_at = 1234567890.0;
    doc.updated_at = 1234567890.0;
    for (auto _ : state) {
        store.AddDocument(doc);
        bm::ClobberMemory();
    }
    store.Shutdown();
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_VectorStore_AddDocument)->Arg(100)->Arg(1000)->Arg(10000);

static void BM_VectorStore_GetDocument(bm::State& state) {
    inferdeck::vector_store::VectorStore store;
    store.Initialize(":memory:");
    for (int i = 0; i < 100; ++i) {
        inferdeck::vector_store::DocumentRecord doc;
        doc.id = "doc_" + std::to_string(i);
        doc.title = "Doc " + std::to_string(i);
        doc.content = std::string(100, 'x');
        doc.embedding = std::vector<float>(384, 0.01f);
        doc.version = 1;
        doc.created_at = 1234567890.0;
        doc.updated_at = 1234567890.0;
        store.AddDocument(doc);
    }
    for (auto _ : state) {
        auto result = store.GetDocument("doc_50");
        bm::DoNotOptimize(result);
        bm::ClobberMemory();
    }
    store.Shutdown();
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_VectorStore_GetDocument);

static void BM_VectorStore_UpdateDocument(bm::State& state) {
    inferdeck::vector_store::VectorStore store;
    store.Initialize(":memory:");
    inferdeck::vector_store::DocumentRecord doc;
    doc.id = "doc_update";
    doc.title = "Original";
    doc.content = std::string(1000, 'x');
    doc.embedding = std::vector<float>(384, 0.01f);
    doc.version = 1;
    doc.created_at = 1234567890.0;
    doc.updated_at = 1234567890.0;
    store.AddDocument(doc);
    for (auto _ : state) {
        doc.title = "Updated";
        doc.version = 2;
        doc.updated_at = 1234567891.0;
        bool success = store.UpdateDocument("doc_update", doc);
        bm::DoNotOptimize(success);
        bm::ClobberMemory();
    }
    store.Shutdown();
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_VectorStore_UpdateDocument);

static void BM_VectorStore_DeleteDocument(bm::State& state) {
    inferdeck::vector_store::VectorStore store;
    store.Initialize(":memory:");
    for (int i = 0; i < 100; ++i) {
        inferdeck::vector_store::DocumentRecord doc;
        doc.id = "doc_del_" + std::to_string(i);
        doc.title = "Doc";
        doc.content = "content";
        doc.embedding = std::vector<float>(384, 0.01f);
        doc.version = 1;
        doc.created_at = 1234567890.0;
        doc.updated_at = 1234567890.0;
        store.AddDocument(doc);
    }
    for (auto _ : state) {
        bool deleted = store.DeleteDocument("doc_del_50");
        bm::DoNotOptimize(deleted);
        bm::ClobberMemory();
    }
    store.Shutdown();
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_VectorStore_DeleteDocument);

static void BM_VectorStore_SearchText(bm::State& state) {
    inferdeck::vector_store::VectorStore store;
    store.Initialize(":memory:");
    for (int i = 0; i < 200; ++i) {
        inferdeck::vector_store::DocumentRecord doc;
        doc.id = "search_doc_" + std::to_string(i);
        doc.title = "Document " + std::to_string(i) + " about machine learning";
        doc.content = "This document contains information about transformers, neural networks, and deep learning concepts. Item " + std::to_string(i);
        doc.embedding = std::vector<float>(384, static_cast<float>(i) * 0.001f);
        doc.version = 1;
        doc.created_at = 1234567890.0;
        doc.updated_at = 1234567890.0;
        store.AddDocument(doc);
    }
    for (auto _ : state) {
        auto results = store.Search("machine learning", 10);
        bm::DoNotOptimize(results);
        bm::ClobberMemory();
    }
    store.Shutdown();
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_VectorStore_SearchText);

static void BM_VectorStore_SearchByEmbedding(bm::State& state) {
    inferdeck::vector_store::VectorStore store;
    store.Initialize(":memory:");
    std::mt19937 gen(42);
    std::uniform_real_distribution<> dist(0.0, 1.0);
    for (int i = 0; i < 200; ++i) {
        inferdeck::vector_store::DocumentRecord doc;
        doc.id = "emb_doc_" + std::to_string(i);
        doc.title = "Doc";
        doc.content = "content";
        doc.embedding.resize(384);
        for (auto& v : doc.embedding) v = dist(gen);
        doc.version = 1;
        doc.created_at = 1234567890.0;
        doc.updated_at = 1234567890.0;
        store.AddDocument(doc);
    }
    std::vector<float> query(384);
    for (auto& v : query) v = dist(gen);
    for (auto _ : state) {
        auto results = store.SearchByEmbedding(query, 10);
        bm::DoNotOptimize(results);
        bm::ClobberMemory();
    }
    store.Shutdown();
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_VectorStore_SearchByEmbedding);

static void BM_VectorStore_CosineSimilarity(bm::State& state) {
    std::vector<float> a(512, 0.5f);
    std::vector<float> b(512, 0.5f);
    for (auto _ : state) {
        double sim = inferdeck::vector_store::VectorStore::ComputeCosineSimilarity(a, b);
        bm::DoNotOptimize(sim);
        bm::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_VectorStore_CosineSimilarity);

static void BM_VectorStore_ListDocuments(bm::State& state) {
    inferdeck::vector_store::VectorStore store;
    store.Initialize(":memory:");
    for (int i = 0; i < state.range(0); ++i) {
        inferdeck::vector_store::DocumentRecord doc;
        doc.id = "list_doc_" + std::to_string(i);
        doc.title = "Doc";
        doc.content = "content";
        doc.embedding = std::vector<float>(384, 0.01f);
        doc.version = 1;
        doc.created_at = 1234567890.0;
        doc.updated_at = 1234567890.0;
        store.AddDocument(doc);
    }
    for (auto _ : state) {
        auto ids = store.ListDocuments();
        bm::DoNotOptimize(ids);
        bm::ClobberMemory();
    }
    store.Shutdown();
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_VectorStore_ListDocuments)->Arg(100)->Arg(500)->Arg(1000);

static void BM_VectorStore_Count(bm::State& state) {
    inferdeck::vector_store::VectorStore store;
    store.Initialize(":memory:");
    for (int i = 0; i < 500; ++i) {
        inferdeck::vector_store::DocumentRecord doc;
        doc.id = "count_doc_" + std::to_string(i);
        doc.title = "Doc";
        doc.content = "content";
        doc.embedding = std::vector<float>(384, 0.01f);
        doc.version = 1;
        doc.created_at = 1234567890.0;
        doc.updated_at = 1234567890.0;
        store.AddDocument(doc);
    }
    for (auto _ : state) {
        int count = store.GetCount();
        bm::DoNotOptimize(count);
        bm::ClobberMemory();
    }
    store.Shutdown();
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_VectorStore_Count);

// =============================================================================
// GpuResourceManager Benchmarks
// =============================================================================

static void BM_GpuResource_AcquireRelease(bm::State& state) {
    auto& mgr = inferdeck::backends::GpuResourceManager::Instance();
    mgr.Initialize(0);
    std::string name = "benchmark_backend";
    uint64_t vram = 1ULL << 30; // 1GB
    for (auto _ : state) {
        mgr.AcquireVRAM(name, vram, inferdeck::backends::TEXT_GENERATION);
        mgr.ReleaseVRAM(name, vram);
        bm::ClobberMemory();
    }
    mgr.ReleaseVRAM(name, vram);
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_GpuResource_AcquireRelease);

static void BM_GpuResource_Snapshot(bm::State& state) {
    auto& mgr = inferdeck::backends::GpuResourceManager::Instance();
    mgr.Initialize(0);
    mgr.AcquireVRAM("test1", 1ULL << 30, inferdeck::backends::TEXT_GENERATION);
    mgr.AcquireVRAM("test2", 512ULL << 20, inferdeck::backends::EMBEDDING);
    mgr.AcquireVRAM("test3", 256ULL << 20, inferdeck::backends::TEXT_TO_SPEECH);
    for (auto _ : state) {
        auto snap = mgr.GetSnapshot();
        bm::DoNotOptimize(snap);
        bm::ClobberMemory();
    }
    mgr.ReleaseVRAM("test1", 1ULL << 30);
    mgr.ReleaseVRAM("test2", 512ULL << 20);
    mgr.ReleaseVRAM("test3", 256ULL << 20);
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_GpuResource_Snapshot);

static void BM_GpuResource_GetBackendVRAM(bm::State& state) {
    auto& mgr = inferdeck::backends::GpuResourceManager::Instance();
    mgr.Initialize(0);
    mgr.AcquireVRAM("bench1", 1ULL << 30, inferdeck::backends::TEXT_GENERATION);
    mgr.AcquireVRAM("bench2", 512ULL << 20, inferdeck::backends::EMBEDDING);
    for (auto _ : state) {
        auto vram = mgr.GetBackendVRAM("bench1");
        bm::DoNotOptimize(vram);
        bm::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_GpuResource_GetBackendVRAM);

static void BM_GpuResource_ExclusiveLock(bm::State& state) {
    auto& mgr = inferdeck::backends::GpuResourceManager::Instance();
    mgr.Initialize(0);
    std::string name = "training_backend";
    uint64_t vram = 2ULL << 30;
    for (auto _ : state) {
        mgr.LockExclusiveVRAM(name, vram, inferdeck::backends::FINE_TUNING);
        mgr.UnlockExclusiveVRAM(name);
        bm::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_GpuResource_ExclusiveLock);

static void BM_GpuResource_UsagePercent(bm::State& state) {
    auto& mgr = inferdeck::backends::GpuResourceManager::Instance();
    mgr.Initialize(0);
    mgr.AcquireVRAM("load1", 1ULL << 30, inferdeck::backends::TEXT_GENERATION);
    for (auto _ : state) {
        double pct = mgr.GetVRAMUsagePercent();
        bm::DoNotOptimize(pct);
        bm::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_GpuResource_UsagePercent);

static void BM_GpuResource_DeviceInfo(bm::State& state) {
    auto& mgr = inferdeck::backends::GpuResourceManager::Instance();
    mgr.Initialize(0);
    for (auto _ : state) {
        auto info = mgr.GetDeviceInfo();
        bm::DoNotOptimize(info);
        bm::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_GpuResource_DeviceInfo);

static void BM_GpuResource_ActiveBackendCount(bm::State& state) {
    auto& mgr = inferdeck::backends::GpuResourceManager::Instance();
    mgr.Initialize(0);
    mgr.AcquireVRAM("b1", 1ULL << 30, inferdeck::backends::TEXT_GENERATION);
    mgr.AcquireVRAM("b2", 512ULL << 20, inferdeck::backends::EMBEDDING);
    for (auto _ : state) {
        int count = mgr.GetActiveBackendCount();
        bm::DoNotOptimize(count);
        bm::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_GpuResource_ActiveBackendCount);

// =============================================================================
// BackendRegistry Benchmarks
// =============================================================================

static void BM_BackendRegistry_Initialize(bm::State& state) {
    auto& reg = inferdeck::backends::BackendRegistry::Instance();
    for (auto _ : state) {
        reg.Initialize();
        reg.Shutdown();
        bm::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_BackendRegistry_Initialize);

static void BM_BackendRegistry_Shutdown(bm::State& state) {
    auto& reg = inferdeck::backends::BackendRegistry::Instance();
    reg.Shutdown();
    for (auto _ : state) {
        reg.Shutdown();
        bm::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_BackendRegistry_Shutdown);

// =============================================================================
// InferenceParams Benchmarks
// =============================================================================

static void BM_InferenceParams_Validate(bm::State& state) {
    inferdeck::core::InferenceParams params;
    params.temperature = 0.7f;
    params.top_k = 40;
    params.top_p = 0.9f;
    params.max_tokens = 256;
    for (auto _ : state) {
        auto valid = params.Validate();
        bm::DoNotOptimize(valid);
        bm::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_InferenceParams_Validate);

static void BM_InferenceParams_DefaultValues(bm::State& state) {
    inferdeck::core::InferenceParams params;
    for (auto _ : state) {
        auto t = params.temperature;
        auto tk = params.top_k;
        auto tp = params.top_p;
        auto mt = params.max_tokens;
        bm::DoNotOptimize(t); bm::DoNotOptimize(tk);
        bm::DoNotOptimize(tp); bm::DoNotOptimize(mt);
        bm::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_InferenceParams_DefaultValues);

// =============================================================================
// VRAM Priority Enum Benchmarks
// =============================================================================

static void BM_VRamPriority_ToString(bm::State& state) {
    auto& mgr = inferdeck::backends::GpuResourceManager::Instance();
    std::vector<inferdeck::backends::VRamPriority> priorities = {
        inferdeck::backends::SYSTEM_HEALTH,
        inferdeck::backends::TEXT_GENERATION,
        inferdeck::backends::SPEECH_TO_TEXT,
        inferdeck::backends::TEXT_TO_SPEECH,
        inferdeck::backends::EMBEDDING,
        inferdeck::backends::DOCUMENT_QUERY,
        inferdeck::backends::DOCUMENT_INDEX,
        inferdeck::backends::FINE_TUNING,
        inferdeck::backends::IMAGE_GENERATION
    };
    for (auto _ : state) {
        for (auto p : priorities) {
            auto str = mgr.PriorityToString(p);
            bm::DoNotOptimize(str);
            bm::ClobberMemory();
        }
    }
    state.SetItemsProcessed(state.iterations() * priorities.size());
}
BENCHMARK(BM_VRamPriority_ToString);

// =============================================================================
// Audio Processing Benchmarks
// =============================================================================

static void BM_Audio_PathValidation(bm::State& state) {
    std::string path = "/path/to/audio/file.wav";
    for (auto _ : state) {
        auto ext = path.substr(path.find_last_of('.') + 1);
        auto valid = (ext == "wav" || ext == "mp3" || ext == "flac" || ext == "ogg" || ext == "m4a");
        bm::DoNotOptimize(valid);
        bm::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Audio_PathValidation);

static void BM_Audio_TranscriptionResponse(bm::State& state) {
    nlohmann::json result;
    result["object"] = "transcription";
    result["text"] = std::string(4096, 'a');
    result["duration"] = 60.5;
    result["model"] = "whisper-large-v3";
    result["language"] = "en";
    result["segments"] = nlohmann::json::array({
        {"id", 0, "start", 0.0, "end", 30.0, "text", "first half of the audio content"},
        {"id", 1, "start", 30.0, "end", 60.5, "text", "second half of the audio content"}
    });
    std::string serialized;
    for (auto _ : state) {
        serialized = result.dump(2);
        bm::DoNotOptimize(serialized);
        bm::ClobberMemory();
    }
    state.SetBytesProcessed(state.iterations() * serialized.size());
}
BENCHMARK(BM_Audio_TranscriptionResponse);

static void BM_Audio_TTS_SegmentText(bm::State& state) {
    std::string text = "This is a long passage of text that will be segmented into smaller chunks for text-to-speech synthesis. Each segment should be a natural sentence or clause that can be spoken coherently. The segmentation algorithm should handle punctuation, abbreviations, and various sentence structures correctly.";
    for (auto _ : state) {
        std::vector<std::string> segments;
        std::string current;
        for (char c : text) {
            if (c == '.' || c == '!' || c == '?' || c == '\n') {
                current += c;
                if (!current.empty()) { segments.push_back(current); current.clear(); }
                continue;
            }
            current += c;
        }
        if (!current.empty()) segments.push_back(current);
        bm::DoNotOptimize(segments.size());
        bm::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Audio_TTS_SegmentText);

static void BM_Audio_TTS_Response(bm::State& state) {
    nlohmann::json result;
    result["object"] = "speech";
    result["model"] = "piper-en";
    result["voice"] = "amy";
    result["format"] = "wav";
    result["duration_ms"] = 5000;
    result["sample_rate"] = 22050;
    std::string serialized;
    for (auto _ : state) {
        serialized = result.dump(2);
        bm::DoNotOptimize(serialized);
        bm::ClobberMemory();
    }
    state.SetBytesProcessed(state.iterations() * serialized.size());
}
BENCHMARK(BM_Audio_TTS_Response);

// =============================================================================
// Image Generation Benchmarks
// =============================================================================

static void BM_Image_PromptProcessing(bm::State& state) {
    std::string prompt = "A beautiful sunset over the ocean with palm trees silhouetted against the sky, warm golden lighting, photorealistic style, 4K resolution";
    for (auto _ : state) {
        auto neg = "blurry, low quality, distorted, watermark, text";
        auto width = 512;
        auto height = 512;
        auto steps = 30;
        bm::DoNotOptimize(width); bm::DoNotOptimize(height);
        bm::DoNotOptimize(steps); bm::DoNotOptimize(neg);
        bm::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Image_PromptProcessing);

static void BM_Image_GenerationResponse(bm::State& state) {
    nlohmann::json result;
    result["object"] = "list";
    result["data"] = nlohmann::json::array({
        {
            "url", "https://example.com/generated_image.png",
            "b64_json", std::string(1000000, 'A'),
            "revised_prompt", "Revised prompt for better quality"
        }
    });
    std::string serialized;
    for (auto _ : state) {
        serialized = result.dump(2);
        bm::DoNotOptimize(serialized);
        bm::ClobberMemory();
    }
    state.SetBytesProcessed(state.iterations() * serialized.size());
}
BENCHMARK(BM_Image_GenerationResponse);

static void BM_Image_CheckpointList(bm::State& state) {
    nlohmann::json checkpoints;
    for (int i = 0; i < 50; ++i) {
        checkpoints.push_back(nlohmann::json{
            {"id", "checkpoint_" + std::to_string(i)},
            {"name", "sd-model-v" + std::to_string(i)},
            {"size_mb", 2048 + i * 10},
            {"quantization", "f16"},
            {"version", "1.0." + std::to_string(i)}
        });
    }
    std::string serialized;
    for (auto _ : state) {
        serialized = checkpoints.dump(2);
        bm::DoNotOptimize(serialized);
        bm::ClobberMemory();
    }
    state.SetBytesProcessed(state.iterations() * serialized.size());
}
BENCHMARK(BM_Image_CheckpointList);

// =============================================================================
// Document CRUD Benchmarks
// =============================================================================

static void BM_Documents_AddDocument(bm::State& state) {
    nlohmann::json doc;
    doc["id"] = "doc_" + std::to_string(state.range(0));
    doc["title"] = "Test Document Title";
    doc["content"] = std::string(5000, 'x');
    doc["embedding"] = std::vector<float>(384, 0.01f);
    doc["metadata"] = nlohmann::json{{"source", "test"}, {"tags", nlohmann::json::array({"a", "b", "c"})}};
    doc["version"] = 1;
    doc["created_at"] = 1234567890.0;
    doc["updated_at"] = 1234567890.0;
    std::string serialized;
    for (auto _ : state) {
        serialized = doc.dump(2);
        bm::DoNotOptimize(serialized);
        bm::ClobberMemory();
    }
    state.SetBytesProcessed(state.iterations() * serialized.size());
}
BENCHMARK(BM_Documents_AddDocument)->Arg(1)->Arg(100)->Arg(1000);

static void BM_Documents_SearchResponse(bm::State& state) {
    nlohmann::json response;
    response["object"] = "list";
    response["data"] = nlohmann::json::array();
    for (int i = 0; i < 20; ++i) {
        nlohmann::json r;
        r["document"] = nlohmann::json{
            {"id", "search_doc_" + std::to_string(i)},
            {"title", "Relevant Document " + std::to_string(i)},
            {"content", "This document is highly relevant to the search query with matching keywords and context."},
            {"metadata", nlohmann::json{{"source", "file_" + std::to_string(i)}}},
            {"version", 1},
            {"created_at", 1234567890.0},
            {"updated_at", 1234567891.0}
        };
        r["similarity"] = 0.95 - i * 0.02;
        response["data"].push_back(r);
    }
    std::string serialized;
    for (auto _ : state) {
        serialized = response.dump(2);
        bm::DoNotOptimize(serialized);
        bm::ClobberMemory();
    }
    state.SetBytesProcessed(state.iterations() * serialized.size());
}
BENCHMARK(BM_Documents_SearchResponse);

// =============================================================================
// Fine-Tuning Benchmarks
// =============================================================================

static void BM_FineTuning_CreateResponse(bm::State& state) {
    std::string job_id = "ft_20260518_001";
    nlohmann::json result;
    result["id"] = job_id;
    result["object"] = "fine_tuning.job";
    result["model"] = "llama-3.1-8b-instruct";
    result["status"] = "queued";
    result["created_at"] = std::time(nullptr);
    result["finished_at"] = 0;
    result["result_files"] = nlohmann::json::array();
    result["hyperparameters"] = nlohmann::json{{"n_epochs", 3}, {"learning_rate_multiplier", 0.0001f}};
    result["training_file"] = "/data/training.jsonl";
    result["trained_tokens"] = 0;
    result["validation_file"] = "/data/validation.jsonl";
    result["integrations"] = nlohmann::json::array();
    result["seed"] = 0;
    result["suffix"] = "custom-lora";
    std::string serialized;
    for (auto _ : state) {
        serialized = result.dump(2);
        bm::DoNotOptimize(serialized);
        bm::ClobberMemory();
    }
    state.SetBytesProcessed(state.iterations() * serialized.size());
}
BENCHMARK(BM_FineTuning_CreateResponse);

static void BM_FineTuning_ListResponse(bm::State& state) {
    nlohmann::json response;
    response["object"] = "list";
    response["data"] = nlohmann::json::array();
    response["has_more"] = false;
    for (int i = 0; i < 50; ++i) {
        nlohmann::json job;
        job["id"] = "ft_20260518_" + std::to_string(i);
        job["object"] = "fine_tuning.job";
        job["model"] = "llama-3.1-8b-instruct";
        job["status"] = (i < 30) ? "succeeded" : "running";
        job["created_at"] = 1234567890 + i * 100;
        job["finished_at"] = (i < 30) ? 1234567890 + i * 100 + 3600 : 0;
        job["result_files"] = nlohmann::json::array();
        job["hyperparameters"] = nlohmann::json{{"n_epochs", 3}};
        job["training_file"] = "/data/train.jsonl";
        job["trained_tokens"] = (i < 30) ? 1000000 : 0;
        response["data"].push_back(job);
    }
    std::string serialized;
    for (auto _ : state) {
        serialized = response.dump(2);
        bm::DoNotOptimize(serialized);
        bm::ClobberMemory();
    }
    state.SetBytesProcessed(state.iterations() * serialized.size());
}
BENCHMARK(BM_FineTuning_ListResponse);

// =============================================================================
// SSE Event Formatting Benchmarks
// =============================================================================

static void BM_SSE_FormatEvent(bm::State& state) {
    std::string text = "This is a chunk of text from the LLM response stream. It contains multiple sentences and punctuation that need to be properly formatted as SSE events. The format should include event type, data field, and proper line breaks.";
    for (auto _ : state) {
        std::string sse = "event: message\ndata: {\"text\": \"" + text + "\"}\n\n";
        bm::DoNotOptimize(sse);
        bm::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SSE_FormatEvent);

static void BM_SSE_StreamResponse(bm::State& state) {
    std::vector<std::string> chunks = {
        "Hello,", " ", "world", "!", " ",
        "This", " ", "is", " ", "a", " ", "streamed", " ", "response", ".", " ",
        "Each", " ", "word", " ", "is", " ", "a", " ", "separate", " ", "chunk", "."
    };
    for (auto _ : state) {
        for (const auto& chunk : chunks) {
            std::string sse = "event: message\ndata: {\"text\": \"" + chunk + "\"}\n\n";
            bm::ClobberMemory();
        }
    }
    state.SetItemsProcessed(state.iterations() * chunks.size());
}
BENCHMARK(BM_SSE_StreamResponse);

BENCHMARK_MAIN();
