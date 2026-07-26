#include "gateway/model_store.hpp"

#include "foundation/path_utils.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <stdexcept>
#include <sstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#endif

namespace inferdeck::gateway {

using foundation::ErrorCode;
using foundation::Err;
using foundation::Ok;
using foundation::Result;

namespace {

std::string encode(const std::string& value) {
    std::ostringstream output;
    output << std::hex << std::uppercase;
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') output << c;
        else output << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(c);
    }
    return output.str();
}

std::string safe_name(const std::string& value) {
    std::string output;
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.') output += static_cast<char>(c);
        else output += '_';
    }
    while (!output.empty() && output.front() == '.') output.erase(output.begin());
    return output.empty() ? "model" : output;
}

bool valid_repo(const std::string& repo) {
    const auto slash = repo.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= repo.size()) return false;
    if (repo.find("..") != std::string::npos || repo.find('/', slash + 1) != std::string::npos) return false;
    return std::all_of(repo.begin(), repo.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '/';
    });
}

bool compatible_extension(const std::string& filename, const std::string& runtime) {
    std::string extension = std::filesystem::path(filename).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (runtime == "llama_cpp") return extension == ".gguf";
    if (runtime == "stable_diffusion_cpp") return extension == ".safetensors" || extension == ".gguf" ||
                                                    extension == ".ckpt" || extension == ".pth" || extension == ".pt";
    if (runtime == "whisper_cpp") return extension == ".bin" || extension == ".gguf";
    if (runtime == "sherpa_onnx") {
        return extension == ".onnx" || extension == ".bin" ||
               extension == ".txt" || extension == ".json";
    }
    return false;
}

std::vector<std::string> capabilities_for(const std::string& runtime,
                                           const std::string& modality) {
    if (modality == "embedding") return {"embeddings"};
    if (runtime == "stable_diffusion_cpp") return {"image_generation"};
    if (runtime == "whisper_cpp") return {"audio_transcription"};
    if (runtime == "sherpa_onnx") {
        return {modality == "audio_transcription" ? "audio_transcription" : "audio_speech"};
    }
    return {"chat_completions", "responses"};
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string infer_runtime(const std::string& filename, const std::string& pipeline) {
    const std::string name = lower(filename);
    const std::string tag = lower(pipeline);
    if (name.ends_with(".onnx") || name.ends_with(".ort")) return "sherpa_onnx";
    if (tag.find("text-to-image") != std::string::npos || name.ends_with(".safetensors")) return "stable_diffusion_cpp";
    if (tag.find("automatic-speech-recognition") != std::string::npos) return "whisper_cpp";
    if (tag.find("text-to-speech") != std::string::npos) return "sherpa_onnx";
    return "llama_cpp";
}

std::string infer_modality(const std::string& runtime, const std::string& pipeline) {
    if (runtime == "stable_diffusion_cpp") return "image";
    if (runtime == "whisper_cpp") return "audio_transcription";
    if (runtime == "sherpa_onnx") {
        return lower(pipeline).find("automatic-speech-recognition") != std::string::npos
            ? "audio_transcription"
            : "audio_speech";
    }
    if (lower(pipeline).find("feature-extraction") != std::string::npos) return "embedding";
    return "text";
}

std::string infer_quantization(const std::string& filename) {
    const std::string name = lower(filename);
    for (const std::string& quantization : {
             "q2_k", "q3_k_s", "q3_k_m", "q3_k_l", "q4_0", "q4_1", "q4_k_s",
             "q4_k_m", "q5_0", "q5_1", "q5_k_s", "q5_k_m", "q6_k", "q8_0",
             "iq2_xxs", "iq2_xs", "iq3_xxs", "iq3_s", "iq4_xs", "f16", "bf16"}) {
        if (name.find(quantization) != std::string::npos) return quantization;
    }
    return "unknown";
}

std::uint64_t local_file_size(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    return error ? 0 : size;
}

Result<void> replace_file(const std::filesystem::path& source,
                          const std::filesystem::path& destination) {
#ifdef _WIN32
    if (!MoveFileExA(source.string().c_str(), destination.string().c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return Err<void>(ErrorCode::IoError, "cannot finalize model artifact");
    }
#else
    std::error_code error;
    std::filesystem::rename(source, destination, error);
    if (error) return Err<void>(ErrorCode::IoError, error.message());
#endif
    return Ok();
}

Result<std::string> sha256_file(const std::filesystem::path& path) {
#ifdef _WIN32
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD object_size = 0;
    DWORD hash_size = 0;
    DWORD received = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &received, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                          reinterpret_cast<PUCHAR>(&hash_size), sizeof(hash_size), &received, 0) < 0) {
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        return Err<std::string>(ErrorCode::Internal, "cannot initialize SHA-256");
    }
    std::vector<unsigned char> object(object_size);
    std::vector<unsigned char> digest(hash_size);
    if (BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return Err<std::string>(ErrorCode::Internal, "cannot initialize SHA-256 hash");
    }
    std::ifstream input(path, std::ios::binary);
    std::vector<char> buffer(1024 * 1024);
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0 && BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()),
                                       static_cast<ULONG>(count), 0) < 0) {
            BCryptDestroyHash(hash);
            BCryptCloseAlgorithmProvider(algorithm, 0);
            return Err<std::string>(ErrorCode::IoError, "cannot hash model artifact");
        }
    }
    const auto status = BCryptFinishHash(hash, digest.data(), hash_size, 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) return Err<std::string>(ErrorCode::IoError, "cannot finish model checksum");
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (unsigned char byte : digest) output << std::setw(2) << static_cast<int>(byte);
    return Ok(output.str());
