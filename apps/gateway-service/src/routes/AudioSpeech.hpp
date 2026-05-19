#pragma once
#include <httplib.h>
#include <nlohmann/json.hpp>
namespace inferdeck::gateway::routes {
void HandleAudioSpeech(const httplib::Request& req, httplib::Response& resp);
}
