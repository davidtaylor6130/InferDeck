#include <catch2/catch_test_macros.hpp>

#include "gateway/model_store.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
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

std::optional<std::string> environment_value(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || !value) {
        return std::nullopt;
    }
    std::string result(value);
    std::free(value);
    return result;
#else
    const char* value = std::getenv(name);
    return value ? std::optional<std::string>(value) : std::nullopt;
#endif
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

std::optional<gateway::StoreQuantization> wait_for_quantization_terminal(
    gateway::ModelStore& store, std::uint64_t id) {
    for (int attempt = 0; attempt < 400; ++attempt) {
        for (const auto& job : store.quantizations()) {
            if (job.id != id) continue;
            if (job.state == "failed" || job.state == "installed") {
                return job;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return std::nullopt;
}

class RecordingQuantizer final : public gateway::IModelQuantizer {
public:
    foundation::Result<void> quantize(
        const std::filesystem::path& source,
        const std::filesystem::path& destination,
        const std::string& quantization,
        int threads) override {
        {
            std::lock_guard lock(mutex_);
            source_ = source;
            destination_ = destination;
            quantization_ = quantization;
            threads_ = threads;
        }
        std::ofstream output(destination, std::ios::binary | std::ios::trunc);
        output << "quantized";
        return output
            ? foundation::Ok()
            : foundation::Err<void>(foundation::ErrorCode::IoError,
                                    "fake quantizer could not write output");
    }

    [[nodiscard]] std::string quantization() const {
        std::lock_guard lock(mutex_);
        return quantization_;
    }

    [[nodiscard]] int threads() const {
        std::lock_guard lock(mutex_);
        return threads_;
    }

    [[nodiscard]] std::filesystem::path source() const {
        std::lock_guard lock(mutex_);
        return source_;
    }

    [[nodiscard]] std::filesystem::path destination() const {
        std::lock_guard lock(mutex_);
        return destination_;
    }

private:
    mutable std::mutex mutex_;
    std::filesystem::path source_;
    std::filesystem::path destination_;
    std::string quantization_;
    int threads_{0};
};

class FailingQuantizer final : public gateway::IModelQuantizer {
public:
    foundation::Result<void> quantize(
        const std::filesystem::path&, const std::filesystem::path&,
        const std::string&, int) override {
        return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                     "source is not high precision");
    }
};

class BlockingQuantizer final : public gateway::IModelQuantizer {
public:
    foundation::Result<void> quantize(
        const std::filesystem::path&, const std::filesystem::path& destination,
        const std::string&, int) override {
        {
            std::lock_guard lock(mutex_);
            active_ = true;
        }
        changed_.notify_all();
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [this] { return released_; });
        lock.unlock();
        std::ofstream output(destination, std::ios::binary | std::ios::trunc);
        output << "quantized";
        return output
            ? foundation::Ok()
            : foundation::Err<void>(foundation::ErrorCode::IoError,
                                    "fake quantizer could not write output");
    }

    [[nodiscard]] bool wait_for_active() {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(
            lock, std::chrono::seconds(2), [this] { return active_; });
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
    bool active_{false};
    bool released_{false};
};

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

class SherpaBundleTransport : public FakeTransport {
public:
    foundation::Result<nlohmann::json> get_json(
        const std::string&, const std::string&) override {
        const std::string checksum = "9f86d081884c7d659a2feaa0c55ad015"
                                     "a3bf4f1b2b0b822cd15d6c15b0f00a08";
        return foundation::Ok(nlohmann::json{
            {"sha", "revision"}, {"pipeline_tag", "automatic-speech-recognition"},
            {"siblings", nlohmann::json::array({
                {{"rfilename", "encoder.onnx"}, {"lfs", {{"size", 4}, {"sha256", checksum}}}},
                {{"rfilename", "decoder.onnx"}, {"lfs", {{"size", 4}, {"sha256", checksum}}}},
                {{"rfilename", "joiner.onnx"}, {"lfs", {{"size", 4}, {"sha256", checksum}}}},
                {{"rfilename", "config/tokens.txt"}, {"lfs", {{"size", 4}, {"sha256", checksum}}}}
            })}
        });
    }
};

class AceStepBundleTransport final : public FakeTransport {
public:
    foundation::Result<nlohmann::json> get_json(
        const std::string& url, const std::string&) override {
        if (url.find("?search=") != std::string::npos) {
            return foundation::Ok(nlohmann::json::array({
                {
                    {"id", "Serveurperso/ACE-Step-1.5-GGUF"},
                    {"pipeline_tag", "text-to-audio"},
                    {"tags", nlohmann::json::array({"ace-step", "gguf"})},
                    {"downloads", 100},
                },
            }));
        }
        const std::string checksum = "9f86d081884c7d659a2feaa0c55ad015"
                                     "a3bf4f1b2b0b822cd15d6c15b0f00a08";
        return foundation::Ok(nlohmann::json{
            {"sha", "revision"}, {"pipeline_tag", "text-to-audio"},
            {"tags", nlohmann::json::array({"ace-step", "gguf"})},
            {"siblings", nlohmann::json::array({
                {{"rfilename", "Qwen3-Embedding-0.6B-BF16.gguf"},
                 {"lfs", {{"size", 4}, {"sha256", checksum}}}},
                {{"rfilename", "Qwen3-Embedding-0.6B-Q8_0.gguf"},
                 {"lfs", {{"size", 4}, {"sha256", checksum}}}},
                {{"rfilename", "acestep-5Hz-lm-0.6B-Q8_0.gguf"},
                 {"lfs", {{"size", 4}, {"sha256", checksum}}}},
                {{"rfilename", "acestep-v15-turbo-Q4_K_M.gguf"},
                 {"lfs", {{"size", 4}, {"sha256", checksum}}}},
                {{"rfilename", "acestep-v15-turbo-Q8_0.gguf"},
                 {"lfs", {{"size", 4}, {"sha256", checksum}}}},
                {{"rfilename", "vae-BF16.gguf"},
                 {"lfs", {{"size", 4}, {"sha256", checksum}}}},
            })}
        });
    }
};

class IncompleteSherpaTransport final : public SherpaBundleTransport {
public:
    foundation::Result<nlohmann::json> get_json(
        const std::string&, const std::string&) override {
        const std::string checksum = "9f86d081884c7d659a2feaa0c55ad015"
                                     "a3bf4f1b2b0b822cd15d6c15b0f00a08";
        return foundation::Ok(nlohmann::json{
            {"sha", "revision"}, {"pipeline_tag", "automatic-speech-recognition"},
            {"siblings", nlohmann::json::array({
                {{"rfilename", "encoder.onnx"}, {"lfs", {{"size", 4}, {"sha256", checksum}}}},
                {{"rfilename", "decoder.onnx"}, {"lfs", {{"size", 4}, {"sha256", checksum}}}},
                {{"rfilename", "tokens.txt"}, {"lfs", {{"size", 4}, {"sha256", checksum}}}}
            })}
        });
    }
};

class CorruptSherpaTransport final : public SherpaBundleTransport {
public:
    foundation::Result<void> download(
        const std::string&, const std::string&,
        const std::filesystem::path& destination, std::uint64_t offset,
        const std::function<bool(std::uint64_t)>& progress) override {
        std::ofstream output(destination, std::ios::binary |
                            (offset ? std::ios::app : std::ios::trunc));
        output << "xxxx";
        output.close();
        progress(offset + 4);
        return foundation::Ok();
    }
};

class ResumableSherpaTransport final : public SherpaBundleTransport {
public:
    foundation::Result<void> download(
        const std::string&, const std::string&,
        const std::filesystem::path& destination, std::uint64_t offset,
        const std::function<bool(std::uint64_t)>& progress) override {
        if (!interrupted_.exchange(true)) {
            std::ofstream output(destination, std::ios::binary | std::ios::trunc);
            output << "te";
            output.close();
            progress(2);
            {
                std::lock_guard lock(mutex_);
                waiting_ = true;
            }
            changed_.notify_all();
            std::unique_lock lock(mutex_);
            changed_.wait(lock, [this] { return released_; });
            return foundation::Err<void>(foundation::ErrorCode::Cancelled,
                                         "interrupted by test");
        }
        std::ofstream output(destination, std::ios::binary |
                            (offset ? std::ios::app : std::ios::trunc));
        const std::string bytes = offset == 2 ? "st" : "test";
        output << bytes;
        output.close();
        if (!progress(offset + bytes.size())) {
            return foundation::Err<void>(foundation::ErrorCode::Cancelled,
                                         "cancelled");
        }
        return foundation::Ok();
    }

    void wait_until_interrupted() {
        std::unique_lock lock(mutex_);
        REQUIRE(changed_.wait_for(lock, std::chrono::seconds{2},
                                  [this] { return waiting_; }));
    }

    void release() {
        {
            std::lock_guard lock(mutex_);
            released_ = true;
        }
        changed_.notify_all();
    }

private:
    std::atomic<bool> interrupted_{false};
    std::mutex mutex_;
    std::condition_variable changed_;
    bool waiting_{false};
    bool released_{false};
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

TEST_CASE("Model store discovers and installs a verified ACE-Step bundle",
          "[model-store][ace-step]") {
    model::ModelRegistry registry;
    model::BackendCoordinator coordinator(registry);
    const auto root = test_root();
    {
        gateway::ModelStore store(root, "", coordinator,
                                  std::make_unique<AceStepBundleTransport>());
        const auto search = store.search(
            "ACE-Step", "ace_step_cpp", "audio_generation");
        REQUIRE(search);
        REQUIRE(search->size() == 1);
        CHECK((*search)[0]["runtime"] == "ace_step_cpp");
        CHECK((*search)[0]["modality"] == "audio_generation");

        const auto inspected =
            store.inspect("Serveurperso/ACE-Step-1.5-GGUF");
        REQUIRE(inspected);
        const std::string selected_bundle =
            "__inferdeck_ace_step_bundle__:acestep-v15-turbo-Q4_K_M.gguf";
        const auto bundle = std::find_if(
            inspected->at("files").begin(), inspected->at("files").end(),
            [&selected_bundle](const auto& file) {
                return file.value("name", "") == selected_bundle;
            });
        REQUIRE(bundle != inspected->at("files").end());
        CHECK(bundle->value("compatible", false));
        CHECK(bundle->value("artifactCount", 0) == 3);
        CHECK(bundle->value("variant", "") ==
              "acestep-v15-turbo-Q4_K_M.gguf");
        CHECK(std::count_if(
                  inspected->at("files").begin(),
                  inspected->at("files").end(),
                  [](const auto& file) {
                      return file.value("format", "") == "bundle" &&
                             file.value("runtime", "") == "ace_step_cpp";
                  }) == 2);
        CHECK_FALSE(store.install(
            "Serveurperso/ACE-Step-1.5-GGUF",
            "acestep-v15-turbo-Q4_K_M.gguf", "ace_step_cpp",
            "audio_generation", "unsafe-single-file"));
        CHECK_FALSE(store.install(
            "Serveurperso/ACE-Step-1.5-GGUF",
            "__inferdeck_ace_step_bundle__:acestep-v15-base-Q4_K_M.gguf",
            "ace_step_cpp", "audio_generation", "missing-variant"));

        const auto install = store.install(
            "Serveurperso/ACE-Step-1.5-GGUF",
            selected_bundle, "ace_step_cpp",
            "audio_generation", "ace-step-bundle");
        REQUIRE(install);
        const auto job = wait_for_terminal(store, *install);
        REQUIRE(job);
        CHECK(job->state == "installed");
        const auto info = registry.get_info_result("ace-step-bundle");
        REQUIRE(info);
        CHECK(info->runtime == "ace_step_cpp");
        CHECK(info->modality == "audio_generation");
        CHECK(info->artifacts.contains("text_encoder"));
        CHECK(info->artifacts.contains("dit"));
        CHECK(info->artifacts.contains("vae"));
        CHECK(info->artifacts.size() == 3);
        CHECK(std::filesystem::path(info->artifacts.at("text_encoder"))
                  .filename().string() ==
              "Qwen3-Embedding-0.6B-Q8_0.gguf");
        CHECK(std::filesystem::path(info->artifacts.at("dit"))
                  .filename().string() ==
              "acestep-v15-turbo-Q4_K_M.gguf");
        CHECK(info->vram_required_mb == 1);
    }
    std::filesystem::remove_all(root);
}

TEST_CASE("Model store installs verified sherpa repositories as one atomic bundle",
          "[model-store][sherpa]") {
    model::ModelRegistry registry;
    model::BackendCoordinator coordinator(registry);
    const auto root = test_root();
    {
        gateway::ModelStore store(root, "", coordinator,
                                  std::make_unique<SherpaBundleTransport>());
        const auto inspected = store.inspect("owner/bundle");
        REQUIRE(inspected);
        const auto bundle = std::find_if(inspected->at("files").begin(),
                                         inspected->at("files").end(),
            [](const auto& file) {
                return file.value("name", "") == "__inferdeck_sherpa_bundle__";
            });
        REQUIRE(bundle != inspected->at("files").end());
        CHECK(bundle->value("compatible", false));
        CHECK(bundle->value("artifactCount", 0) == 4);

        const auto install = store.install(
            "owner/bundle", "__inferdeck_sherpa_bundle__", "sherpa_onnx",
            "audio_transcription", "parakeet-bundle");
        REQUIRE(install);
        const auto job = wait_for_terminal(store, *install);
        REQUIRE(job);
        CHECK(job->state == "installed");
        const auto info = registry.get_info_result("parakeet-bundle");
        REQUIRE(info);
        CHECK(info->artifacts.contains("encoder"));
        CHECK(info->artifacts.contains("decoder"));
        CHECK(info->artifacts.contains("joiner"));
        CHECK(info->artifacts.contains("tokens"));
        CHECK(std::filesystem::is_directory(job->installed_path));
        CHECK_FALSE(std::filesystem::exists(
            std::filesystem::path(job->installed_path).string() + ".bundle.partial"));
    }
    std::filesystem::remove_all(root);
}

TEST_CASE("Model store rejects incomplete and corrupt sherpa bundles",
          "[model-store][sherpa]") {
    model::ModelRegistry registry;
    model::BackendCoordinator coordinator(registry);
    const auto root = test_root();
    {
        gateway::ModelStore incomplete(root / "incomplete", "", coordinator,
                                       std::make_unique<IncompleteSherpaTransport>());
        const auto inspected = incomplete.inspect("owner/incomplete");
        REQUIRE(inspected);
        const auto bundle = std::find_if(inspected->at("files").begin(),
                                         inspected->at("files").end(), [](const auto& file) {
            return file.value("name", "") == "__inferdeck_sherpa_bundle__";
        });
        REQUIRE(bundle != inspected->at("files").end());
        CHECK_FALSE(bundle->value("compatible", true));
        CHECK_FALSE(incomplete.install(
            "owner/incomplete", "__inferdeck_sherpa_bundle__", "sherpa_onnx",
            "audio_transcription", "incomplete-model"));

        gateway::ModelStore corrupt(root / "corrupt", "", coordinator,
                                    std::make_unique<CorruptSherpaTransport>());
        const auto install = corrupt.install(
            "owner/corrupt", "__inferdeck_sherpa_bundle__", "sherpa_onnx",
            "audio_transcription", "corrupt-model");
        REQUIRE(install);
        const auto job = wait_for_terminal(corrupt, *install);
        REQUIRE(job);
        CHECK(job->state == "failed");
        CHECK_FALSE(registry.has("corrupt-model"));
        CHECK_FALSE(std::filesystem::exists(
            root / "corrupt" / "sherpa_onnx" / "owner_corrupt" /
            "corrupt-model.bundle.partial"));
    }
    std::filesystem::remove_all(root);
}

TEST_CASE("Model store resumes a cancelled sherpa bundle as one job",
          "[model-store][sherpa]") {
    model::ModelRegistry registry;
    model::BackendCoordinator coordinator(registry);
    const auto root = test_root();
    auto transport = std::make_unique<ResumableSherpaTransport>();
    auto* resumable = transport.get();
    {
        gateway::ModelStore store(root, "", coordinator, std::move(transport));
        const auto install = store.install(
            "owner/resume", "__inferdeck_sherpa_bundle__", "sherpa_onnx",
            "audio_transcription", "resumed-model");
        REQUIRE(install);
        resumable->wait_until_interrupted();
        REQUIRE(store.cancel(*install));
        resumable->release();
        const auto cancelled = wait_for_terminal(store, *install);
        REQUIRE(cancelled);
        REQUIRE(cancelled->state == "cancelled");
        REQUIRE(store.resume(*install));
        const auto installed = wait_for_terminal(store, *install);
        REQUIRE(installed);
        CHECK(installed->state == "installed");
        CHECK(registry.has("resumed-model"));
        CHECK(std::filesystem::is_directory(installed->installed_path));
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
        REQUIRE((*result)["files"].size() == 3);
        CHECK((*result)["files"][0]["name"] == "model.onnx");
        CHECK((*result)["files"][0]["runtime"] == "sherpa_onnx");
        CHECK((*result)["files"][1]["name"] == "tts.json");
        CHECK((*result)["files"][1]["runtime"] == "sherpa_onnx");
        CHECK((*result)["files"][2]["name"] == "__inferdeck_sherpa_bundle__");
        CHECK_FALSE((*result)["files"][2]["compatible"]);
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

#ifdef _WIN32
TEST_CASE("Native quantizer produces a real GGUF artifact",
          "[.][post-training][native-quantizer]") {
    const auto source_value = environment_value("INFERDECK_QUANTIZATION_SOURCE");
    const auto output_value = environment_value("INFERDECK_QUANTIZATION_OUTPUT");
    if (!source_value || !output_value) {
        SKIP("set INFERDECK_QUANTIZATION_SOURCE and INFERDECK_QUANTIZATION_OUTPUT");
    }
    const std::filesystem::path source(*source_value);
    const std::filesystem::path output(*output_value);
    REQUIRE(std::filesystem::is_regular_file(source));
    REQUIRE_FALSE(std::filesystem::exists(output));

    const auto quantizer = gateway::make_native_model_quantizer();
    REQUIRE(quantizer);
    const auto result = quantizer->quantize(source, output, "Q8_0", 1);
    if (!result) INFO(result.error().message);
    REQUIRE(result);
    REQUIRE(std::filesystem::is_regular_file(output));
    CHECK(std::filesystem::file_size(output) > 0);

    std::ifstream input(output, std::ios::binary);
    std::array<char, 4> signature{};
    input.read(signature.data(), static_cast<std::streamsize>(signature.size()));
    REQUIRE(input.gcount() == static_cast<std::streamsize>(signature.size()));
    CHECK(std::string(signature.data(), signature.size()) == "GGUF");
}

TEST_CASE("Model store quantizes a managed GGUF without overwriting its source",
          "[model-store][post-training]") {
    model::ModelRegistry registry;
    model::BackendCoordinator coordinator(registry);
    const auto root = test_root();
    auto quantizer = std::make_unique<RecordingQuantizer>();
    auto* recording = quantizer.get();
    {
        gateway::ModelStore store(
            root, "", coordinator, std::make_unique<SuccessfulTransport>(),
            std::move(quantizer));
        const auto install = store.install(
            "owner/source", "model.gguf", "llama_cpp", "text", "source-model");
        REQUIRE(install);
        const auto source_job = wait_for_terminal(store, *install);
        REQUIRE(source_job);
        REQUIRE(source_job->state == "installed");
        const auto source_path = std::filesystem::path(source_job->installed_path);

        const auto started = store.quantize(
            "source-model", "source-model-q4", "Q4_K_M", 3);
        REQUIRE(started);
        const auto job = wait_for_quantization_terminal(store, *started);
        REQUIRE(job);
        INFO(job->error);
        REQUIRE(job->state == "installed");
        CHECK(job->quantization == "q4_k_m");
        CHECK(job->output_size == 9);
        CHECK(job->output_sha256.size() == 64);
        CHECK(std::filesystem::exists(source_path));
        CHECK(std::filesystem::exists(job->output_path));
        CHECK_FALSE(std::filesystem::exists(job->output_path + ".partial"));
        CHECK(registry.has("source-model"));
        CHECK(registry.has("source-model-q4"));
        CHECK(recording->source() == source_path);
        CHECK(recording->destination().extension() == ".partial");
        CHECK(recording->quantization() == "q4_k_m");
        CHECK(recording->threads() == 3);
        const auto manifest = store.installed();
        REQUIRE(manifest.contains("source-model-q4"));
        CHECK(manifest["source-model-q4"]["sourceModel"] == "source-model");
        CHECK(manifest["source-model-q4"]["quantization"] == "q4_k_m");
    }
    std::filesystem::remove_all(root);
}

TEST_CASE("Model store cleans failed quantizations and releases the output name",
          "[model-store][post-training]") {
    model::ModelRegistry registry;
    model::BackendCoordinator coordinator(registry);
    const auto root = test_root();
    std::atomic<gateway::ComputeResource> maintenance_resource{
        gateway::ComputeResource::None};
    {
        gateway::ModelStore store(
            root, "", coordinator, std::make_unique<SuccessfulTransport>(),
            std::make_unique<FailingQuantizer>(), &maintenance_resource);
        CHECK_FALSE(store.quantize("missing", "output", "Q4_K_M"));
        const auto install = store.install(
            "owner/source", "model.gguf", "llama_cpp", "text", "source-model");
        REQUIRE(install);
        const auto source_job = wait_for_terminal(store, *install);
        REQUIRE(source_job);
        REQUIRE(source_job->state == "installed");
        CHECK_FALSE(store.quantize(
            "source-model", "source-model", "Q4_K_M"));
        CHECK_FALSE(store.quantize(
            "source-model", "unsafe/name", "Q4_K_M"));
        CHECK_FALSE(store.quantize(
            "source-model", "output-model", "IQ1_M"));

        const auto first = store.quantize(
            "source-model", "output-model", "Q8_0");
        REQUIRE(first);
        const auto failed = wait_for_quantization_terminal(store, *first);
        REQUIRE(failed);
        CHECK(failed->state == "failed");
        CHECK(failed->error == "source is not high precision");
        CHECK(maintenance_resource.load() == gateway::ComputeResource::None);
        CHECK_FALSE(registry.has("output-model"));
        CHECK_FALSE(store.installed().contains("output-model"));
        CHECK_FALSE(std::filesystem::exists(
            root / "llama_cpp" / "quantized" / "output-model"));

        const auto retry = store.quantize(
            "source-model", "output-model", "Q8_0");
        REQUIRE(retry);
        REQUIRE(wait_for_quantization_terminal(store, *retry));
    }
    std::filesystem::remove_all(root);
}

TEST_CASE("Model store owns CPU maintenance while quantization is active",
          "[model-store][post-training][background-lease]") {
    model::ModelRegistry registry;
    model::BackendCoordinator coordinator(registry);
    const auto root = test_root();
    std::atomic<gateway::ComputeResource> maintenance_resource{
        gateway::ComputeResource::None};
    auto quantizer = std::make_unique<BlockingQuantizer>();
    auto* control = quantizer.get();
    {
        gateway::ModelStore store(
            root, "", coordinator, std::make_unique<SuccessfulTransport>(),
            std::move(quantizer), &maintenance_resource);
        const auto install = store.install(
            "owner/source", "model.gguf", "llama_cpp", "text", "source-model");
        REQUIRE(install);
        const auto source_job = wait_for_terminal(store, *install);
        REQUIRE(source_job);
        REQUIRE(source_job->state == "installed");

        maintenance_resource.store(gateway::ComputeResource::Gpu);
        const auto occupied = store.quantize(
            "source-model", "blocked-output", "Q8_0");
        REQUIRE_FALSE(occupied);
        CHECK(occupied.error().code == foundation::ErrorCode::Unavailable);
        CHECK(maintenance_resource.load() == gateway::ComputeResource::Gpu);
        maintenance_resource.store(gateway::ComputeResource::None);

        const auto started = store.quantize(
            "source-model", "quantized-output", "Q8_0");
        REQUIRE(started);
        const bool became_active = control->wait_for_active();
        CHECK(became_active);
        CHECK(maintenance_resource.load() == gateway::ComputeResource::Cpu);
        control->release();
        const auto completed = wait_for_quantization_terminal(store, *started);
        REQUIRE(completed);
        INFO(completed->error);
        CHECK(completed->state == "installed");
        CHECK(maintenance_resource.load() == gateway::ComputeResource::None);
    }
    std::filesystem::remove_all(root);
}
#endif

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
        INFO("first install error: " << first_job->error);
        INFO("second install error: " << second_job->error);
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

TEST_CASE("Model store archives and permanently deletes managed artifacts",
          "[model-store][retire]") {
    model::ModelRegistry registry;
    model::BackendCoordinator coordinator(registry);
    const auto root = test_root();
    const auto archive_root = root / "archive";
    {
        gateway::ModelStore store(root / "store", archive_root, "", coordinator,
                                  std::make_unique<SuccessfulTransport>());
        const auto archived_install = store.install(
            "owner/archive", "model.gguf", "llama_cpp", "text", "archived-model");
        REQUIRE(archived_install);
        const auto archived_job = wait_for_terminal(store, *archived_install);
        REQUIRE(archived_job);
        REQUIRE(archived_job->state == "installed");
        const auto archived_source = std::filesystem::path(archived_job->installed_path);
        REQUIRE(store.archive("archived-model"));
        CHECK_FALSE(std::filesystem::exists(archived_source));
        CHECK(std::filesystem::exists(
            archive_root / "archived-model" / archived_source.filename()));
        CHECK_FALSE(registry.has("archived-model"));
        CHECK_FALSE(store.installed().contains("archived-model"));

        const auto deleted_install = store.install(
            "owner/delete", "model.gguf", "llama_cpp", "text", "deleted-model");
        REQUIRE(deleted_install);
        const auto deleted_job = wait_for_terminal(store, *deleted_install);
        REQUIRE(deleted_job);
        REQUIRE(deleted_job->state == "installed");
        const auto deleted_source = std::filesystem::path(deleted_job->installed_path);
        REQUIRE(store.remove("deleted-model"));
        CHECK_FALSE(std::filesystem::exists(deleted_source));
        CHECK_FALSE(registry.has("deleted-model"));
        CHECK_FALSE(store.installed().contains("deleted-model"));
    }
    std::filesystem::remove_all(root);
}