#else
    (void)path;
    return Err<std::string>(ErrorCode::Unavailable, "SHA-256 validation requires the Windows build");
#endif
}

}

nlohmann::json to_json(const StoreDownload& download) {
    return {
        {"id", download.id}, {"repo", download.file.repo},
        {"revision", download.file.revision}, {"filename", download.file.name},
        {"modelName", download.model_name}, {"runtime", download.file.runtime},
        {"modality", download.file.modality}, {"state", download.state},
        {"error", download.error}, {"bytesDownloaded", download.bytes_downloaded},
        {"bytesTotal", download.bytes_total}, {"installedPath", download.installed_path}
    };
}

ModelStore::ModelStore(std::filesystem::path root, std::string token,
                       model::BackendCoordinator& coordinator,
                       std::unique_ptr<IModelStoreTransport> transport)
    : root_(std::move(root)), token_(std::move(token)), coordinator_(coordinator),
      transport_(transport ? std::move(transport) : make_native_model_store_transport()) {
    std::filesystem::create_directories(root_);
    load_manifest();
}

ModelStore::~ModelStore() {
    std::vector<std::thread> workers;
    {
        std::lock_guard lock(mutex_);
        for (auto& [_, cancel] : cancellations_) cancel->store(true);
        for (auto& [_, worker] : workers_) workers.push_back(std::move(worker));
    }
    for (auto& worker : workers) if (worker.joinable()) worker.join();
}

Result<nlohmann::json> ModelStore::search(const std::string& query,
                                          const std::string& runtime,
                                          const std::string& modality, int limit) {
    if (query.empty() || query.size() > 200 || limit < 1 || limit > 100) {
        return Err<nlohmann::json>(ErrorCode::InvalidArgument, "invalid model search");
    }
    auto response = transport_->get_json(
        "https://huggingface.co/api/models?search=" + encode(query) +
        "&full=true&sort=downloads&direction=-1&limit=" +
        std::to_string(limit), token_);
    if (!response) return Err<nlohmann::json>(response.error().code, response.error().message);
    nlohmann::json results = nlohmann::json::array();
    if (!response->is_array()) return Err<nlohmann::json>(ErrorCode::ParseError, "invalid model source response");
    for (const auto& item : *response) {
        const std::string id = item.value("id", "");
        const std::string pipeline = item.value("pipeline_tag", "");
        if (id.empty()) continue;
        const auto tags = item.value("tags", nlohmann::json::array());
        const std::string searchable = lower(id + " " + tags.dump());
        std::string inferred_runtime = infer_runtime("", pipeline);
        if (lower(pipeline).find("automatic-speech-recognition") != std::string::npos &&
            (searchable.find("onnx") != std::string::npos ||
             searchable.find("sherpa") != std::string::npos)) {
            inferred_runtime = "sherpa_onnx";
        }
        const std::string inferred_modality = infer_modality(inferred_runtime, pipeline);
        if (!runtime.empty() && inferred_runtime != runtime) continue;
        if (!modality.empty() && inferred_modality != modality) continue;
        const auto downloads = item.value("downloads", 0);
        const auto likes = item.value("likes", 0);
        const bool has_vision =
            lower(pipeline).find("image-text") != std::string::npos ||
            lower(pipeline).find("visual-question") != std::string::npos ||
            searchable.find("vision") != std::string::npos ||
            searchable.find("multimodal") != std::string::npos;
        results.push_back({
            {"id", id}, {"pipeline", pipeline}, {"runtime", inferred_runtime},
            {"modality", inferred_modality}, {"downloads", downloads},
            {"likes", likes}, {"private", item.value("private", false)},
            {"gated", item.value("gated", nlohmann::json(false))},
            {"lastModified", item.value("lastModified", "")},
            {"hasVision", has_vision},
            {"recommended", downloads >= 1000 || likes >= 25}
        });
    }
    std::sort(results.begin(), results.end(), [](const auto& left, const auto& right) {
        const auto left_downloads = left.value("downloads", 0LL);
        const auto right_downloads = right.value("downloads", 0LL);
        if (left_downloads != right_downloads) return left_downloads > right_downloads;
        return left.value("likes", 0LL) > right.value("likes", 0LL);
    });
    return Ok(std::move(results));
}

