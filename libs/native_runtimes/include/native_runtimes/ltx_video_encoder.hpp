#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <stable-diffusion.h>

#include "foundation/result.hpp"

namespace inferdeck::native_runtimes {

const char* ltx_video_content_type() noexcept;

#if defined(_WIN32)
foundation::Result<std::vector<std::byte>> encode_ltx_video_mp4(
    sd_image_t* frames, int frame_count, int fps, const sd_audio_t* audio);
#endif

foundation::Result<std::vector<std::byte>> encode_ltx_video(
    sd_image_t* frames, int frame_count, int fps, const sd_audio_t* audio);

}
