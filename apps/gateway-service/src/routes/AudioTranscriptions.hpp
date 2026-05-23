#pragma once
#include <httplib.h>
#include <nlohmann/json.hpp>
namespace inferdeck::gateway::routes {
std::string ValidateAudioRequest(const httplib::Request& req, bool require_file = true);
void HandleAudioTranscriptions(const httplib::Request& req, httplib::Response& resp);
void HandleAudioTranslations(const httplib::Request& req, httplib::Response& resp);
}