Result<nlohmann::json> ModelStore::inspect(const std::string& repo) {
    if (!valid_repo(repo)) return Err<nlohmann::json>(ErrorCode::InvalidArgument, "invalid repository id");
    auto response = transport_->get_json(
        "https://huggingface.co/api/models/" + repo + "?blobs=true", token_);
    if (!response) return Err<nlohmann::json>(response.error().code, response.error().message);
    const std::string revision = response->value("sha", "main");
    const std::string pipeline = response->value("pipeline_tag", "");
    nlohmann::json files = nlohmann::json::array();
    for (const auto& sibling : response->value("siblings", nlohmann::json::array())) {
        const std::string filename = sibling.value("rfilename", "");
        const std::string runtime = infer_runtime(filename, pipeline);
        if (filename.empty() || !compatible_extension(filename, runtime)) continue;
        const auto lfs = sibling.value("lfs", nlohmann::json::object());
        const std::uint64_t size = lfs.value("size", sibling.value("size", std::uint64_t{0}));
        const std::string sha = lfs.value("sha256", lfs.value("oid", ""));
        const std::string modality = infer_modality(runtime, pipeline);
        files.push_back({
            {"repo", repo}, {"revision", revision}, {"name", filename},
            {"size", size}, {"sha256", sha}, {"runtime", runtime},
            {"modality", modality}, {"capabilities", capabilities_for(runtime, modality)},
            {"format", lower(std::filesystem::path(filename).extension().string())},
            {"quantization", infer_quantization(filename)}, {"compatible", size > 0 && sha.size() == 64},
            {"estimatedRamMb", static_cast<std::uint64_t>((size + 1024 * 1024 - 1) / (1024 * 1024))},
            {"estimatedVramMb", static_cast<std::uint64_t>((size + 1024 * 1024 - 1) / (1024 * 1024))}
        });
    }
    return Ok(nlohmann::json{{"id", repo}, {"revision", revision},
                              {"pipeline", pipeline}, {"files", std::move(files)}});
}

Result<StoreFile> ModelStore::resolve_file(const std::string& repo,
                                           const std::string& filename,
                                           const std::string& runtime,
                                           const std::string& modality) {
    auto details = inspect(repo);
    if (!details) return Err<StoreFile>(details.error().code, details.error().message);
    for (const auto& file : details->at("files")) {
        if (file.value("name", "") != filename) continue;
        if (file.value("runtime", "") != runtime || file.value("modality", "") != modality) {
            return Err<StoreFile>(ErrorCode::InvalidArgument, "runtime or modality is incompatible with artifact");
        }
        StoreFile result;
        result.repo = repo;
        result.revision = file.value("revision", details->value("revision", "main"));
        result.name = filename;
        result.sha256 = file.value("sha256", "");
        result.size = file.value("size", std::uint64_t{0});
        result.runtime = runtime;
        result.modality = modality;
        result.capabilities = file.value("capabilities", capabilities_for(runtime, modality));
        if (result.size == 0 || result.sha256.size() != 64) {
            return Err<StoreFile>(ErrorCode::InvalidArgument, "artifact has no verifiable size and SHA-256 metadata");
        }
        return Ok(std::move(result));
    }
    return Err<StoreFile>(ErrorCode::NotFound, "compatible artifact not found");
}

Result<std::uint64_t> ModelStore::install(const std::string& repo,
                                          const std::string& filename,
                                          const std::string& runtime,
                                          const std::string& modality,
                                          const std::string& model_name) {
    if (model_name.empty() || model_name.size() > 160 || safe_name(model_name) != model_name) {
        return Err<std::uint64_t>(ErrorCode::InvalidArgument, "invalid model name");
    }
    auto file = resolve_file(repo, filename, runtime, modality);
    if (!file) return Err<std::uint64_t>(file.error().code, file.error().message);
    const auto space = std::filesystem::space(root_);
    if (space.available < file->size + 64 * 1024 * 1024) {
        return Err<std::uint64_t>(ErrorCode::Unavailable, "insufficient disk space for model artifact");
    }
    reap_completed_workers();
    std::uint64_t id = 0;
    {
        std::lock_guard lock(mutex_);
        prune_completed_jobs_locked();
        const auto active = std::count_if(
            downloads_.begin(), downloads_.end(), [](const auto& entry) {
                return entry.second.state == "queued" || entry.second.state == "downloading";
            });
        if (active >= kMaxActiveInstalls) {
            return Err<std::uint64_t>(
                ErrorCode::Unavailable,
                "model store already has the maximum number of active installs");
        }
        if (downloads_.size() >= kMaxRetainedJobs) {
            return Err<std::uint64_t>(
                ErrorCode::Unavailable,
                "model store job history is full; retry after active installs finish");
        }
        if (reserved_names_.contains(model_name) || installed_.contains(model_name) ||
            coordinator_.registry().has(model_name)) {
            return Err<std::uint64_t>(
                ErrorCode::AlreadyExists,
                "model name is already installed, registered, or being installed");
        }
        id = next_id_++;
        reserved_names_.emplace(model_name, id);
        StoreDownload job;
        job.id = id;
        job.file = std::move(*file);
        job.model_name = model_name;
        job.bytes_total = job.file.size;
        downloads_[id] = std::move(job);
        cancellations_[id] = std::make_shared<std::atomic<bool>>(false);
    }
    auto started = start(id);
    if (!started) {
        std::lock_guard lock(mutex_);
        const auto reservation = reserved_names_.find(model_name);
        if (reservation != reserved_names_.end() &&
            reservation->second == id) {
            reserved_names_.erase(reservation);
        }
        downloads_.erase(id);
        cancellations_.erase(id);
        return Err<std::uint64_t>(started.error().code, started.error().message);
    }
    return Ok(id);
}

