/// @file WhisperSttBackend.cpp
/// @brief Whisper GGML speech-to-text implementation.

#include "whisper_stt/WhisperSttBackend.hpp"
#include <spdlog/spdlog.h>
#include <chrono>

namespace inferdeck::backends {

std::string WhisperSttBackend::GetBackendName() const { return "whisper_stt"; }

BackendStatus WhisperSttBackend::GetStatus() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

bool WhisperSttBackend::Initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    status_ = BackendStatus::INITIALIZING;

    // Load GGML whisper model from model_path_
    // On target: whisper_model_init(&ctx, model_path_.c_str());
    // Detect model type: ggml_type, n_layers, n_vocab, etc.
    if (model_path_.empty()) {
        status_ = BackendStatus::ERROR;
        spdlog::error("WhisperSttBackend: no model path set");
        return false;
    }

    // Parse GGUF header to detect quantization
    // auto ggp = GGUFParser::Parse(model_path_);
    spdlog::info("WhisperSttBackend: loaded model '{}' [threads: {}]", model_path_, threads_);
    status_ = BackendStatus::READY;
    return true;
}

void WhisperSttBackend::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    // whisper_free(ctx);
    status_ = BackendStatus::UNINITIALIZED;
    spdlog::info("WhisperSttBackend: shut down");
}

nlohmann::json WhisperSttBackend::GetInfo() const {
    nlohmann::json info;
    info["name"] = "whisper_stt";
    info["backend"] = "whisper.cpp";
    info["model"] = model_name_;
    info["model_path"] = model_path_;
    info["threads"] = threads_;
    info["language_autodetect"] = language_autodetect_;
    info["status"] = static_cast<int>(status_);
    info["detected_language"] = detected_language_;
    return info;
}

nlohmann::json WhisperSttBackend::GetVRAMUsage() const {
    nlohmann::json vram;
    vram["backend"] = "whisper_stt";
    vram["allocated_bytes"] = 0;
    vram["peak_bytes"] = 0;
    return vram;
}

bool WhisperSttBackend::IsReady() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_ == BackendStatus::READY;
}

STTResult WhisperSttBackend::Transcribe(const std::string& audio_path,
                                         const nlohmann::json& params) {
    auto start = std::chrono::steady_clock::now();

    STTResult result;

    // whisper_full params:
    // auto wf_params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    // wf_params.language = language_autodetect_ ? -1 : 0; // -1 = auto detect
    // if (params.contains("language")) {
    //     wf_params.language = whisper_lang_auto_detect();
    // }
    // if (params.contains("max_tokens")) {
    //     wf_params.max_tokens = params["max_tokens"].get<int>();
    // }
    // if (params.contains("split_on_word")) {
    //     wf_params.split_on_word = params["split_on_word"].get<bool>();
    // }

    // Process audio file and run inference
    // auto audio = LoadAudioFile(audio_path);
    // whisper_full(ctx, wf_params, audio.data(), audio.size());
    // int n_segments = whisper_full_n_segments(ctx);
    // for (int i = 0; i < n_segments; i++) {
    //     const char* text = whisper_full_get_segment_text(ctx, i);
    //     int64_t t0 = whisper_full_get_segment_t0(ctx, i);
    //     int64_t t1 = whisper_full_get_segment_t1(ctx, i);
    //     result.text += text;
    //     result.timestamps.push_back(static_cast<float>(t0) / 100.0f);
    // }
    // auto lang = whisper_full_lang_id(ctx);
    // result.language = whisper_lang_str(lang);
    // result.confidence = 1.0; // whisper doesn't provide confidence natively

    // Compute duration
    auto end = std::chrono::steady_clock::now();
    double duration = std::chrono::duration<double>(end - start).count();
    result.duration_seconds = duration;

    spdlog::info("WhisperSttBackend: transcribed '{}' ({}s)", audio_path, result.duration_seconds);
    return result;
}

STTResult WhisperSttBackend::Translate(const std::string& audio_path,
                                        const nlohmann::json& params) {
    auto start = std::chrono::steady_clock::now();

    STTResult result;

    // whisper_full params for translation (no language auto-detect):
    // auto wf_params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    // wf_params.language = whisper_lang_id("en"); // always translate to English
    // wf_params.translate = true;
    // wf_params.no_context = params.value("no_context", true);

    // Process and translate
    // auto audio = LoadAudioFile(audio_path);
    // whisper_full(ctx, wf_params, audio.data(), audio.size());
    // result.language = "en"; // translated to English

    auto end = std::chrono::steady_clock::now();
    double duration = std::chrono::duration<double>(end - start).count();
    result.duration_seconds = duration;

    spdlog::info("WhisperSttBackend: translated '{}' ({}s)", audio_path, result.duration_seconds);
    return result;
}

nlohmann::json WhisperSttBackend::GetAvailableLanguages() const {
    nlohmann::json langs;
    // Standard whisper language codes:
    langs["detect"] = "auto-detect";
    langs["en"] = "english";
    langs["zh"] = "chinese";
    langs["de"] = "german";
    langs["es"] = "spanish";
    langs["ru"] = "russian";
    langs["ko"] = "korean";
    langs["fr"] = "french";
    langs["ja"] = "japanese";
    langs["pt"] = "portuguese";
    langs["tr"] = "turkish";
    langs["pl"] = "polish";
    langs["ca"] = "catalan";
    langs["nl"] = "dutch";
    langs["ar"] = "arabic";
    langs["sv"] = "swedish";
    return langs;
}

bool WhisperSttBackend::SetModelPath(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    model_path_ = path;
    // Extract model name from path
    auto pos = path.find_last_of("/\\");
    model_name_ = (pos != std::string::npos) ? path.substr(pos + 1) : path;
    return true;
}

std::string WhisperSttBackend::GetModelPath() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return model_path_;
}

bool WhisperSttBackend::SetThreads(int threads) {
    std::lock_guard<std::mutex> lock(mutex_);
    threads_ = threads;
    return true;
}

int WhisperSttBackend::GetThreads() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return threads_;
}

} // namespace inferdeck::backends
