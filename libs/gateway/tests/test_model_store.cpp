#include <catch2/catch_test_macros.hpp>

#include "gateway/model_store.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>

using namespace inferdeck;

namespace {

class FakeTransport : public gateway::IModelStoreTransport {
public:
    foundation::Result<nlohmann::json> get_json(
        const std::string& url, const std::string&) override {
        if (url.find("?search=") != std::string::npos) {
            return foundation::Ok(nlohmann::json::array({
                {{"id", "owner/text-GGUF"}, {"pipeline_tag", "text-generation"}, {"downloads", 12}},
                {{"id", "owner/image"}, {"pipeline_tag", "text-to-image"}, {"downloads", 4}}
            }));
        }
        if (url.find("owner/tts") != std::string::npos) {
            return foundation::Ok(nlohmann::json{
                {"sha", "revision"}, {"pipeline_tag", "text-to-speech"},
                {"siblings", nlohmann::json::array({
                    {{"rfilename", "model.onnx"},
                     {"lfs", {{"size", 4}, {"sha256", std::string(64, '1')}}}},
                    {{"rfilename", "tts.json"},
                     {"lfs", {{"size", 4}, {"sha256", std::string(64, '2')}}}},
                    {{"rfilename", "README.md"}, {"size", 5}}
                })}
            });
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

class RecordingTransport final : public FakeTransport {
public:
    foundation::Result<nlohmann::json> get_json(
        const std::string& url, const std::string& token) override {
        last_url = url;
        return FakeTransport::get_json(url, token);
    }

    std::string last_url;
};

std::filesystem::path test_root() {
    static std::atomic<std::uint64_t> suffix{0};
    return std::filesystem::temp_directory_path() /
           ("inferdeck-store-test-" + std::to_string(
               std::chrono::steady_clock::now().time_since_epoch().count()) +
            "-" + std::to_string(suffix.fetch_add(1)));
}

std::optional<gateway::StoreDownload> wait_for_terminal(
    gateway::ModelStore& store, std::uint64_t id) {
    for (int attempt = 0; attempt < 200; ++attempt) {
        for (const auto& job : store.downloads()) {
            if (job.id != id) continue;
            if (job.state == "failed" || job.state == "cancelled" ||
                job.state == "installed") {
                return job;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return std::nullopt;
}

class ThrowingTransport final : public FakeTransport {
public:
    foundation::Result<void> download(
        const std::string&, const std::string&, const std::filesystem::path&,
        std::uint64_t, const std::function<bool(std::uint64_t)>&) override {
        throw std::runtime_error("transport exploded");
    }
};

class OversizedTransport final : public FakeTransport {
public:
    foundation::Result<void> download(
        const std::string&, const std::string&,
        const std::filesystem::path& destination, std::uint64_t offset,
        const std::function<bool(std::uint64_t)>& progress) override {
        std::ofstream output(destination, std::ios::binary | std::ios::trunc);
        output << "too-long";
        output.close();
        progress_accepted.store(progress(offset + 8));
        return foundation::Err<void>(foundation::ErrorCode::Cancelled,
                                     "stopped");
    }

    std::atomic<bool> progress_accepted{true};
};

class BlockingTransport : public FakeTransport {
public:
    foundation::Result<void> download(
        const std::string&, const std::string&,
        const std::filesystem::path&, std::uint64_t offset,
        const std::function<bool(std::uint64_t)>& progress) override {
        {
            std::lock_guard lock(mutex_);
            ++active_;
        }
        changed_.notify_all();
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [this] { return released_; });
        lock.unlock();
        progress(offset);
        return foundation::Err<void>(foundation::ErrorCode::Cancelled,
                                     "released by test");
    }

    void wait_for_active(std::size_t count) {
        std::unique_lock lock(mutex_);
        REQUIRE(changed_.wait_for(
            lock, std::chrono::seconds(2),
            [this, count] { return active_ >= count; }));
    }

    void release() {
        {
            std::lock_guard lock(mutex_);
            released_ = true;
        }
        changed_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::size_t active_{0};
    bool released_{false};
};

class RacingTransport final : public BlockingTransport {
public:
    foundation::Result<nlohmann::json> get_json(
        const std::string& url, const std::string& token) override {
        if (url.find("?search=") != std::string::npos) {
            return FakeTransport::get_json(url, token);
        }
        std::unique_lock lock(inspect_mutex_);
        ++inspections_;
        inspect_changed_.notify_all();
        if (!inspect_changed_.wait_for(
                lock, std::chrono::seconds(2),
                [this] { return inspections_ >= 2; })) {
            return foundation::Err<nlohmann::json>(
                foundation::ErrorCode::Timeout,
                "test did not reach concurrent inspection");
        }
        lock.unlock();
        return FakeTransport::get_json(url, token);
    }

private:
    std::mutex inspect_mutex_;
    std::condition_variable inspect_changed_;
    std::size_t inspections_{0};
};

class RetryCollisionTransport final : public FakeTransport {
public:
    foundation::Result<void> download(
        const std::string&, const std::string&,
        const std::filesystem::path&, std::uint64_t,
        const std::function<bool(std::uint64_t)>&) override {
        if (calls_.fetch_add(1) == 0) {
            return foundation::Err<void>(
                foundation::ErrorCode::IoError, "first attempt failed");
        }
        {
            std::lock_guard lock(mutex_);
            active_ = true;
        }
        changed_.notify_all();
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [this] { return released_; });
        return foundation::Err<void>(
            foundation::ErrorCode::Cancelled, "released by test");
    }

    void wait_for_active() {
        std::unique_lock lock(mutex_);
        REQUIRE(changed_.wait_for(
            lock, std::chrono::seconds(2),
            [this] { return active_; }));
    }

    void release() {
        {
            std::lock_guard lock(mutex_);
            released_ = true;
        }
        changed_.notify_all();
    }

private:
    std::atomic<int> calls_{0};
    std::mutex mutex_;
    std::condition_variable changed_;
    bool active_{false};
    bool released_{false};
};

class SuccessfulTransport final : public FakeTransport {
public:
    foundation::Result<nlohmann::json> get_json(
        const std::string&, const std::string&) override {
        return foundation::Ok(nlohmann::json{
            {"sha", "revision"}, {"pipeline_tag", "text-generation"},
            {"siblings", nlohmann::json::array({
                {{"rfilename", "model.gguf"},
                 {"lfs", {{"size", 4},
                          {"sha256", "9f86d081884c7d659a2feaa0c55ad015"
                                     "a3bf4f1b2b0b822cd15d6c15b0f00a08"}}}}
            })}
        });
    }
};

}

TEST_CASE("Model store filters search results by runtime", "[model-store]") {
    model::ModelRegistry registry;
    model::BackendCoordinator coordinator(registry);
    const auto root = test_root();
    {
        gateway::ModelStore store(root, "", coordinator,
                                  std::make_unique<FakeTransport>());
        auto result = store.search("model", "stable_diffusion_cpp", "image");
        REQUIRE(result);
        REQUIRE(result->size() == 1);
        CHECK((*result)[0]["id"] == "owner/image");
        CHECK((*result)[0]["modality"] == "image");
    }
    std::filesystem::remove_all(root);
}

TEST_CASE("Model store ranks Hugging Face discovery by downloads",
          "[model-store][catalogue]") {
    model::ModelRegistry registry;
    model::BackendCoordinator coordinator(registry);
    const auto root = test_root();
    auto transport = std::make_unique<RecordingTransport>();
    auto* recording = transport.get();
    {
        gateway::ModelStore store(root, "", coordinator, std::move(transport));
        auto result = store.search("model", "", "");
        REQUIRE(result);
        CHECK(recording->last_url.find("sort=downloads") != std::string::npos);
        CHECK(recording->last_url.find("sort=lastModified") == std::string::npos);
    }
    std::filesystem::remove_all(root);
}

TEST_CASE("Model store library includes registered and unconfigured local artifacts",
          "[model-store][library]") {
    model::ModelRegistry registry;
    model::BackendCoordinator coordinator(registry);
    const auto root = test_root();
    std::filesystem::create_directories(root / "configured");
    std::filesystem::create_directories(root / "extra");
    {
        std::ofstream(root / "configured" / "model-Q4_K_M.gguf", std::ios::binary) << "configured";
        std::ofstream(root / "extra" / "alternate-Q8_0.gguf", std::ios::binary) << "alternate";
    }
    model::ModelInfo info;
    info.name = "configured-model";
    info.family = "test";
    info.gguf_path = (root / "configured" / "model-Q4_K_M.gguf").string();
    info.vram_required_mb = 1234;
    registry.register_model(info);
    {
        gateway::ModelStore store(root, "", coordinator,
                                  std::make_unique<FakeTransport>());
        const auto library = store.library();
        REQUIRE(library.is_array());
        const auto configured = std::find_if(
            library.begin(), library.end(), [](const auto& entry) {
                return entry.value("name", "") == "configured-model";
            });
        REQUIRE(configured != library.end());
        CHECK(configured->value("configured", false));
        CHECK(configured->value("quantization", "") == "q4_k_m");
        const auto extra = std::find_if(
            library.begin(), library.end(), [](const auto& entry) {
                return !entry.value("configured", true);
            });
        REQUIRE(extra != library.end());
        CHECK(extra->value("quantization", "") == "q8_0");
        CHECK_FALSE(extra->value("managed", true));
    }
    std::filesystem::remove_all(root);
}

TEST_CASE("Model store exposes only compatible verifiable artifacts", "[model-store]") {
    model::ModelRegistry registry;
    model::BackendCoordinator coordinator(registry);
    const auto root = test_root();
    {
        gateway::ModelStore store(root, "", coordinator,
                                  std::make_unique<FakeTransport>());
        auto result = store.inspect("owner/text-GGUF");
        REQUIRE(result);
        REQUIRE((*result)["files"].size() == 1);
        CHECK((*result)["files"][0]["name"] == "model.gguf");
        CHECK((*result)["files"][0]["size"] == 4);
        CHECK_FALSE(store.inspect("../outside"));
    }
    std::filesystem::remove_all(root);
}

TEST_CASE("Model store exposes sherpa neural TTS metadata artifacts",
          "[model-store][tts]") {
    model::ModelRegistry registry;
    model::BackendCoordinator coordinator(registry);
    const auto root = test_root();
    {
        gateway::ModelStore store(root, "", coordinator,
                                  std::make_unique<FakeTransport>());
        auto result = store.inspect("owner/tts");
        REQUIRE(result);
        REQUIRE((*result)["files"].size() == 2);
        CHECK((*result)["files"][0]["name"] == "model.onnx");
        CHECK((*result)["files"][0]["runtime"] == "sherpa_onnx");
        CHECK((*result)["files"][1]["name"] == "tts.json");
        CHECK((*result)["files"][1]["runtime"] == "sherpa_onnx");
    }
    std::filesystem::remove_all(root);
}

TEST_CASE("Model store never registers a corrupt artifact", "[model-store]") {
    model::ModelRegistry registry;
    model::BackendCoordinator coordinator(registry);
    const auto root = test_root();
    {
        gateway::ModelStore store(root, "", coordinator,
                                  std::make_unique<FakeTransport>());
        auto install = store.install("owner/text-GGUF", "model.gguf",
                                     "llama_cpp", "text", "safe-model");
        REQUIRE(install);
        const auto job = wait_for_terminal(store, *install);
        REQUIRE(job);
        CHECK(job->state == "failed");
        CHECK_FALSE(registry.has("safe-model"));
        CHECK_FALSE(store.install("owner/text-GGUF", "model.gguf",
                                  "llama_cpp", "text", "../escape"));
    }
    std::filesystem::remove_all(root);
}

TEST_CASE("Model store contains unexpected worker exceptions",
          "[model-store]") {
    model::ModelRegistry registry;
    model::BackendCoordinator coordinator(registry);
    const auto root = test_root();
    {
        gateway::ModelStore store(root, "", coordinator,
                                  std::make_unique<ThrowingTransport>());
        const auto install = store.install(
            "owner/text-GGUF", "model.gguf", "llama_cpp", "text",
            "throwing-model");
        REQUIRE(install);
        const auto job = wait_for_terminal(store, *install);
        REQUIRE(job);
        CHECK(job->state == "failed");
        CHECK(job->error.find("transport exploded") != std::string::npos);
        CHECK_FALSE(registry.has("throwing-model"));
    }
    std::filesystem::remove_all(root);
}

TEST_CASE("Model store reserves duplicate names atomically",
          "[model-store]") {
    model::ModelRegistry registry;
    model::BackendCoordinator coordinator(registry);
    const auto root = test_root();
    auto transport = std::make_unique<RacingTransport>();
    auto* control = transport.get();
    {
        gateway::ModelStore store(root, "", coordinator, std::move(transport));
        using InstallResult = foundation::Result<std::uint64_t>;
        std::optional<InstallResult> first;
        std::optional<InstallResult> second;
        std::thread left([&] {
            first = store.install("owner/text-GGUF", "model.gguf",
                                  "llama_cpp", "text", "same-name");
        });
        std::thread right([&] {
            second = store.install("owner/text-GGUF", "model.gguf",
                                   "llama_cpp", "text", "same-name");
        });
        left.join();
        right.join();

        REQUIRE(first);
        REQUIRE(second);
        const auto successes = static_cast<int>(first->has_value()) +
                               static_cast<int>(second->has_value());
        CHECK(successes == 1);
        const auto& rejected = first->has_value() ? *second : *first;
        REQUIRE_FALSE(rejected);
        CHECK(rejected.error().code ==
              foundation::ErrorCode::AlreadyExists);
        control->release();
    }
    std::filesystem::remove_all(root);
}

TEST_CASE("Model store rejects work above its active install limit",
          "[model-store]") {
    model::ModelRegistry registry;
    model::BackendCoordinator coordinator(registry);
    const auto root = test_root();
    auto transport = std::make_unique<BlockingTransport>();
    auto* control = transport.get();
    {
        gateway::ModelStore store(root, "", coordinator, std::move(transport));
        const auto first = store.install(
            "owner/text-GGUF", "model.gguf", "llama_cpp", "text", "one");
        REQUIRE(first);
        control->wait_for_active(1);
        const auto second = store.install(
            "owner/text-GGUF", "model.gguf", "llama_cpp", "text", "two");
        REQUIRE(second);
        control->wait_for_active(2);
        const auto third = store.install(
            "owner/text-GGUF", "model.gguf", "llama_cpp", "text", "three");
        REQUIRE_FALSE(third);
        CHECK(third.error().code == foundation::ErrorCode::Unavailable);
        control->release();
    }
    std::filesystem::remove_all(root);
}

TEST_CASE("Model store cannot resume into another job's name reservation",
          "[model-store]") {
    model::ModelRegistry registry;
    model::BackendCoordinator coordinator(registry);
    const auto root = test_root();
    auto transport = std::make_unique<RetryCollisionTransport>();
    auto* control = transport.get();
    {
        gateway::ModelStore store(root, "", coordinator, std::move(transport));
        const auto first = store.install(
            "owner/text-GGUF", "model.gguf", "llama_cpp", "text",
            "shared-name");
        REQUIRE(first);
        const auto failed = wait_for_terminal(store, *first);
        REQUIRE(failed);
        CHECK(failed->state == "failed");

        const auto second = store.install(
            "owner/text-GGUF", "model.gguf", "llama_cpp", "text",
            "shared-name");
        REQUIRE(second);
        control->wait_for_active();
        const auto resumed = store.resume(*first);
        REQUIRE_FALSE(resumed);
        CHECK(resumed.error().code ==
              foundation::ErrorCode::AlreadyExists);
        control->release();
    }
    std::filesystem::remove_all(root);
}

TEST_CASE("Model store aborts downloads exceeding validated size",
          "[model-store]") {
    model::ModelRegistry registry;
    model::BackendCoordinator coordinator(registry);
    const auto root = test_root();
    auto transport = std::make_unique<OversizedTransport>();
    auto* control = transport.get();
    {
        gateway::ModelStore store(root, "", coordinator, std::move(transport));
        const auto install = store.install(
            "owner/text-GGUF", "model.gguf", "llama_cpp", "text",
            "oversized-model");
        REQUIRE(install);
        const auto job = wait_for_terminal(store, *install);
        REQUIRE(job);
        CHECK(job->state == "failed");
        CHECK(job->error.find("exceeds") != std::string::npos);
        CHECK_FALSE(control->progress_accepted.load());
        CHECK_FALSE(std::filesystem::exists(
            root / "llama_cpp" / "owner_text-GGUF" /
            "model.gguf.partial"));
    }
    std::filesystem::remove_all(root);
}

TEST_CASE("Model store bounds retained terminal job history",
          "[model-store]") {
    model::ModelRegistry registry;
    model::BackendCoordinator coordinator(registry);
    const auto root = test_root();
    {
        gateway::ModelStore store(root, "", coordinator,
                                  std::make_unique<ThrowingTransport>());
        for (int index = 0; index < 105; ++index) {
            const auto install = store.install(
                "owner/text-GGUF", "model.gguf", "llama_cpp", "text",
                "failed-" + std::to_string(index));
            REQUIRE(install);
            REQUIRE(wait_for_terminal(store, *install));
        }
        CHECK(store.downloads().size() <= 100);
    }
    std::filesystem::remove_all(root);
}

#ifdef _WIN32
TEST_CASE("Model store serializes concurrent manifest updates",
          "[model-store]") {
    model::ModelRegistry registry;
    model::BackendCoordinator coordinator(registry);
    const auto root = test_root();
    {
        gateway::ModelStore store(root, "", coordinator,
                                  std::make_unique<SuccessfulTransport>());
        const auto first = store.install(
            "owner/first", "model.gguf", "llama_cpp", "text", "first");
        const auto second = store.install(
            "owner/second", "model.gguf", "llama_cpp", "text", "second");
        REQUIRE(first);
        REQUIRE(second);
        const auto first_job = wait_for_terminal(store, *first);
        const auto second_job = wait_for_terminal(store, *second);
        REQUIRE(first_job);
        REQUIRE(second_job);
        CHECK(first_job->state == "installed");
        CHECK(second_job->state == "installed");
        CHECK(registry.has("first"));
        CHECK(registry.has("second"));

        std::ifstream input(root / "installed.json", std::ios::binary);
        REQUIRE(input);
        const auto manifest = nlohmann::json::parse(input);
        CHECK(manifest.contains("first"));
        CHECK(manifest.contains("second"));
    }
    std::filesystem::remove_all(root);
}
#endif
