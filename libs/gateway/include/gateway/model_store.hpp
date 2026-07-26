#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "foundation/result.hpp"
#include "model/backend_coordinator.hpp"

namespace inferdeck::gateway {

struct StoreFile {
    std::string repo;
    std::string revision;
    std::string name;
    std::string sha256;
    std::uint64_t size{0};
    std::string runtime;
    std::string modality;
    std::vector<std::string> capabilities;
};

struct StoreDownload {
    std::uint64_t id{0};
    StoreFile file;
    std::string model_name;
    std::string state{"queued"};
    std::string error;
    std::uint64_t bytes_downloaded{0};
    std::uint64_t bytes_total{0};
    std::string installed_path;
};

class IModelStoreTransport {
public:
    virtual ~IModelStoreTransport() = default;
    virtual foundation::Result<nlohmann::json> get_json(
        const std::string& url, const std::string& token) = 0;
    virtual foundation::Result<void> download(
        const std::string& url, const std::string& token,
        const std::filesystem::path& destination, std::uint64_t offset,
        const std::function<bool(std::uint64_t)>& progress) = 0;
};

std::unique_ptr<IModelStoreTransport> make_native_model_store_transport();

class ModelStore {
public:
    ModelStore(std::filesystem::path root, std::string token,
               model::BackendCoordinator& coordinator,
               std::unique_ptr<IModelStoreTransport> transport = {});
    ~ModelStore();

    foundation::Result<nlohmann::json> search(
        const std::string& query, const std::string& runtime,
        const std::string& modality, int limit = 20);
    foundation::Result<nlohmann::json> inspect(const std::string& repo);
    foundation::Result<std::uint64_t> install(
        const std::string& repo, const std::string& filename,
        const std::string& runtime, const std::string& modality,
        const std::string& model_name);
    foundation::Result<void> cancel(std::uint64_t id);
    foundation::Result<void> resume(std::uint64_t id);
    foundation::Result<void> remove(const std::string& model_name);

    [[nodiscard]] std::vector<StoreDownload> downloads() const;
    [[nodiscard]] nlohmann::json installed() const;
    [[nodiscard]] nlohmann::json library() const;

private:
    foundation::Result<StoreFile> resolve_file(
        const std::string& repo, const std::string& filename,
        const std::string& runtime, const std::string& modality);
    foundation::Result<void> start(std::uint64_t id);
    void worker_entry(std::uint64_t id,
                      const std::shared_ptr<std::atomic<bool>>& done) noexcept;
    void run(std::uint64_t id);
    void fail_job(std::uint64_t id, std::string error) noexcept;
    void finish_job(std::uint64_t id, std::string state, std::string error = {});
    void reap_completed_workers();
    void prune_completed_jobs_locked();
    void load_manifest();
    foundation::Result<void> save_manifest();

    static constexpr std::size_t kMaxActiveInstalls = 2;
    static constexpr std::size_t kMaxRetainedJobs = 100;

    std::filesystem::path root_;
    std::string token_;
    model::BackendCoordinator& coordinator_;
    std::unique_ptr<IModelStoreTransport> transport_;
    mutable std::mutex mutex_;
    std::unordered_map<std::uint64_t, StoreDownload> downloads_;
    std::unordered_map<std::uint64_t, std::shared_ptr<std::atomic<bool>>> cancellations_;
    std::unordered_map<std::uint64_t, std::thread> workers_;
    std::unordered_map<std::uint64_t, std::shared_ptr<std::atomic<bool>>> worker_done_;
    std::unordered_map<std::string, std::uint64_t> reserved_names_;
    nlohmann::json installed_{nlohmann::json::object()};
    std::mutex manifest_mutex_;
    std::uint64_t next_id_{1};
};

nlohmann::json to_json(const StoreDownload& download);

}
