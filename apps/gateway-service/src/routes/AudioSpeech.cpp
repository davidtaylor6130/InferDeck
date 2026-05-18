/// @file AudioSpeech.cpp
/// @brief /v1/audio/speech route handler for TTS.

#include "AudioSpeech.hpp"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace inferdeck::gateway::routes {

std::string ValidateSpeechRequest(const nlohmann::json& body) {
    if (!body.contains("input") || !body["input"].is_string() || body["input"].get<std::string>().empty()) {
        return "missing_or_empty_input";
    }
    if (!body.contains("model")) {
        return "missing_model";
    }
    return ""; // valid
}

void HandleAudioSpeech(const httplib::Request& req, httplib::Response& resp) {
    // Parse body
    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req.body);
    } catch (const std::exception& e) {
        nlohmann::json error;
        error["error"]["message"] = "Invalid JSON body";
        error["error"]["type"] = "invalid_request_error";
        resp.status = 400;
        resp.set_content(error.dump(), "application/json");
        return;
    }

    // Validate
    auto validation = ValidateSpeechRequest(body);
    if (!validation.empty()) {
        nlohmann::json error;
        if (validation == "missing_or_empty_input") {
            error["error"]["message"] = "'input' is required and must be non-empty string";
        } else if (validation == "missing_model") {
            error["error"]["message"] = "'model' is required";
        }
        error["error"]["type"] = "invalid_request_error";
        resp.status = 400;
        resp.set_content(error.dump(), "application/json");
        return;
    }

    std::string model = body["model"].get<std::string>();
    std::string input = body["input"].get<std::string>();
    std::string voice = body.value("voice", "alloy");
    std::string response_format = body.value("response_format", "wav");

    // Check TTS backend
    // auto& registry = backends::BackendRegistry::Instance();
    // auto tts = registry.GetTtsBackend("piper");
    // if (!tts || !tts->IsReady()) {
    //     nlohmann::json error;
    //     error["error"]["message"] = "TTS backend not available";
    //     error["error"]["type"] = "service_unavailable";
    //     resp.status = 503;
    //     resp.set_content(error.dump(), "application/json");
    //     return;
    // }

    // Generate speech
    // auto wav_data = tts->Synthesize(input, {{ "voice", voice }});

    // Return simulated WAV bytes
    std::string output_data;
    // output_data.assign(reinterpret_cast<const char*>(wav_data.data()), wav_data.size());

    resp.status = 200;
    resp.set_content(output_data, "audio/" + response_format);
    spdlog::info("AudioSpeech: TTS '{}' -> '{}' (model: {}, voice: {})",
                 input.substr(0, 50), response_format, model, voice);
}

} // namespace inferdeck::gateway::routes
