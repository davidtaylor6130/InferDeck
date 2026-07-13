#include "gateway/model_store.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iomanip>
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
    if (runtime == "sherpa_onnx") return extension == ".onnx" || extension == ".bin" || extension == ".txt";
    return false;
}

std::vector<std::string> capabilities_for(const std::string& runtime,
                                           const std::string& modality) {
    if (modality == "embedding") return {"embeddings"};
    if (runtime == "stable_diffusion_cpp") return {"image_generation"};
    if (runtime == "whisper_cpp") return {"audio_transcription"};
    if (runtime == "sherpa_onnx") return {"audio_speech"};
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
    if (tag.find("text-to-image") != std::string::npos || name.ends_with(".safetensors")) return "stable_diffusion_cpp";
    if (tag.find("automatic-speech-recognition") != std::string::npos) return "whisper_cpp";
    if (tag.find("text-to-speech") != std::string::npos) return "sherpa_onnx";
    return "llama_cpp";
}

std::string infer_modality(const std::string& runtime, const std::string& pipeline) {
    if (runtime == "stable_diffusion_cpp") return "image";
    if (runtime == "whisper_cpp") return "audio_transcription";
    if (runtime == "sherpa_onnx") return "audio_speech";
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
    std::array<char, 1024 * 1024> buffer{};
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
        "&full=true&limit=" + std::to_string(limit), token_);
    if (!response) return Err<nlohmann::json>(response.error().code, response.error().message);
    nlohmann::json results = nlohmann::json::array();
    if (!response->is_array()) return Err<nlohmann::json>(ErrorCode::ParseError, "invalid model source response");
    for (const auto& item : *response) {
        const std::string id = item.value("id", "");
        const std::string pipeline = item.value("pipeline_tag", "");
        if (id.empty()) continue;
        const std::string inferred_runtime = infer_runtime("", pipeline);
        const std::string inferred_modality = infer_modality(inferred_runtime, pipeline);
        if (!runtime.empty() && inferred_runtime != runtime) continue;
        if (!modality.empty() && inferred_modality != modality) continue;
        results.push_back({
            {"id", id}, {"pipeline", pipeline}, {"runtime", inferred_runtime},
            {"modality", inferred_modality}, {"downloads", item.value("downloads", 0)},
            {"likes", item.value("likes", 0)}, {"private", item.value("private", false)},
            {"gated", item.value("gated", nlohmann::json(false))}
        });
    }
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
    if (coordinator_.registry().has(model_name)) {
        return Err<std::uint64_t>(ErrorCode::AlreadyExists, "model name is already registered");
    }
    auto file = resolve_file(repo, filename, runtime, modality);
    if (!file) return Err<std::uint64_t>(file.error().code, file.error().message);
    const auto space = std::filesystem::space(root_);
    if (space.available < file->size + 64 * 1024 * 1024) {
        return Err<std::uint64_t>(ErrorCode::Unavailable, "insufficient disk space for model artifact");
    }
    std::uint64_t id;
    {
        std::lock_guard lock(mutex_);
        id = next_id_++;
        StoreDownload job;
        job.id = id;
        job.file = std::move(*file);
        job.model_name = model_name;
        job.bytes_total = job.file.size;
        downloads_[id] = std::move(job);
        cancellations_[id] = std::make_shared<std::atomic<bool>>(false);
    }
    auto started = start(id);
    if (!started) return Err<std::uint64_t>(started.error().code, started.error().message);
    return Ok(id);
}

Result<void> ModelStore::start(std::uint64_t id) {
    std::thread previous;
    {
        std::lock_guard lock(mutex_);
        auto job = downloads_.find(id);
        if (job == downloads_.end()) return Err<void>(ErrorCode::NotFound, "download not found");
        if (job->second.state == "downloading") return Err<void>(ErrorCode::AlreadyExists, "download is already running");
        if (auto worker = workers_.find(id); worker != workers_.end()) {
            previous = std::move(worker->second);
            workers_.erase(worker);
        }
        cancellations_[id] = std::make_shared<std::atomic<bool>>(false);
        job->second.state = "queued";
        job->second.error.clear();
    }
    if (previous.joinable()) previous.join();
    {
        std::lock_guard lock(mutex_);
        workers_.emplace(id, std::thread([this, id] { run(id); }));
    }
    return Ok();
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
    const std::uint64_t offset = std::min(local_file_size(partial_path), job.file.size);
    const std::string url = "https://huggingface.co/" + job.file.repo + "/resolve/" +
                            encode(job.file.revision) + "/" + encode(job.file.name);
    Result<void> downloaded = Ok();
    if (offset < job.file.size) {
        downloaded = transport_->download(
            url, token_, partial_path, offset,
            [this, id, cancelled](std::uint64_t bytes) {
                std::lock_guard lock(mutex_);
                downloads_.at(id).bytes_downloaded = bytes;
                return !cancelled->load();
            });
    }
    if (!downloaded) {
        std::lock_guard lock(mutex_);
        downloads_.at(id).state = cancelled->load() ? "cancelled" : "failed";
        downloads_.at(id).error = cancelled->load() ? "cancelled" : downloaded.error().message;
        return;
    }
    if (local_file_size(partial_path) != job.file.size) {
        std::lock_guard lock(mutex_);
        downloads_.at(id).state = "failed";
        downloads_.at(id).error = "downloaded size does not match source metadata";
        return;
    }
    auto checksum = sha256_file(partial_path);
    if (!checksum || lower(*checksum) != lower(job.file.sha256)) {
        std::error_code ignored;
        std::filesystem::remove(partial_path, ignored);
        std::lock_guard lock(mutex_);
        downloads_.at(id).state = "failed";
        downloads_.at(id).error = checksum ? "downloaded checksum does not match source metadata" : checksum.error().message;
        return;
    }
    auto finalized = replace_file(partial_path, final_path);
    if (!finalized) {
        std::lock_guard lock(mutex_);
        downloads_.at(id).state = "failed";
        downloads_.at(id).error = finalized.error().message;
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
        std::lock_guard lock(mutex_);
        installed_.erase(job.model_name);
        downloads_.at(id).state = "failed";
        downloads_.at(id).error = saved.error().message;
        return;
    }
    coordinator_.registry().register_model(info);
    std::lock_guard lock(mutex_);
    downloads_.at(id).state = "installed";
}

Result<void> ModelStore::cancel(std::uint64_t id) {
    std::lock_guard lock(mutex_);
    const auto cancel = cancellations_.find(id);
    if (cancel == cancellations_.end()) return Err<void>(ErrorCode::NotFound, "download not found");
    cancel->second->store(true);
    return Ok();
}

Result<void> ModelStore::resume(std::uint64_t id) {
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
    if (error || path.string().rfind(root.string() + std::filesystem::path::preferred_separator, 0) != 0) {
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
                path.string().rfind(root.string() + std::filesystem::path::preferred_separator, 0) != 0) continue;
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
        std::array<char, 1024 * 1024> buffer{};
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
