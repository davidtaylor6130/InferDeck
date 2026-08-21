#include "gateway/model_store.hpp"

#include "foundation/path_utils.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <numeric>
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
        {"bytesTotal", download.bytes_total}, {"installedPath", download.installed_path},
        {"artifactCount", download.artifacts.empty() ? 1 : download.artifacts.size()}
    };
}

ModelStore::ModelStore(std::filesystem::path root, std::filesystem::path archive_root,
                       std::string token,
                       model::BackendCoordinator& coordinator,
                       std::unique_ptr<IModelStoreTransport> transport)
    : root_(std::move(root)), archive_root_(std::move(archive_root)),
      token_(std::move(token)), coordinator_(coordinator),
      transport_(transport ? std::move(transport) : make_native_model_store_transport()) {
    std::filesystem::create_directories(root_);
    std::filesystem::create_directories(archive_root_);
    load_manifest();
}

ModelStore::ModelStore(std::filesystem::path root, std::string token,
                       model::BackendCoordinator& coordinator,
                       std::unique_ptr<IModelStoreTransport> transport)
    : ModelStore(root, root.parent_path() / "archive", std::move(token),
                 coordinator, std::move(transport)) {}

ModelStore::~ModelStore() {
    std::vector<std::thread> workers;
    {
        std::lock_guard lock(mutex_);
        for (auto& [_, cancel] : cancellations_) cancel->store(true);
        for (auto& [_, worker] : workers_) workers.push_back(std::move(worker));
    }
    for (auto& worker : workers) if (worker.joinable()) worker.join();
}

#include "model_store_discovery.ipp"

#include "model_store_downloads.ipp"

#include "model_store_library.ipp"

#include "model_store_transport.ipp"

}
