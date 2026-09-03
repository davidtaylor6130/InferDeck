#include "gateway/model_store.hpp"

#include "foundation/path_utils.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <sstream>
#include <unordered_set>

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

constexpr std::string_view sherpa_bundle_name = "__inferdeck_sherpa_bundle__";
constexpr std::string_view ace_step_bundle_prefix =
    "__inferdeck_ace_step_bundle__:";

std::string encode(const std::string& value) {
    std::ostringstream output;
    output << std::hex << std::uppercase;
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') output << c;
        else output << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(c);
    }
    return output.str();
}

std::string encode_path(const std::string& value) {
    std::string output;
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto slash = value.find('/', start);
        if (!output.empty()) output += '/';
        output += encode(value.substr(start, slash == std::string::npos
            ? std::string::npos : slash - start));
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return output;
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

bool valid_artifact_path(const std::string& name) {
    const auto path = std::filesystem::path(name).lexically_normal();
    if (path.empty() || path.is_absolute()) return false;
    return std::none_of(path.begin(), path.end(), [](const auto& component) {
        return component == "..";
    });
}

std::string artifact_key(const std::string& name) {
    auto lower_name = std::filesystem::path(name).filename().string();
    std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower_name == "tts.json") return "tts_json";
    if (lower_name.find("voice_styles") != std::string::npos) return "voice_style";
    if (lower_name.find("qwen3-embedding") != std::string::npos) {
        return "text_encoder";
    }
    if (lower_name.starts_with("acestep-5hz-lm") &&
        std::filesystem::path(lower_name).extension() == ".gguf") {
        return "language_model";
    }
    if (lower_name.starts_with("acestep-v15") &&
        std::filesystem::path(lower_name).extension() == ".gguf") {
        return "dit";
    }
    if (lower_name.find("vae") != std::string::npos &&
        std::filesystem::path(lower_name).extension() == ".gguf") {
        return "vae";
    }
    for (const char* key : {"duration_predictor", "text_encoder", "vector_estimator",
                            "unicode_indexer", "voice_style", "tts_json", "encoder",
                            "decoder", "joiner", "tokens", "vocab", "voices",
                            "lexicon", "vocoder", "model"}) {
        if (lower_name.find(key) != std::string::npos) return key;
    }
    return safe_name(std::filesystem::path(name).stem().string());
}

bool compatible_extension(const std::string& filename, const std::string& runtime) {
    std::string extension = std::filesystem::path(filename).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (runtime == "llama_cpp") return extension == ".gguf";
    if (runtime == "stable_diffusion_cpp") return extension == ".safetensors" || extension == ".gguf" ||
                                                     extension == ".ckpt" || extension == ".pth" || extension == ".pt";
    if (runtime == "ace_step_cpp") return extension == ".gguf";
    if (runtime == "whisper_cpp") return extension == ".bin" || extension == ".gguf";
    if (runtime == "sherpa_onnx") {
        return extension == ".onnx" || extension == ".ort" || extension == ".bin" ||
               extension == ".txt" || extension == ".json";
    }
    return false;
}