Result<void> ModelStore::start(std::uint64_t id) {
    reap_completed_workers();
    std::thread previous;
    std::shared_ptr<std::atomic<bool>> done;
    {
        std::lock_guard lock(mutex_);
        auto job = downloads_.find(id);
        if (job == downloads_.end()) return Err<void>(ErrorCode::NotFound, "download not found");
        if (job->second.state == "downloading") return Err<void>(ErrorCode::AlreadyExists, "download is already running");
        const auto active = std::count_if(
            downloads_.begin(), downloads_.end(), [id](const auto& entry) {
                return entry.first != id &&
                       (entry.second.state == "queued" || entry.second.state == "downloading");
            });
        if (active >= kMaxActiveInstalls) {
            return Err<void>(
                ErrorCode::Unavailable,
                "model store already has the maximum number of active installs");
        }
        if (workers_.size() >= kMaxRetainedJobs) {
            return Err<void>(
                ErrorCode::Unavailable,
                "model store worker history is full; retry after installs finish");
        }
        const auto reservation = reserved_names_.find(job->second.model_name);
        if ((reservation != reserved_names_.end() &&
             reservation->second != id) ||
            (reservation == reserved_names_.end() &&
             (installed_.contains(job->second.model_name) ||
              coordinator_.registry().has(job->second.model_name)))) {
            return Err<void>(
                ErrorCode::AlreadyExists,
                "model name is already installed, registered, or being installed");
        }
        if (auto worker = workers_.find(id); worker != workers_.end()) {
            previous = std::move(worker->second);
            workers_.erase(worker);
            worker_done_.erase(id);
        }
        cancellations_[id] = std::make_shared<std::atomic<bool>>(false);
        job->second.state = "queued";
        job->second.error.clear();
        reserved_names_[job->second.model_name] = id;
    }
    if (previous.joinable()) previous.join();
    done = std::make_shared<std::atomic<bool>>(false);
    {
        std::lock_guard lock(mutex_);
        try {
            workers_.emplace(
                id, std::thread([this, id, done] { worker_entry(id, done); }));
            worker_done_[id] = done;
        } catch (const std::exception& error) {
            downloads_.at(id).state = "failed";
            downloads_.at(id).error =
                std::string("cannot start model install worker: ") + error.what();
            const auto reservation =
                reserved_names_.find(downloads_.at(id).model_name);
            if (reservation != reserved_names_.end() &&
                reservation->second == id) {
                reserved_names_.erase(reservation);
            }
            return Err<void>(ErrorCode::Unavailable,
                             "cannot start model install worker");
        }
    }
    return Ok();
}

void ModelStore::worker_entry(
    std::uint64_t id,
    const std::shared_ptr<std::atomic<bool>>& done) noexcept {
    try {
        run(id);
    } catch (const std::exception& error) {
        fail_job(id, std::string("model install failed unexpectedly: ") + error.what());
    } catch (...) {
        fail_job(id, "model install failed unexpectedly");
    }
    done->store(true, std::memory_order_release);
}

void ModelStore::fail_job(std::uint64_t id, std::string error) noexcept {
    try {
        finish_job(id, "failed", std::move(error));
    } catch (...) {
    }
}

void ModelStore::finish_job(std::uint64_t id, std::string state,
                            std::string error) {
    std::lock_guard lock(mutex_);
    const auto job = downloads_.find(id);
    if (job == downloads_.end()) return;
    job->second.state = std::move(state);
    job->second.error = std::move(error);
    const auto reservation = reserved_names_.find(job->second.model_name);
    if (reservation != reserved_names_.end() &&
        reservation->second == id) {
        reserved_names_.erase(reservation);
    }
}

void ModelStore::reap_completed_workers() {
    std::vector<std::thread> completed;
    {
        std::lock_guard lock(mutex_);
        for (auto worker = workers_.begin(); worker != workers_.end();) {
            const auto done = worker_done_.find(worker->first);
            if (done == worker_done_.end() ||
                !done->second->load(std::memory_order_acquire)) {
                ++worker;
                continue;
            }
            completed.push_back(std::move(worker->second));
            worker_done_.erase(worker->first);
            worker = workers_.erase(worker);
        }
    }
    for (auto& worker : completed) {
        if (worker.joinable()) worker.join();
    }
}

void ModelStore::prune_completed_jobs_locked() {
    while (downloads_.size() >= kMaxRetainedJobs) {
        auto oldest = downloads_.end();
        for (auto job = downloads_.begin(); job != downloads_.end(); ++job) {
            const bool terminal = job->second.state == "installed" ||
                                  job->second.state == "failed" ||
                                  job->second.state == "cancelled";
            if (terminal && (oldest == downloads_.end() ||
                             job->first < oldest->first)) {
                oldest = job;
            }
        }
        if (oldest == downloads_.end()) return;
        cancellations_.erase(oldest->first);
        downloads_.erase(oldest);
    }
}

