#include "routes/AudioTranscriptions.hpp"

#include "WhisperRuntime.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>

using json = nlohmann::json;

namespace inferdeck::gateway::routes {
namespace {

void JsonResponse(httplib::Response& resp, const json& body, int status = 200) {
    resp.status = status;
    resp.set_content(body.dump(), "application/json");
}

void OpenAiError(httplib::Response& resp, int status, const std::string& code, const std::string& message) {
    JsonResponse(resp, {{"error", {{"message", message}, {"type", "invalid_request_error"}, {"code", code}}}}, status);
}

std::string Param(const httplib::Request& req, const std::string& name, const std::string& fallback = "") {
    if (req.form.has_field(name)) return req.form.get_field(name);
    return req.has_param(name) ? req.get_param_value(name) : fallback;
}

std::filesystem::path SaveUploadedFile(const httplib::Request& req) {
    const auto file = req.form.get_file("file");
    auto filename = file.filename.empty() ? "audio.wav" : file.filename;
    auto ext = std::filesystem::path(filename).extension().string();
    if (ext.empty()) ext = ".wav";
    auto path = std::filesystem::temp_directory_path() /
        ("inferdeck-audio-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ext);
    std::ofstream out(path, std::ios::binary);
    out << file.content;
    return path;
}

void HandleAudio(const httplib::Request& req, httplib::Response& resp, bool translate) {
    auto validation = ValidateAudioRequest(req, true);
    if (!validation.empty()) {
        if (validation == "invalid_content_type") {
            OpenAiError(resp, 400, validation, "Content-Type must be multipart/form-data");
        } else {
            OpenAiError(resp, 400, validation, "Audio file is required in multipart field 'file'.");
        }
        return;
    }

    auto audio_path = SaveUploadedFile(req);
    json params = {
        {"model", Param(req, "model", "whisper")},
        {"language", Param(req, "language", "")},
        {"prompt", Param(req, "prompt", "")},
        {"response_format", Param(req, "response_format", "json")},
        {"temperature", Param(req, "temperature", "0")},
        {"task", translate ? "translate" : Param(req, "task", "transcribe")}
    };

    auto result = inferdeck::gateway::WhisperRuntime::Get().Transcribe(audio_path.string(), params, translate);
    std::error_code ec;
    std::filesystem::remove(audio_path, ec);

    if (!result.ok) {
        OpenAiError(resp, 503, result.error_code.empty() ? "transcription_failed" : result.error_code,
            result.error_message.empty() ? "Whisper transcription failed." : result.error_message);
        return;
    }

    auto response_format = params.value("response_format", std::string{"json"});
    if (response_format == "text") {
        resp.status = 200;
        resp.set_content(result.text, "text/plain");
        return;
    }

    json body = {
        {"text", result.text},
        {"model", params["model"]},
        {"task", translate ? "translate" : "transcribe"},
        {"language", translate ? "en" : (result.language.empty() ? json(nullptr) : json(result.language))},
        {"duration", result.duration_seconds}
    };
    if (response_format == "verbose_json") {
        body["segments"] = json::array({{{"id", 0}, {"start", 0.0}, {"end", result.duration_seconds}, {"text", result.text}}});
        body["no_speech_prob"] = 0.0;
    }
    JsonResponse(resp, body);
}

} // namespace

std::string ValidateAudioRequest(const httplib::Request& req, bool require_file) {
    const auto content_type = req.get_header_value("Content-Type");
    if (content_type.find("multipart/form-data") == std::string::npos) return "invalid_content_type";
    if (require_file && !req.form.has_file("file")) return "missing_file";
    return "";
}

void HandleAudioTranscriptions(const httplib::Request& req, httplib::Response& resp) {
    HandleAudio(req, resp, false);
}

void HandleAudioTranslations(const httplib::Request& req, httplib::Response& resp) {
    HandleAudio(req, resp, true);
}

} // namespace inferdeck::gateway::routes
