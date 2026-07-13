#include <catch2/catch_test_macros.hpp>

#include "gateway/model_store.hpp"

#include <chrono>
#include <fstream>
#include <thread>

using namespace inferdeck;

namespace {

class FakeTransport final : public gateway::IModelStoreTransport {
public:
    foundation::Result<nlohmann::json> get_json(
        const std::string& url, const std::string&) override {
        if (url.find("?search=") != std::string::npos) {
            return foundation::Ok(nlohmann::json::array({
                {{"id", "owner/text-GGUF"}, {"pipeline_tag", "text-generation"}, {"downloads", 12}},
                {{"id", "owner/image"}, {"pipeline_tag", "text-to-image"}, {"downloads", 4}}
            }));
        }
        return foundation::Ok(nlohmann::json{
            {"sha", "revision"}, {"pipeline_tag", "text-generation"},
            {"siblings", nlohmann::json::array({
                {{"rfilename", "model.gguf"},
                 {"lfs", {{"size", 4}, {"sha256", std::string(64, '0')}}}},
                {{"rfilename", "README.md"}, {"size", 5}}
            })}
        });
    }

    foundation::Result<void> download(
        const std::string&, const std::string&, const std::filesystem::path& destination,
        std::uint64_t offset, const std::function<bool(std::uint64_t)>& progress) override {
        std::ofstream output(destination, std::ios::binary |
                            (offset ? std::ios::app : std::ios::trunc));
        output << "test";
        output.close();
        if (!progress(offset + 4)) {
            return foundation::Err<void>(foundation::ErrorCode::Cancelled, "cancelled");
        }
        return foundation::Ok();
    }
};

std::filesystem::path test_root() {
    return std::filesystem::temp_directory_path() /
           ("inferdeck-store-test-" + std::to_string(
               std::chrono::steady_clock::now().time_since_epoch().count()));
}

}

TEST_CASE("Model store filters search results by runtime", "[model-store]") {
    model::ModelRegistry registry;
    model::BackendCoordinator coordinator(registry);
    const auto root = test_root();
    gateway::ModelStore store(root, "", coordinator, std::make_unique<FakeTransport>());

    auto result = store.search("model", "stable_diffusion_cpp", "image");
    REQUIRE(result);
    REQUIRE(result->size() == 1);
    CHECK((*result)[0]["id"] == "owner/image");
    CHECK((*result)[0]["modality"] == "image");

    std::filesystem::remove_all(root);
}

TEST_CASE("Model store exposes only compatible verifiable artifacts", "[model-store]") {
    model::ModelRegistry registry;
    model::BackendCoordinator coordinator(registry);
    const auto root = test_root();
    gateway::ModelStore store(root, "", coordinator, std::make_unique<FakeTransport>());

    auto result = store.inspect("owner/text-GGUF");
    REQUIRE(result);
    REQUIRE((*result)["files"].size() == 1);
    CHECK((*result)["files"][0]["name"] == "model.gguf");
    CHECK((*result)["files"][0]["size"] == 4);
    CHECK_FALSE(store.inspect("../outside"));

    std::filesystem::remove_all(root);
}

TEST_CASE("Model store never registers a corrupt artifact", "[model-store]") {
    model::ModelRegistry registry;
    model::BackendCoordinator coordinator(registry);
    const auto root = test_root();
    gateway::ModelStore store(root, "", coordinator, std::make_unique<FakeTransport>());

    auto install = store.install("owner/text-GGUF", "model.gguf", "llama_cpp", "text", "safe-model");
    REQUIRE(install);
    for (int attempt = 0; attempt < 100; ++attempt) {
        const auto jobs = store.downloads();
        if (!jobs.empty() && jobs[0].state == "failed") break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const auto jobs = store.downloads();
    REQUIRE(jobs.size() == 1);
    CHECK(jobs[0].state == "failed");
    CHECK_FALSE(registry.has("safe-model"));
    CHECK_FALSE(store.install("owner/text-GGUF", "model.gguf", "llama_cpp", "text", "../escape"));

    std::filesystem::remove_all(root);
}