std::vector<std::string> capabilities_for(const std::string& runtime,
                                           const std::string& modality) {
    if (modality == "embedding") return {"embeddings"};
    if (runtime == "stable_diffusion_cpp") return {"image_generation"};
    if (runtime == "ace_step_cpp") return {"audio_generation"};
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

std::string infer_runtime(const std::string& filename, const std::string& pipeline,
                          const std::string& context = {}) {
    const std::string name = lower(filename);
    const std::string tag = lower(pipeline);
    const std::string searchable = lower(filename + " " + pipeline + " " + context);
    if (searchable.find("ace-step") != std::string::npos ||
        searchable.find("acestep") != std::string::npos) {
        return "ace_step_cpp";
    }
    if (name.ends_with(".onnx") || name.ends_with(".ort")) return "sherpa_onnx";
    if (tag.find("text-to-image") != std::string::npos || name.ends_with(".safetensors")) return "stable_diffusion_cpp";
    if (tag.find("automatic-speech-recognition") != std::string::npos) return "whisper_cpp";
    if (tag.find("text-to-speech") != std::string::npos) return "sherpa_onnx";
    return "llama_cpp";
}

std::string infer_modality(const std::string& runtime, const std::string& pipeline) {
    if (runtime == "stable_diffusion_cpp") return "image";
    if (runtime == "ace_step_cpp") return "audio_generation";
    if (runtime == "whisper_cpp") return "audio_transcription";
    if (runtime == "sherpa_onnx") {
        return lower(pipeline).find("automatic-speech-recognition") != std::string::npos
            ? "audio_transcription"
            : "audio_speech";
    }
    if (lower(pipeline).find("feature-extraction") != std::string::npos) return "embedding";
    return "text";
}

struct CatalogueCompatibility {
    std::size_t artifacts{0};
    std::string format;
};

std::string catalogue_string(const nlohmann::json& item, const char* key) {
    if (!item.is_object()) return {};
    const auto value = item.find(key);
    return value != item.end() && value->is_string()
        ? value->get<std::string>() : std::string{};
}

std::int64_t catalogue_integer(const nlohmann::json& item, const char* key) {
    if (!item.is_object()) return 0;
    const auto value = item.find(key);
    if (value == item.end() ||
        (!value->is_number_integer() && !value->is_number_unsigned())) {
        return 0;
    }
    try {
        return value->get<std::int64_t>();
    } catch (...) {
        return 0;
    }
}

std::uint64_t catalogue_unsigned(const nlohmann::json& item, const char* key) {
    if (!item.is_object()) return 0;
    const auto value = item.find(key);
    if (value == item.end() ||
        (!value->is_number_integer() && !value->is_number_unsigned())) {
        return 0;
    }
    try {
        if (value->is_number_unsigned()) return value->get<std::uint64_t>();
        const auto signed_value = value->get<std::int64_t>();
        return signed_value > 0 ? static_cast<std::uint64_t>(signed_value) : 0;
    } catch (...) {
        return 0;
    }
}

double catalogue_number(const nlohmann::json& item, const char* key) {
    if (!item.is_object()) return 0.0;
    const auto value = item.find(key);
    if (value == item.end() || !value->is_number()) return 0.0;
    try {
        return value->get<double>();
    } catch (...) {
        return 0.0;
    }
}

bool catalogue_boolean(const nlohmann::json& item, const char* key) {
    if (!item.is_object()) return false;
    const auto value = item.find(key);
    return value != item.end() && value->is_boolean() && value->get<bool>();
}

bool catalogue_item_is_gated(const nlohmann::json& item) {
    const auto gated = item.find("gated");
    if (gated == item.end() || gated->is_null()) return false;
    if (gated->is_boolean()) return gated->get<bool>();
    if (gated->is_string()) {
        const std::string value = lower(gated->get<std::string>());
        return !value.empty() && value != "false" && value != "none";
    }
    return true;
}

bool is_split_gguf(const std::string& name) {
    const std::string filename = lower(
        std::filesystem::path(name).filename().string());
    return filename.ends_with(".gguf") &&
           filename.find("-of-") != std::string::npos;
}

bool is_primary_llama_artifact(const std::string& name) {
    if (!valid_artifact_path(name) || !compatible_extension(name, "llama_cpp") ||
        is_split_gguf(name)) {
        return false;
    }
    const std::string normalized = lower(name);
    const std::string filename = lower(
        std::filesystem::path(name).filename().string());
    return !filename.starts_with("mmproj") &&
           filename.find("-mmproj") == std::string::npos &&
           normalized.find("/mtp/") == std::string::npos &&
           filename.find("-mtp-") == std::string::npos;
}

bool is_primary_image_artifact(const std::string& name) {
    if (!valid_artifact_path(name) ||
        !compatible_extension(name, "stable_diffusion_cpp")) {
        return false;
    }
    const std::string normalized = lower(name);
    const std::string rooted = "/" + normalized;
    const std::string filename = lower(
        std::filesystem::path(name).filename().string());
    for (const std::string_view component : {
             "/vae/", "/text_encoder/", "/text_encoder_2/", "/clip/",
             "/controlnet/", "/lora/", "/unet/", "/transformer/"}) {
        if (rooted.find(component) != std::string::npos) return false;
    }
    return !filename.starts_with("vae") && !filename.starts_with("ae.") &&
           !filename.starts_with("clip") &&
           !filename.starts_with("text_encoder") &&
           !filename.starts_with("pytorch_model") &&
           !filename.starts_with("diffusion_pytorch_model");
}

bool is_primary_whisper_artifact(const std::string& name) {
    if (!valid_artifact_path(name) ||
        !compatible_extension(name, "whisper_cpp") || is_split_gguf(name)) {
        return false;
    }
    const std::string filename = lower(
        std::filesystem::path(name).filename().string());
    const std::string extension = lower(
        std::filesystem::path(filename).extension().string());
    return extension == ".gguf" ||
           (extension == ".bin" && filename.starts_with("ggml-"));
}

std::optional<CatalogueCompatibility> catalogue_compatibility(
    const nlohmann::json& siblings, const std::string& runtime,
    const std::string& modality) {
    if (!siblings.is_array()) return std::nullopt;
    std::size_t standalone_count = 0;
    std::unordered_set<std::string> bundle_keys;
    for (const auto& sibling : siblings) {
        const std::string name = catalogue_string(sibling, "rfilename");
        if (name.empty()) continue;
        if (runtime == "llama_cpp" && is_primary_llama_artifact(name)) {
            ++standalone_count;
        } else if (runtime == "stable_diffusion_cpp" &&
                   is_primary_image_artifact(name)) {
            ++standalone_count;
        } else if (runtime == "whisper_cpp" &&
                   is_primary_whisper_artifact(name)) {
            ++standalone_count;
        } else if ((runtime == "sherpa_onnx" || runtime == "ace_step_cpp") &&
                   valid_artifact_path(name) &&
                   compatible_extension(name, runtime)) {
            bundle_keys.insert(artifact_key(name));
        }
    }
    if (runtime == "ace_step_cpp") {
        if (bundle_keys.contains("text_encoder") &&
            bundle_keys.contains("dit") && bundle_keys.contains("vae")) {
            return CatalogueCompatibility{3, "bundle"};
        }
        return std::nullopt;
    }
    if (runtime == "sherpa_onnx") {
        const auto contains_all = [&bundle_keys](
                                      std::initializer_list<const char*> keys) {
            return std::all_of(keys.begin(), keys.end(),
                               [&bundle_keys](const char* key) {
                                   return bundle_keys.contains(key);
                               });
        };
        const bool complete_asr =
            contains_all({"encoder", "decoder", "joiner", "tokens"});
        const bool supertonic = bundle_keys.contains("duration_predictor") ||
            bundle_keys.contains("text_encoder") ||
            bundle_keys.contains("vector_estimator");
        const bool complete_tts = supertonic
            ? contains_all({"duration_predictor", "text_encoder",
                            "vector_estimator", "vocoder", "tts_json",
                            "unicode_indexer", "voice_style"})
            : contains_all({"model", "tokens"});
        const bool complete = modality == "audio_transcription"
            ? complete_asr : complete_tts;
        if (complete) {
            return CatalogueCompatibility{bundle_keys.size(), "bundle"};
        }
        return std::nullopt;
    }
    if (standalone_count == 0) return std::nullopt;
    const std::string format = runtime == "stable_diffusion_cpp"
        ? "checkpoint" : runtime == "whisper_cpp" ? "whisper" : "GGUF";
    return CatalogueCompatibility{standalone_count, format};
}

std::string catalogue_license(const nlohmann::json& tags) {
    if (!tags.is_array()) return {};
    for (const auto& tag : tags) {
        if (!tag.is_string()) continue;
        const std::string value = tag.get<std::string>();
        if (lower(value).starts_with("license:")) return value.substr(8);
    }
    return {};
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

bool is_ace_step_bundle(const std::string& name) {
    return name.rfind(ace_step_bundle_prefix, 0) == 0;
}

std::string ace_step_bundle_name(const std::string& dit_name) {
    return std::string(ace_step_bundle_prefix) + dit_name;
}

std::optional<std::string> ace_step_bundle_dit(const std::string& name) {
    if (!is_ace_step_bundle(name)) return std::nullopt;
    const std::string dit_name = name.substr(ace_step_bundle_prefix.size());
    if (!valid_artifact_path(dit_name) || artifact_key(dit_name) != "dit") {
        return std::nullopt;
    }
    return dit_name;
}

std::optional<std::string> preferred_ace_artifact(
    const nlohmann::json& files, const std::string_view artifact) {
    std::optional<std::string> selected;
    int selected_priority = 0;
    std::uint64_t selected_size = 0;
    for (const auto& file : files) {
        const std::string name = file.value("name", "");
        if (file.value("runtime", "") != "ace_step_cpp" ||
            artifact_key(name) != artifact || !valid_artifact_path(name) ||
            !file.value("compatible", false)) {
            continue;
        }
        const std::string normalized = lower(name);
        int priority = 1;
        if (artifact == "text_encoder" &&
            normalized.find("q8_0") != std::string::npos) {
            priority = 0;
        } else if (artifact == "vae" &&
                   normalized.find("bf16") != std::string::npos) {
            priority = 0;
        }
        const std::uint64_t size = file.value("size", std::uint64_t{0});
        if (!selected || priority < selected_priority ||
            (priority == selected_priority &&
             (size < selected_size ||
              (size == selected_size && name < *selected)))) {
            selected = name;
            selected_priority = priority;
            selected_size = size;
        }
    }
    return selected;
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

Result<void> install_new_file(const std::filesystem::path& source,
                              const std::filesystem::path& destination) {
    std::error_code exists_error;
    if (std::filesystem::exists(destination, exists_error)) {
        return Err<void>(ErrorCode::AlreadyExists,
                         "quantized model artifact already exists");
    }
#ifdef _WIN32
    if (!MoveFileExW(source.c_str(), destination.c_str(),
                     MOVEFILE_WRITE_THROUGH)) {
        const DWORD code = GetLastError();
        return Err<void>(
            code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS
                ? ErrorCode::AlreadyExists : ErrorCode::IoError,
            code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS
                ? "quantized model artifact already exists"
                : "cannot finalize quantized model artifact");
    }
#else
    std::error_code error;
    std::filesystem::rename(source, destination, error);
    if (error) return Err<void>(ErrorCode::IoError, error.message());
#endif
    return Ok();
}

Result<void> create_confined_directory(
    const std::filesystem::path& canonical_root,
    const std::filesystem::path& directory) {
    std::error_code error;
    std::filesystem::create_directories(directory.parent_path(), error);
    const auto parent = std::filesystem::weakly_canonical(
        directory.parent_path(), error);
    if (error || !std::filesystem::is_directory(parent, error) ||
        !foundation::is_path_within(canonical_root, parent)) {
        return Err<void>(ErrorCode::InvalidArgument,
                         "quantization output parent escapes the model store");
    }
    if (!std::filesystem::create_directory(directory, error)) {
        return Err<void>(
            error ? ErrorCode::IoError : ErrorCode::AlreadyExists,
            error ? error.message() : "quantization output path already exists");
    }
    const auto resolved = std::filesystem::weakly_canonical(directory, error);
    if (error || !std::filesystem::is_directory(resolved, error) ||
        !foundation::is_path_within(canonical_root, resolved)) {
        std::error_code ignored;
        std::filesystem::remove(directory, ignored);
        return Err<void>(ErrorCode::InvalidArgument,
                         "quantization output path escapes the model store");
    }
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
        {"bytesTotal", download.bytes_total}, {"installedPath", download.installed_path},
        {"artifactCount", download.artifacts.empty() ? 1 : download.artifacts.size()}
    };
}

nlohmann::json to_json(const StoreQuantization& quantization) {
    return {
        {"id", quantization.id},
        {"sourceModel", quantization.source_model},
        {"outputModel", quantization.output_model},
        {"quantization", quantization.quantization},
        {"threads", quantization.threads},
        {"state", quantization.state},
        {"error", quantization.error},
        {"outputPath", quantization.state == "installed"
                           ? quantization.output_path : std::string{}},
        {"outputSize", quantization.output_size},
        {"outputSha256", quantization.output_sha256},
        {"cancellable", false}
    };
}

ModelStore::ModelStore(std::filesystem::path root, std::filesystem::path archive_root,
                       std::string token,
                       model::BackendCoordinator& coordinator,
                       std::unique_ptr<IModelStoreTransport> transport,
                       std::unique_ptr<IModelQuantizer> quantizer,
                       std::atomic<ComputeResource>* maintenance_resource)
    : root_(std::move(root)), archive_root_(std::move(archive_root)),
      token_(std::move(token)), coordinator_(coordinator),
      transport_(transport ? std::move(transport) : make_native_model_store_transport()),
      quantizer_(quantizer ? std::move(quantizer) : make_native_model_quantizer()),
      maintenance_resource_(maintenance_resource) {
    std::filesystem::create_directories(root_);
    std::filesystem::create_directories(archive_root_);
    load_manifest();
}

ModelStore::ModelStore(std::filesystem::path root, std::string token,
                       model::BackendCoordinator& coordinator,
                       std::unique_ptr<IModelStoreTransport> transport,
                       std::unique_ptr<IModelQuantizer> quantizer,
                       std::atomic<ComputeResource>* maintenance_resource)
    : ModelStore(root, root.parent_path() / "archive", std::move(token),
                 coordinator, std::move(transport), std::move(quantizer),
                 maintenance_resource) {}

ModelStore::~ModelStore() {
    std::vector<std::thread> workers;
    {
        std::lock_guard lock(mutex_);
        for (auto& [_, cancel] : cancellations_) cancel->store(true);
        for (auto& [_, worker] : workers_) workers.push_back(std::move(worker));
    }
    for (auto& worker : workers) if (worker.joinable()) worker.join();
    release_quantization_resource();
}

void ModelStore::release_quantization_resource() noexcept {
    if (!quantization_resource_reserved_.exchange(
            false, std::memory_order_acq_rel)) {
        return;
    }
    if (!maintenance_resource_) return;
    ComputeResource expected = ComputeResource::Cpu;
    (void)maintenance_resource_->compare_exchange_strong(
        expected, ComputeResource::None);
}

#include "model_store_discovery.ipp"

#include "model_store_downloads.ipp"

#include "model_store_quantization.ipp"

#include "model_store_library.ipp"

#include "model_store_transport.ipp"

}
