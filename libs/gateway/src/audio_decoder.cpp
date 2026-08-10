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
        static_cast<ma_uint64>(sample_rate) * 60ULL * 30ULL;
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

    ma_uint64 frame_count = 0;
    const auto length_result =
        ma_decoder_get_length_in_pcm_frames(&decoder, &frame_count);
    if (length_result != MA_SUCCESS || frame_count == 0 ||
        frame_count > max_frames ||
        frame_count > (std::numeric_limits<std::size_t>::max)()) {
        ma_decoder_uninit(&decoder);
        return foundation::Err<model::TranscriptionRequest>(
            foundation::ErrorCode::InvalidArgument,
            "audio duration is invalid or exceeds 30 minutes");
    }

    model::TranscriptionRequest request;
    request.sample_rate = static_cast<int>(sample_rate);
    request.pcm.resize(static_cast<std::size_t>(frame_count));
    ma_uint64 frames_read = 0;
    const auto read_result = ma_decoder_read_pcm_frames(
        &decoder, request.pcm.data(), frame_count, &frames_read);
    ma_decoder_uninit(&decoder);
    if (read_result != MA_SUCCESS || frames_read == 0) {
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