void ModelStore::run(std::uint64_t id) {
    StoreDownload job;
    std::shared_ptr<std::atomic<bool>> cancelled;
    {
        std::lock_guard lock(mutex_);
        job = downloads_.at(id);
        cancelled = cancellations_.at(id);
        downloads_.at(id).state = "downloading";
    }
    const std::filesystem::path directory = root_ / safe_name(job.file.runtime) /
                                            safe_name(job.file.repo);
    std::filesystem::create_directories(directory);
    const std::filesystem::path final_path = directory / safe_name(std::filesystem::path(job.file.name).filename().string());
    const std::filesystem::path partial_path = final_path.string() + ".partial";
    const auto partial_size = local_file_size(partial_path);
    if (partial_size > job.file.size) {
        std::error_code ignored;
        std::filesystem::remove(partial_path, ignored);
        finish_job(id, "failed",
                   "partial download exceeds the validated source size");
        return;
    }
    const std::uint64_t offset = partial_size;
    const std::string url = "https://huggingface.co/" + job.file.repo + "/resolve/" +
                            encode(job.file.revision) + "/" + encode(job.file.name);
    Result<void> downloaded = Ok();
    const auto exceeded_size = std::make_shared<std::atomic<bool>>(false);
    if (offset < job.file.size) {
        downloaded = transport_->download(
            url, token_, partial_path, offset,
            [this, id, cancelled, exceeded_size,
             expected_size = job.file.size](std::uint64_t bytes) {
                if (bytes > expected_size) {
                    exceeded_size->store(true, std::memory_order_release);
                    return false;
                }
                std::lock_guard lock(mutex_);
                downloads_.at(id).bytes_downloaded = bytes;
                return !cancelled->load();
            });
    }
    if (!downloaded) {
        if (exceeded_size->load(std::memory_order_acquire)) {
            std::error_code ignored;
            std::filesystem::remove(partial_path, ignored);
            finish_job(id, "failed",
                       "download exceeds the validated source size");
        } else {
            finish_job(id, cancelled->load() ? "cancelled" : "failed",
                       cancelled->load() ? "cancelled" : downloaded.error().message);
        }
        return;
    }
    if (local_file_size(partial_path) != job.file.size) {
        finish_job(id, "failed",
                   "downloaded size does not match source metadata");
        return;
    }
    auto checksum = sha256_file(partial_path);
    if (!checksum || lower(*checksum) != lower(job.file.sha256)) {
        std::error_code ignored;
        std::filesystem::remove(partial_path, ignored);
        finish_job(
            id, "failed",
            checksum ? "downloaded checksum does not match source metadata"
                     : checksum.error().message);
        return;
    }
    auto finalized = replace_file(partial_path, final_path);
    if (!finalized) {
        finish_job(id, "failed", finalized.error().message);
        return;
    }
    model::ModelInfo info;
    info.name = job.model_name;
    info.family = job.file.repo;
    info.runtime = job.file.runtime;
    info.modality = job.file.modality;
    info.capabilities = job.file.capabilities;
    info.gguf_path = final_path.string();
    info.vram_required_mb = static_cast<int>((job.file.size + 1024 * 1024 - 1) / (1024 * 1024));
    {
        std::lock_guard lock(mutex_);
        installed_[job.model_name] = {
            {"name", info.name}, {"family", info.family}, {"runtime", info.runtime},
            {"modality", info.modality}, {"capabilities", info.capabilities},
            {"path", final_path.string()}, {"size", job.file.size},
            {"sha256", job.file.sha256}, {"vramRequiredMb", info.vram_required_mb}
        };
        downloads_.at(id).bytes_downloaded = job.file.size;
        downloads_.at(id).installed_path = final_path.string();
    }
    auto saved = save_manifest();
    if (!saved) {
        std::error_code ignored;
        std::filesystem::remove(final_path, ignored);
        {
            std::lock_guard lock(mutex_);
            installed_.erase(job.model_name);
        }
        finish_job(id, "failed", saved.error().message);
        return;
    }
    coordinator_.registry().register_model(info);
    finish_job(id, "installed");
}

Result<void> ModelStore::cancel(std::uint64_t id) {
    std::lock_guard lock(mutex_);
    const auto cancel = cancellations_.find(id);
    if (cancel == cancellations_.end()) return Err<void>(ErrorCode::NotFound, "download not found");
    cancel->second->store(true);
    return Ok();
}

Result<void> ModelStore::resume(std::uint64_t id) {
    reap_completed_workers();
    {
        std::lock_guard lock(mutex_);
        const auto job = downloads_.find(id);
        if (job == downloads_.end()) return Err<void>(ErrorCode::NotFound, "download not found");
        if (job->second.state != "cancelled" && job->second.state != "failed") {
            return Err<void>(ErrorCode::InvalidArgument, "only cancelled or failed downloads can resume");
        }
    }
    return start(id);
}

