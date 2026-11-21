#include "audio_decoder.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#define MA_NO_ENCODING
#define MA_NO_DEVICE_IO
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MA_NO_GENERATION
#define MA_NO_STDIO
#define MA_API static
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio/miniaudio.h"

namespace inferdeck::gateway {

foundation::Result<model::TranscriptionRequest> decode_compressed_audio(
    std::string_view content) {
    constexpr ma_uint32 sample_rate = 16000;
    constexpr ma_uint64 max_frames =
        static_cast<ma_uint64>(sample_rate) * 60ULL * 240ULL;
    ma_decoder_config config =
        ma_decoder_config_init(ma_format_f32, 1, sample_rate);
    ma_decoder decoder{};
    const auto initialized = ma_decoder_init_memory(
        content.data(), content.size(), &config, &decoder);
    if (initialized != MA_SUCCESS) {
        return foundation::Err<model::TranscriptionRequest>(
            foundation::ErrorCode::InvalidArgument,
            "audio must be RIFF/WAVE, MP3, or FLAC");
    }

    model::TranscriptionRequest request;
    request.sample_rate = static_cast<int>(sample_rate);
    constexpr ma_uint64 block_frames = 1ULL << 16;
    ma_uint64 frames_read = 0;
    bool read_failed = false;
    while (frames_read < max_frames) {
        const auto old_size = request.pcm.size();
        request.pcm.resize(old_size + block_frames);
        ma_uint64 got = 0;
        const auto read_result = ma_decoder_read_pcm_frames(
            &decoder, request.pcm.data() + old_size, block_frames, &got);
        request.pcm.resize(old_size + static_cast<std::size_t>(got));
        frames_read += got;
        if (read_result != MA_SUCCESS && read_result != MA_AT_END) read_failed = true;
        if (got < block_frames || read_failed) break;
    }
    const bool too_long = frames_read >= max_frames;
    ma_decoder_uninit(&decoder);
    if (too_long) {
        return foundation::Err<model::TranscriptionRequest>(
            foundation::ErrorCode::InvalidArgument,
            "audio duration is invalid or exceeds 4 hours");
    }
    if (read_failed || frames_read == 0) {
        return foundation::Err<model::TranscriptionRequest>(
            foundation::ErrorCode::InvalidArgument,
            "compressed audio could not be decoded");
    }
    request.pcm.resize(static_cast<std::size_t>(frames_read));
    if (std::any_of(request.pcm.begin(), request.pcm.end(),
                    [](float sample) { return !std::isfinite(sample); })) {
        return foundation::Err<model::TranscriptionRequest>(
            foundation::ErrorCode::InvalidArgument,
            "decoded audio samples must be finite");
    }
    return foundation::Ok(std::move(request));
}

}
