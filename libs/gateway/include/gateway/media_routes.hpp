#pragma once

#include <httplib.h>

#include "gateway/routes.hpp"

namespace inferdeck::gateway {

void handle_image_generations(const httplib::Request& req, httplib::Response& resp,
                              const GatewayDeps& deps);
void handle_audio_speech(const httplib::Request& req, httplib::Response& resp,
                         const GatewayDeps& deps);
void handle_audio_transcriptions(const httplib::Request& req, httplib::Response& resp,
                                 const GatewayDeps& deps);
nlohmann::json media_jobs();
foundation::Result<void> cancel_media_job(std::uint64_t id);

}
