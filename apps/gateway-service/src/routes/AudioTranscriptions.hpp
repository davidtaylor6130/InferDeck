#pragma once
#include <httplib.h>
#include <nlohmann/json.hpp>
namespace inferdeck::gateway::routes {
void HandleAudioTranscriptions(const httplib::Request& req, httplib::Response& resp);
void HandleAudioTranslations(const httplib::Request& req, httplib::Response& resp);
}