Result<void> ModelStore::remove(const std::string& model_name) {
    nlohmann::json entry;
    {
        std::lock_guard lock(mutex_);
        if (!installed_.contains(model_name)) return Err<void>(ErrorCode::NotFound, "installed model not found");
        entry = installed_.at(model_name);
    }
    if (coordinator_.is_loaded(model_name) || coordinator_.active_request_count(model_name) > 0) {
        return Err<void>(ErrorCode::Unavailable, "active or loaded model cannot be removed");
    }
    const auto unregistered = coordinator_.unregister(model_name);
    if (!unregistered && unregistered.error().code != ErrorCode::NotFound) return unregistered;
    std::error_code error;
    const auto root = std::filesystem::weakly_canonical(root_, error);
    const auto path = std::filesystem::weakly_canonical(entry.value("path", ""), error);
    if (error || !foundation::is_path_within(root, path)) {
        return Err<void>(ErrorCode::InvalidArgument, "installed artifact is outside the model store");
    }
    if (!std::filesystem::remove(path, error) || error) {
        return Err<void>(ErrorCode::IoError, error ? error.message() : "cannot remove model artifact");
    }
    {
        std::lock_guard lock(mutex_);
        installed_.erase(model_name);
    }
    return save_manifest();
}

std::vector<StoreDownload> ModelStore::downloads() const {
    std::lock_guard lock(mutex_);
    std::vector<StoreDownload> result;
    result.reserve(downloads_.size());
    for (const auto& [_, download] : downloads_) result.push_back(download);
    std::sort(result.begin(), result.end(),
              [](const auto& left, const auto& right) { return left.id > right.id; });
    return result;
}

nlohmann::json ModelStore::installed() const {
    std::lock_guard lock(mutex_);
    return installed_;
}

nlohmann::json ModelStore::library() const {
    nlohmann::json result = nlohmann::json::array();
    const auto managed = installed();
    std::unordered_map<std::string, std::string> configured_files;

    auto normalized_path = [](const std::filesystem::path& path) {
        std::error_code error;
        auto normalized = std::filesystem::weakly_canonical(path, error).string();
#ifdef _WIN32
        normalized = lower(std::move(normalized));
#endif
        return error ? std::string{} : normalized;
    };

    for (const auto& name : coordinator_.registry().list()) {
        auto info_result = coordinator_.registry().get_info_result(name);
        if (!info_result) continue;
        const auto& info = *info_result;
        std::vector<std::filesystem::path> paths;
        if (!info.gguf_path.empty()) paths.emplace_back(info.gguf_path);
        if (!info.mmproj_path.empty()) paths.emplace_back(info.mmproj_path);
        for (const auto& [_, value] : info.artifacts) {
            std::error_code error;
            if (!value.empty() && std::filesystem::is_regular_file(value, error)) {
                paths.emplace_back(value);
            }
        }
        std::uint64_t size = 0;
        std::size_t artifact_count = 0;
        std::string display_path;
        std::string quantization{"unknown"};
        for (const auto& path : paths) {
            std::error_code error;
            if (!std::filesystem::is_regular_file(path, error)) continue;
            const auto key = normalized_path(path);
            if (!key.empty()) configured_files[key] = name;
            size += local_file_size(path);
            ++artifact_count;
            if (display_path.empty()) display_path = path.parent_path().string();
            if (path.extension() == ".gguf" && path.filename().string().find("mmproj") == std::string::npos) {
                quantization = infer_quantization(path.filename().string());
            }
        }
        result.push_back({
            {"id", "registered:" + name},
            {"name", name},
            {"family", info.family},
            {"runtime", info.runtime},
            {"modality", info.modality},
            {"capabilities", info.capabilities},
            {"path", display_path.empty() ? info.gguf_path : display_path},
            {"size", size},
            {"vramRequiredMb", info.vram_required_mb},
            {"quantization", quantization},
            {"artifactCount", artifact_count},
            {"hasVision", info.has_vision || !info.mmproj_path.empty()},
            {"configured", true},
            {"managed", managed.contains(name)}
        });
    }

    struct DiskGroup {
        std::filesystem::path directory;
        std::uint64_t size{0};
        std::size_t count{0};
        std::string runtime;
        std::string modality;
        std::string quantization{"unknown"};
        bool has_vision{false};
    };
    std::map<std::string, DiskGroup> groups;
    const auto library_root = lower(root_.filename().string()) == "store"
        ? root_.parent_path()
        : root_;
    std::error_code scan_error;
    std::size_t visited = 0;
    if (std::filesystem::is_directory(library_root, scan_error)) {
        std::filesystem::recursive_directory_iterator iterator(
            library_root,
            std::filesystem::directory_options::skip_permission_denied,
            scan_error);
        const std::filesystem::recursive_directory_iterator end;
        for (; iterator != end && !scan_error && visited < 4096;
             iterator.increment(scan_error), ++visited) {
            if (!iterator->is_regular_file(scan_error)) continue;
            const auto path = iterator->path();
            const auto extension = lower(path.extension().string());
            if (extension != ".gguf" && extension != ".onnx" &&
                extension != ".ort" && extension != ".bin" &&
                extension != ".safetensors") {
                continue;
            }
            const auto normalized = normalized_path(path);
            if (normalized.empty() || configured_files.contains(normalized)) continue;
            const auto directory_key = normalized_path(path.parent_path());
            if (directory_key.empty()) continue;
            auto& group = groups[directory_key];
            group.directory = path.parent_path();
            group.size += local_file_size(path);
            ++group.count;
            const auto searchable = lower(path.string());
            const auto file_runtime = infer_runtime(path.filename().string(), "");
            if (group.runtime.empty() || file_runtime != "llama_cpp") {
                group.runtime = file_runtime;
            }
            group.modality =
                searchable.find("\\stt\\") != std::string::npos ||
                searchable.find("/stt/") != std::string::npos
                    ? "audio_transcription"
                    : searchable.find("\\tts\\") != std::string::npos ||
                      searchable.find("/tts/") != std::string::npos
                        ? "audio_speech"
                        : infer_modality(group.runtime, "");
            const auto quantization = infer_quantization(path.filename().string());
            if (quantization != "unknown") group.quantization = quantization;
            if (searchable.find("mmproj") != std::string::npos) group.has_vision = true;
        }
    }
    for (const auto& [key, group] : groups) {
        std::error_code relative_error;
        const auto relative = std::filesystem::relative(
            group.directory, library_root, relative_error);
        const auto label = relative_error ? group.directory.filename().string() : relative.string();
        result.push_back({
            {"id", "disk:" + key},
            {"name", label},
            {"family", "On-disk artifact"},
            {"runtime", group.runtime},
            {"modality", group.modality},
            {"capabilities", capabilities_for(group.runtime, group.modality)},
            {"path", group.directory.string()},
            {"size", group.size},
            {"vramRequiredMb", static_cast<std::uint64_t>((group.size + 1024 * 1024 - 1) / (1024 * 1024))},
            {"quantization", group.quantization},
            {"artifactCount", group.count},
            {"hasVision", group.has_vision},
            {"configured", false},
            {"managed", false}
        });
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        const bool left_configured = left.value("configured", false);
        const bool right_configured = right.value("configured", false);
        if (left_configured != right_configured) return left_configured > right_configured;
        return left.value("name", "") < right.value("name", "");
    });
    return result;
}

