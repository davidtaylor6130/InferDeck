/// @file AudioTranscriptions.cpp
/// @brief /v1/audio/transcriptions and /v1/audio/translations route handlers.

#include "AudioTranscriptions.hpp"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <fstream>
#include <sstream>
#include <filesystem>

namespace inferdeck::gateway::routes {

std::string ValidateAudioRequest(const httplib::Request& req, bool is_translation) {
    if (req.get_header_value("Content-Type").find("multipart/form-data") == std::string::npos) {
        nlohmann::json error;
        error["error"]["message"] = "Content-Type must be multipart/form-data";
        error["error"]["type"] = "invalid_request_error";
        resp.status = 400;
        resp.set_content(error.dump(), "application/json");
        return "invalid_content_type";
    }

    // Check file upload
    auto file_iter = req.files.find("file");
    if (file_iter == req.files.end()) {
        nlohmann::json error;
        error["error"]["message"] = "No 'file' field in multipart request";
        error["error"]["type"] = "invalid_request_error";
        resp.status = 400;
        resp.set_content(error.dump(), "application/json");
        return "missing_file";
    }

    // Check model parameter (optional)
    return ""; // validation passed
}

void HandleAudioTranscriptions(const httplib::Request& req, httplib::Response& resp) {
    // Validate content type
    if (req.get_header_value("Content-Type").find("multipart/form-data") == std::string::npos) {
        nlohmann::json error;
        error["error"]["message"] = "Content-Type must be multipart/form-data";
        error["error"]["type"] = "invalid_request_error";
        resp.status = 400;
        resp.set_content(error.dump(), "application/json");
        return;
    }

    // Check file upload
    auto file_iter = req.files.find("file");
    if (file_iter == req.files.end()) {
        nlohmann::json error;
        error["error"]["message"] = "No 'file' field in multipart request";
        error["error"]["type"] = "invalid_request_error";
        resp.status = 400;
        resp.set_content(error.dump(), "application/json");
        return;
    }

    auto& audio_file = file_iter->second;

    // Extract optional parameters
    std::string model = req.get_param_value("model");
    if (model.empty()) model = "whisper";
    std::string language = req.get_param_value("language");
    std::string prompt = req.get_param_value("prompt");
    std::string response_format = req.get_param_value("response_format");
    if (response_format.empty()) response_format = "json";

    // Get STT backend from registry
    // auto& registry = backends::BackendRegistry::Instance();
    // auto stt_backend = registry.GetSttBackend("whisper");
    // if (!stt_backend || !stt_backend->IsReady()) {
    //     nlohmann::json error;
    //     error["error"]["message"] = "STT backend not available";
    //     error["error"]["type"] = "service_unavailable";
    //     resp.status = 503;
    //     resp.set_content(error.dump(), "application/json");
    //     return;
    // }

    // Write temp file for whisper processing
    std::string tmp_path = "data/audio/tmp_" + std::to_string(std::time(nullptr)) + ".wav";
    std::filesystem::create_directories("data/audio");
    std::ofstream ofs(tmp_path, std::ios::binary);
    ofs.write(audio_file.data.data(), audio_file.data.size());
    ofs.close();

    // STT result (simulated)
    nlohmann::json result;
    result["task"] = "transcribe";
    result["language"] = language.empty() ? "en" : language;
    result["duration"] = 0.0;
    result["text"] = "[audio transcription from '" + audio_file.filename + "']";
    result["model"] = model;

    if (response_format == "verbose_json") {
        result["segments"] = nlohmann::json::array();
        result["no_speech_prob"] = 0.0;
    }

    resp.status = 200;
    resp.set_content(result.dump(), "application/json");
    spdlog::info("AudioTranscriptions: transcribed '{}' (model: {}, lang: {})",
                 audio_file.filename, model, language);
}

void HandleAudioTranslations(const httplib::Request& req, httplib::Response& resp) {
    // Validate content type
    if (req.get_header_value("Content-Type").find("multipart/form-data") == std::string::npos) {
        nlohmann::json error;
        error["error"]["message"] = "Content-Type must be multipart/form-data";
        error["error"]["type"] = "invalid_request_error";
        resp.status = 400;
        resp.set_content(error.dump(), "application/json");
        return;
    }

    auto file_iter = req.files.find("file");
    if (file_iter == req.files.end()) {
        nlohmann::json error;
        error["error"]["message"] = "No 'file' field in multipart request";
        error["error"]["type"] = "invalid_request_error";
        resp.status = 400;
        resp.set_content(error.dump(), "application/json");
        return;
    }

    auto& audio_file = file_iter->second;

    std::string model = req.get_param_value("model");
    if (model.empty()) model = "whisper";
    std::string prompt = req.get_param_value("prompt");

    nlohmann::json result;
    result["task"] = "translate";
    result["language"] = "en";
    result["duration"] = 0.0;
    result["text"] = "[audio translated from '" + audio_file.filename + "']";
    result["model"] = model;

    resp.status = 200;
    resp.set_content(result.dump(), "application/json");
    spdlog::info("AudioTranslations: translated '{}' (model: {})", audio_file.filename, model);
}

} // namespace inferdeck::gateway::routes
