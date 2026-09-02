#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

#include <httplib.h>

#include "gateway/routes.hpp"

namespace inferdeck::gateway {

struct MediaJobOutput {
    std::string body;
    std::string content_type;
    std::string filename;
};

void handle_image_generations(const httplib::Request& req, httplib::Response& resp,
                              const GatewayDeps& deps);
void handle_audio_generations(const httplib::Request& req,
                              httplib::Response& resp,
                              const GatewayDeps& deps);
void handle_audio_speech(const httplib::Request& req, httplib::Response& resp,
                         const GatewayDeps& deps);
void handle_audio_transcriptions(const httplib::Request& req, httplib::Response& resp,
                                 const GatewayDeps& deps);
foundation::Result<void> configure_media_history(
    const std::filesystem::path& directory);
nlohmann::json media_jobs();
foundation::Result<MediaJobOutput> media_job_output(
    std::uint64_t id, std::size_t index);
foundation::Result<void> cancel_media_job(std::uint64_t id);

}