void ModelStore::load_manifest() {
    std::ifstream input(root_ / "installed.json", std::ios::binary);
    if (!input) return;
    try {
        nlohmann::json manifest;
        input >> manifest;
        if (!manifest.is_object()) return;
        std::error_code error;
        const auto root = std::filesystem::weakly_canonical(root_, error);
        for (auto item = manifest.begin(); item != manifest.end(); ++item) {
            const auto path = std::filesystem::weakly_canonical(item.value().value("path", ""), error);
            if (error || !std::filesystem::is_regular_file(path, error) ||
                !foundation::is_path_within(root, path)) continue;
            model::ModelInfo info;
            info.name = item.key();
            info.family = item.value().value("family", "");
            info.runtime = item.value().value("runtime", "llama_cpp");
            info.modality = item.value().value("modality", "text");
            info.capabilities = item.value().value("capabilities", capabilities_for(info.runtime, info.modality));
            info.gguf_path = path.string();
            info.vram_required_mb = item.value().value("vramRequiredMb", 0);
            if (!coordinator_.registry().has(info.name)) coordinator_.registry().register_model(info);
            installed_[item.key()] = item.value();
        }
    } catch (...) {
        installed_ = nlohmann::json::object();
    }
}

Result<void> ModelStore::save_manifest() {
    std::lock_guard manifest_lock(manifest_mutex_);
    nlohmann::json snapshot;
    {
        std::lock_guard lock(mutex_);
        snapshot = installed_;
    }
    const auto temporary = root_ / "installed.json.tmp";
    const auto destination = root_ / "installed.json";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output << snapshot.dump(2);
    output.flush();
    if (!output) return Err<void>(ErrorCode::IoError, "cannot write model store manifest");
    output.close();
    return replace_file(temporary, destination);
}

#ifdef _WIN32

