/// @file AudioSpeech.hpp
/// @brief /v1/audio/speech route handler.

#pragma once

#include <httplib.h>

namespace inferdeck::gateway::routes {

/// Handle POST /v1/audio/speech
void HandleAudioSpeech(const httplib::Request& req, httplib::Response& resp);

/// Validate TTS request body.
std::string ValidateSpeechRequest(const nlohmann::json& body);

} // namespace inferdeck::gateway::routes
