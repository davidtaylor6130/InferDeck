#include "routes/AudioSpeech.hpp"
#include "core/Logger.hpp"
#include <nlohmann/json.hpp>
namespace inferdeck::gateway::routes {
void HandleAudioSpeech(const httplib::Request& req, httplib::Response& resp) {
    resp.status = 501;
    resp.set_content(R"({"error":"Not implemented"})", "application/json");
}
}
