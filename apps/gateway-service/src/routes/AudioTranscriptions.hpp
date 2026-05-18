/// @file AudioTranscriptions.hpp
/// @brief /v1/audio/transcriptions and /v1/audio/translations route handlers.

#pragma once

#include <httplib.h>
#include <string>

namespace inferdeck::gateway::routes {

/// Handle POST /v1/audio/transcriptions
void HandleAudioTranscriptions(const httplib::Request& req, httplib::Response& resp);

/// Handle POST /v1/audio/translations
void HandleAudioTranslations(const httplib::Request& req, httplib::Response& resp);

/// Validate audio transcription request (for multipart).
std::string ValidateAudioRequest(const httplib::Request& req, bool is_translation);

} // namespace inferdeck::gateway::routes
