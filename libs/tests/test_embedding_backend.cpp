/// @file test_embedding_backend.cpp
/// @brief Tests for embedding backend interface and mock implementation.

#include <catch2/catch_test_macros.hpp>
#include "backends/IEmbeddingBackend.hpp"
#include "backends/ITextBackend.hpp"
#include <memory>

namespace {

using namespace inferdeck::backends;

class MockEmbeddingBackend : public IEmbeddingBackend {
public:
    std::string GetBackendName() const override { return "mock_gte"; }
    BackendStatus GetStatus() const override { return status_; }
    bool Initialize() override { status_ = BackendStatus::READY; return true; }
    void Shutdown() override { status_ = BackendStatus::UNINITIALIZED; }
    nlohmann::json GetInfo() const override { return nlohmann::json{{"name", "mock_gte"}}; }
    nlohmann::json GetVRAMUsage() const override { return nlohmann::json{{"used", 2ULL << 30}}; }
    bool IsReady() const override { return status_ == BackendStatus::READY; }

    EmbeddingResult Generate(const std::string& input, const nlohmann::json&) override {
        EmbeddingResult result;
        result.embedding = std::vector<float>(768, 0.0f);
        result.token_count = static_cast<int>(input.length());
        result.inference_time_ms = 15.5;
        result.model_name = "gte-small";
        return result;
    }

    std::vector<EmbeddingResult> GenerateBatch(const std::vector<std::string>& inputs,
                                                const nlohmann::json&) override {
        std::vector<EmbeddingResult> results;
        for (const auto& input : inputs) {
            results.push_back(Generate(input, {}));
        }
        return results;
    }

    int GetEmbeddingDimension() const override { return 768; }
    nlohmann::json GetAvailableModels() const override {
        return nlohmann::json::array({"gte-small", "gte-base", "gte-large"});
    }

    BackendStatus status_ = BackendStatus::UNINITIALIZED;
};

} // namespace

TEST_CASE("Embedding: Mock backend name and status", "[embedding][mock]") {
    MockEmbeddingBackend backend;
    REQUIRE(backend.GetBackendName() == "mock_gte");
    REQUIRE(!backend.IsReady());
}

TEST_CASE("Embedding: Initialize and shutdown", "[embedding][mock]") {
    MockEmbeddingBackend backend;
    REQUIRE(backend.Initialize());
    REQUIRE(backend.IsReady());
    backend.Shutdown();
    REQUIRE(!backend.IsReady());
}

TEST_CASE("Embedding: Generate returns valid embedding", "[embedding][mock]") {
    MockEmbeddingBackend backend;
    backend.Initialize();

    auto result = backend.Generate("Hello world", {});
    REQUIRE(result.embedding.size() == 768);
    REQUIRE(result.token_count == 11); // length of "Hello world"
    REQUIRE(result.inference_time_ms == 15.5);
    REQUIRE(result.model_name == "gte-small");
}

TEST_CASE("Embedding: GenerateBatch processes multiple inputs", "[embedding][mock]") {
    MockEmbeddingBackend backend;
    backend.Initialize();

    std::vector<std::string> inputs = {"first", "second", "third"};
    auto results = backend.GenerateBatch(inputs, {});
    REQUIRE(results.size() == 3);
    for (const auto& r : results) {
        REQUIRE(r.embedding.size() == 768);
        REQUIRE(r.model_name == "gte-small");
    }
}

TEST_CASE("Embedding: GetEmbeddingDimension returns 768", "[embedding][mock]") {
    MockEmbeddingBackend backend;
    REQUIRE(backend.GetEmbeddingDimension() == 768);
}

TEST_CASE("Embedding: GetAvailableModels returns array", "[embedding][mock]") {
    MockEmbeddingBackend backend;
    auto models = backend.GetAvailableModels();
    REQUIRE(models.is_array());
    REQUIRE(models.size() == 3);
    REQUIRE(models[0] == "gte-small");
    REQUIRE(models[2] == "gte-large");
}

TEST_CASE("Embedding: Generate with empty input", "[embedding][mock]") {
    MockEmbeddingBackend backend;
    backend.Initialize();

    auto result = backend.Generate("", {});
    REQUIRE(result.embedding.size() == 768);
    REQUIRE(result.token_count == 0);
}

TEST_CASE("Embedding: GetInfo returns valid JSON", "[embedding][mock]") {
    MockEmbeddingBackend backend;
    auto info = backend.GetInfo();
    REQUIRE(info.is_object());
    REQUIRE(info.contains("name"));
    REQUIRE(info["name"] == "mock_gte");
}

TEST_CASE("Embedding: GetVRAMUsage returns bytes", "[embedding][mock]") {
    MockEmbeddingBackend backend;
    auto vram = backend.GetVRAMUsage();
    REQUIRE(vram.is_object());
    REQUIRE(vram.contains("used"));
    REQUIRE(vram["used"].get<uint64_t>() == (2ULL << 30));
}

TEST_CASE("Embedding: Cast to ITextBackend interface", "[embedding][mock]") {
    std::unique_ptr<ITextBackend> base = std::make_unique<MockEmbeddingBackend>();
    REQUIRE(base->Initialize());
    REQUIRE(base->IsReady());
    base->Shutdown();
}

TEST_CASE("Embedding: Cast to IEmbeddingBackend interface", "[embedding][mock]") {
    std::unique_ptr<IEmbeddingBackend> emb = std::make_unique<MockEmbeddingBackend>();
    REQUIRE(emb->GetBackendName() == "mock_gte");
    auto result = emb->Generate("test", {});
    REQUIRE(result.embedding.size() == 768);
    REQUIRE(emb->GetEmbeddingDimension() == 768);
}