namespace {

std::wstring widen(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

struct InternetHandle {
    HINTERNET value{nullptr};
    InternetHandle() = default;
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
    InternetHandle(InternetHandle&& other) noexcept : value(other.value) { other.value = nullptr; }
    InternetHandle& operator=(InternetHandle&& other) noexcept {
        if (this != &other) {
            if (value) WinHttpCloseHandle(value);
            value = other.value;
            other.value = nullptr;
        }
        return *this;
    }
    ~InternetHandle() { if (value) WinHttpCloseHandle(value); }
};

struct OpenRequest {
    InternetHandle session;
    InternetHandle connection;
    InternetHandle request;
};

Result<OpenRequest> open_request(const std::string& url, const std::string& token,
                                 const std::optional<std::uint64_t>& range) {
    const std::wstring wide_url = widen(url);
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wide_url.c_str(), 0, 0, &components)) {
        return Err<OpenRequest>(ErrorCode::InvalidArgument, "invalid model source URL");
    }
    OpenRequest handles;
    handles.session.value = WinHttpOpen(L"InferDeck/2.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!handles.session.value) return Err<OpenRequest>(ErrorCode::Unavailable, "cannot initialize Windows HTTP");
    if (!WinHttpSetTimeouts(handles.session.value, 10000, 10000, 30000, 30000)) {
        return Err<OpenRequest>(ErrorCode::Unavailable,
                                "cannot configure model source timeouts");
    }
    std::wstring host(components.lpszHostName, components.dwHostNameLength);
    handles.connection.value = WinHttpConnect(handles.session.value, host.c_str(), components.nPort, 0);
    if (!handles.connection.value) return Err<OpenRequest>(ErrorCode::Unavailable, "cannot connect to model source");
    std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);
    if (components.dwExtraInfoLength) path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    handles.request.value = WinHttpOpenRequest(
        handles.connection.value, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!handles.request.value) return Err<OpenRequest>(ErrorCode::Unavailable, "cannot create model source request");
    std::wstring headers = L"Accept: application/json\r\n";
    if (!token.empty()) headers += L"Authorization: Bearer " + widen(token) + L"\r\n";
    if (range && *range > 0) headers += L"Range: bytes=" + std::to_wstring(*range) + L"-\r\n";
    if (!WinHttpSendRequest(handles.request.value, headers.c_str(), static_cast<DWORD>(-1),
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(handles.request.value, nullptr)) {
        return Err<OpenRequest>(ErrorCode::Unavailable, "model source request failed");
    }
    return Ok(std::move(handles));
}

DWORD status_code(HINTERNET request) {
    DWORD status = 0;
    DWORD size = sizeof(status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    return status;
}

class WinHttpModelStoreTransport final : public IModelStoreTransport {
public:
    Result<nlohmann::json> get_json(const std::string& url, const std::string& token) override {
        auto handles = open_request(url, token, std::nullopt);
        if (!handles) return Err<nlohmann::json>(handles.error().code, handles.error().message);
        const DWORD status = status_code(handles->request.value);
        if (status < 200 || status >= 300) {
            return Err<nlohmann::json>(status == 404 ? ErrorCode::NotFound : ErrorCode::Unavailable,
                                       "model source responded " + std::to_string(status));
        }
        std::string body;
        std::array<char, 64 * 1024> buffer{};
        while (true) {
            DWORD received = 0;
            if (!WinHttpReadData(handles->request.value, buffer.data(), static_cast<DWORD>(buffer.size()), &received)) {
                return Err<nlohmann::json>(ErrorCode::IoError, "cannot read model source response");
            }
            if (received == 0) break;
            body.append(buffer.data(), received);
            if (body.size() > 32 * 1024 * 1024) return Err<nlohmann::json>(ErrorCode::InvalidArgument, "model metadata is too large");
        }
        try {
            return Ok(nlohmann::json::parse(body));
        } catch (const std::exception& error) {
            return Err<nlohmann::json>(ErrorCode::ParseError, error.what());
        }
    }

    Result<void> download(const std::string& url, const std::string& token,
                          const std::filesystem::path& destination, std::uint64_t offset,
                          const std::function<bool(std::uint64_t)>& progress) override {
        auto handles = open_request(url, token, offset);
        if (!handles) return Err<void>(handles.error().code, handles.error().message);
        const DWORD status = status_code(handles->request.value);
        if (status != 200 && status != 206) {
            return Err<void>(status == 404 ? ErrorCode::NotFound : ErrorCode::Unavailable,
                             "model download responded " + std::to_string(status));
        }
        if (offset > 0 && status == 200) offset = 0;
        std::ofstream output(destination, std::ios::binary |
                            (offset > 0 ? std::ios::app : std::ios::trunc));
        if (!output) return Err<void>(ErrorCode::IoError, "cannot open partial model artifact");
        std::vector<char> buffer(1024 * 1024);
        std::uint64_t total = offset;
        while (true) {
            DWORD received = 0;
            if (!WinHttpReadData(handles->request.value, buffer.data(), static_cast<DWORD>(buffer.size()), &received)) {
                return Err<void>(ErrorCode::IoError, "model download read failed");
            }
            if (received == 0) break;
            output.write(buffer.data(), received);
            if (!output) return Err<void>(ErrorCode::IoError, "model download write failed");
            total += received;
            if (!progress(total)) return Err<void>(ErrorCode::Cancelled, "cancelled");
        }
        output.flush();
        if (!output) return Err<void>(ErrorCode::IoError, "cannot flush partial model artifact");
        return Ok();
    }
};

}

std::unique_ptr<IModelStoreTransport> make_native_model_store_transport() {
    return std::make_unique<WinHttpModelStoreTransport>();
}

#else

namespace {

class UnavailableModelStoreTransport final : public IModelStoreTransport {
public:
    Result<nlohmann::json> get_json(const std::string&, const std::string&) override {
        return Err<nlohmann::json>(ErrorCode::Unavailable, "model store network transport requires Windows");
    }
    Result<void> download(const std::string&, const std::string&, const std::filesystem::path&,
                          std::uint64_t, const std::function<bool(std::uint64_t)>&) override {
        return Err<void>(ErrorCode::Unavailable, "model store network transport requires Windows");
    }
};

}

std::unique_ptr<IModelStoreTransport> make_native_model_store_transport() {
    return std::make_unique<UnavailableModelStoreTransport>();
}

#endif

}
