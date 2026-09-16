#include "native_runtimes/ltx_video_encoder.hpp"

#include "foundation/result.hpp"
#include "media_io.h"

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace inferdeck::native_runtimes {

const char* ltx_video_content_type() noexcept {
#if defined(_WIN32)
    return "video/mp4";
#else
    return "video/x-msvideo";
#endif
}

#if defined(_WIN32)
namespace {
using Microsoft::WRL::ComPtr;
constexpr std::size_t kMaxEncodedBytes = 25u * 1024u * 1024u;

template <typename T>
foundation::Result<T> mf_error(const char* operation, HRESULT code) {
    char message[128]{};
    std::snprintf(message, sizeof(message), "%s failed (HRESULT 0x%08lX)",
                  operation, static_cast<unsigned long>(code));
    return std::unexpected(foundation::Error{
        foundation::ErrorCode::Internal, message});
}

class MediaFoundationSession final {
public:
    MediaFoundationSession() {
        const HRESULT com_code = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        com_initialized_ = SUCCEEDED(com_code);
        if (FAILED(com_code) && com_code != RPC_E_CHANGED_MODE) {
            code_ = com_code;
            return;
        }
        code_ = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    }
    ~MediaFoundationSession() {
        if (SUCCEEDED(code_)) MFShutdown();
        if (com_initialized_) CoUninitialize();
    }
    HRESULT code() const noexcept { return code_; }
private:
    HRESULT code_{E_FAIL};
    bool com_initialized_{false};
};

class TemporaryMp4 final {
public:
    TemporaryMp4() {
        wchar_t directory[MAX_PATH]{};
        const DWORD length = GetTempPathW(MAX_PATH, directory);
        if (length == 0 || length >= MAX_PATH) return;
        wchar_t filename[MAX_PATH]{};
        if (GetTempFileNameW(directory, L"ltx", 0, filename) == 0) return;
        path_ = filename;
    }
    ~TemporaryMp4() {
        if (!path_.empty()) DeleteFileW(path_.c_str());
    }
    bool valid() const noexcept { return !path_.empty(); }
    const std::wstring& path() const noexcept { return path_; }
private:
    std::wstring path_;
};

std::uint8_t clamp_byte(int value) {
    return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
}

std::size_t nv12_size(std::uint32_t width, std::uint32_t height) {
    const std::size_t y_size = static_cast<std::size_t>(width) * height;
    return y_size + y_size / 2;
}

void rgb_to_nv12(const sd_image_t& frame, std::vector<std::uint8_t>& output) {
    const auto width = frame.width;
    const auto height = frame.height;
    const std::size_t y_size = static_cast<std::size_t>(width) * height;
    output.resize(nv12_size(width, height));
    auto* y_plane = output.data();
    auto* uv_plane = output.data() + y_size;
    const auto pixel = [&frame, width](std::uint32_t x, std::uint32_t y) {
        return frame.data + (static_cast<std::size_t>(y) * width + x) * frame.channel;
    };
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto* rgb = pixel(x, y);
            y_plane[static_cast<std::size_t>(y) * width + x] = clamp_byte(
                ((66 * rgb[0] + 129 * rgb[1] + 25 * rgb[2] + 128) >> 8) + 16);
        }
    }
    for (std::uint32_t y = 0; y < height; y += 2) {
        for (std::uint32_t x = 0; x < width; x += 2) {
            int red = 0;
            int green = 0;
            int blue = 0;
            for (std::uint32_t dy = 0; dy < 2; ++dy) {
                for (std::uint32_t dx = 0; dx < 2; ++dx) {
                    const auto* rgb = pixel(x + dx, y + dy);
                    red += rgb[0]; green += rgb[1]; blue += rgb[2];
                }
            }
            red /= 4; green /= 4; blue /= 4;
            const std::size_t uv = static_cast<std::size_t>(y) / 2 * width + x;
            uv_plane[uv] = clamp_byte(((-38 * red - 74 * green + 112 * blue + 128) >> 8) + 128);
            uv_plane[uv + 1] = clamp_byte(((112 * red - 94 * green - 18 * blue + 128) >> 8) + 128);
        }
    }
}

foundation::Result<ComPtr<IMFSample>> make_sample(const std::uint8_t* data,
                                                    std::size_t size,
                                                    LONGLONG time,
                                                    LONGLONG duration) {
    if (size > std::numeric_limits<DWORD>::max()) {
        return foundation::Err<ComPtr<IMFSample>>(
            foundation::ErrorCode::Unavailable,
            "Media Foundation sample exceeds the 32-bit buffer limit");
    }
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT code = MFCreateMemoryBuffer(static_cast<DWORD>(size), &buffer);
    if (FAILED(code)) return mf_error<ComPtr<IMFSample>>("MFCreateMemoryBuffer", code);
    BYTE* destination = nullptr;
    code = buffer->Lock(&destination, nullptr, nullptr);
    if (FAILED(code)) return mf_error<ComPtr<IMFSample>>("IMFMediaBuffer::Lock", code);
    std::memcpy(destination, data, size);
    buffer->Unlock();
    code = buffer->SetCurrentLength(static_cast<DWORD>(size));
    if (FAILED(code)) return mf_error<ComPtr<IMFSample>>("IMFMediaBuffer::SetCurrentLength", code);
    ComPtr<IMFSample> sample;
    code = MFCreateSample(&sample);
    if (FAILED(code)) return mf_error<ComPtr<IMFSample>>("MFCreateSample", code);
    code = sample->AddBuffer(buffer.Get());
    if (FAILED(code)) return mf_error<ComPtr<IMFSample>>("IMFSample::AddBuffer", code);
    code = sample->SetSampleTime(time);
    if (FAILED(code)) return mf_error<ComPtr<IMFSample>>("IMFSample::SetSampleTime", code);
    code = sample->SetSampleDuration(duration);
    if (FAILED(code)) return mf_error<ComPtr<IMFSample>>("IMFSample::SetSampleDuration", code);
    return foundation::Ok(std::move(sample));
}

foundation::Result<std::vector<std::byte>> encode_mp4(
    sd_image_t* frames, int frame_count, int fps, const sd_audio_t* audio) {
    if (frames == nullptr || frame_count <= 0 || fps <= 0) {
        return foundation::Err<std::vector<std::byte>>(
            foundation::ErrorCode::InvalidArgument,
            "video frames and fps must be valid");
    }
    const std::uint32_t width = frames[0].width;
    const std::uint32_t height = frames[0].height;
    if (width < 64 || height < 64 || (width & 1u) != 0 || (height & 1u) != 0 ||
        frames[0].channel < 3 || width > 8192 || height > 8192 ||
        static_cast<std::uint64_t>(width) * height > 64u * 1024u * 1024u ||
        nv12_size(width, height) > std::numeric_limits<DWORD>::max()) {
        return foundation::Err<std::vector<std::byte>>(
            foundation::ErrorCode::InvalidArgument,
            "video frames must be even-sized RGB images within limits");
    }
    for (int index = 0; index < frame_count; ++index) {
        if (frames[index].data == nullptr || frames[index].width != width ||
            frames[index].height != height || frames[index].channel < 3) {
            return foundation::Err<std::vector<std::byte>>(
                foundation::ErrorCode::InvalidArgument,
                "video frames must have matching RGB dimensions");
        }
    }
    const bool has_audio = audio != nullptr;
    std::vector<std::uint8_t> pcm;
    if (has_audio) {
        if (audio->data == nullptr || audio->sample_count == 0 || audio->channels == 0 ||
            (audio->channels != 1 && audio->channels != 2 && audio->channels != 6) ||
            (audio->sample_rate != 44100 && audio->sample_rate != 48000) ||
            audio->sample_count > std::numeric_limits<DWORD>::max() /
                (static_cast<std::uint64_t>(audio->channels) * sizeof(std::int16_t))) {
            return foundation::Err<std::vector<std::byte>>(
                foundation::ErrorCode::InvalidArgument,
                "audio must contain supported interleaved PCM samples");
        }
        const std::size_t count = static_cast<std::size_t>(audio->sample_count) * audio->channels;
        pcm.resize(count * sizeof(std::int16_t));
        auto* samples = reinterpret_cast<std::int16_t*>(pcm.data());
        for (std::size_t index = 0; index < count; ++index) {
            const float value = std::isfinite(audio->data[index])
                ? std::clamp(audio->data[index], -1.0f, 1.0f) : 0.0f;
            samples[index] = static_cast<std::int16_t>(std::lround(value * 32767.0f));
        }
    }
    MediaFoundationSession session;
    if (FAILED(session.code())) return mf_error<std::vector<std::byte>>(
        "Media Foundation initialization", session.code());
    TemporaryMp4 output;
    if (!output.valid()) return foundation::Err<std::vector<std::byte>>(
        foundation::ErrorCode::Internal, "could not create a temporary MP4 file");

    ComPtr<IMFAttributes> attributes;
    HRESULT code = MFCreateAttributes(&attributes, 2);
    if (FAILED(code)) return mf_error<std::vector<std::byte>>("MFCreateAttributes", code);
    code = attributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
    if (FAILED(code)) return mf_error<std::vector<std::byte>>("Set container type", code);
    code = attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    if (FAILED(code)) return mf_error<std::vector<std::byte>>("Set sink writer attributes", code);
    ComPtr<IMFSinkWriter> writer;
    code = MFCreateSinkWriterFromURL(output.path().c_str(), nullptr, attributes.Get(), &writer);
    if (FAILED(code)) return mf_error<std::vector<std::byte>>("MFCreateSinkWriterFromURL", code);

    ComPtr<IMFMediaType> video_output;
    code = MFCreateMediaType(&video_output);
    if (FAILED(code)) return mf_error<std::vector<std::byte>>("MFCreateMediaType", code);
    video_output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    video_output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    MFSetAttributeSize(video_output.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(video_output.Get(), MF_MT_FRAME_RATE, static_cast<UINT32>(fps), 1);
    MFSetAttributeRatio(video_output.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    video_output->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    video_output->SetUINT32(MF_MT_AVG_BITRATE, 4u * 1000u * 1000u);
    DWORD video_stream = 0;
    code = writer->AddStream(video_output.Get(), &video_stream);
    if (FAILED(code)) return mf_error<std::vector<std::byte>>("Add video stream", code);

    ComPtr<IMFMediaType> video_input;
    code = MFCreateMediaType(&video_input);
    if (FAILED(code)) return mf_error<std::vector<std::byte>>("MFCreateMediaType", code);
    video_input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    video_input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(video_input.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(video_input.Get(), MF_MT_FRAME_RATE, static_cast<UINT32>(fps), 1);
    MFSetAttributeRatio(video_input.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    video_input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    code = writer->SetInputMediaType(video_stream, video_input.Get(), nullptr);
    if (FAILED(code)) return mf_error<std::vector<std::byte>>("Set video input type", code);

    DWORD audio_stream = 0;
    if (has_audio) {
        ComPtr<IMFMediaType> audio_output;
        code = MFCreateMediaType(&audio_output);
        if (FAILED(code)) return mf_error<std::vector<std::byte>>("MFCreateMediaType", code);
        audio_output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        audio_output->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        audio_output->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, audio->channels);
        audio_output->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, audio->sample_rate);
        audio_output->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        audio_output->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 16000);
        audio_output->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 1);
        audio_output->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
        code = writer->AddStream(audio_output.Get(), &audio_stream);
        if (FAILED(code)) return mf_error<std::vector<std::byte>>("Add audio stream", code);

        ComPtr<IMFMediaType> audio_input;
        code = MFCreateMediaType(&audio_input);
        if (FAILED(code)) return mf_error<std::vector<std::byte>>("MFCreateMediaType", code);
        const UINT32 alignment = audio->channels * sizeof(std::int16_t);
        audio_input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        audio_input->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        audio_input->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, audio->channels);
        audio_input->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, audio->sample_rate);
        audio_input->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        audio_input->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, alignment);
        audio_input->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, audio->sample_rate * alignment);
        audio_input->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
        code = writer->SetInputMediaType(audio_stream, audio_input.Get(), nullptr);
        if (FAILED(code)) return mf_error<std::vector<std::byte>>("Set audio input type", code);
    }
    code = writer->BeginWriting();
    if (FAILED(code)) return mf_error<std::vector<std::byte>>("Begin writing", code);
    if (has_audio) {
        const LONGLONG duration = static_cast<LONGLONG>(
            (audio->sample_count * 10'000'000ULL) / audio->sample_rate);
        const auto sample = make_sample(pcm.data(), pcm.size(), 0, duration);
        if (!sample) return std::unexpected(sample.error());
        code = writer->WriteSample(audio_stream, sample->Get());
        if (FAILED(code)) return mf_error<std::vector<std::byte>>("Write audio sample", code);
    }
    std::vector<std::uint8_t> nv12;
    for (int index = 0; index < frame_count; ++index) {
        rgb_to_nv12(frames[index], nv12);
        const LONGLONG start = (static_cast<LONGLONG>(index) * 10'000'000LL) / fps;
        const LONGLONG end = (static_cast<LONGLONG>(index + 1) * 10'000'000LL) / fps;
        const auto sample = make_sample(nv12.data(), nv12.size(), start, end - start);
        if (!sample) return std::unexpected(sample.error());
        code = writer->WriteSample(video_stream, sample->Get());
        if (FAILED(code)) return mf_error<std::vector<std::byte>>("Write video sample", code);
    }
    code = writer->Finalize();
    if (FAILED(code)) return mf_error<std::vector<std::byte>>("Finalize MP4", code);
    writer.Reset();
    std::ifstream file(output.path(), std::ios::binary | std::ios::ate);
    if (!file) return foundation::Err<std::vector<std::byte>>(
        foundation::ErrorCode::Internal, "could not read the temporary MP4 file");
    const std::streamoff size = file.tellg();
    if (size <= 0 || static_cast<std::uint64_t>(size) > kMaxEncodedBytes) {
        return foundation::Err<std::vector<std::byte>>(
            size > 0 ? foundation::ErrorCode::Unavailable : foundation::ErrorCode::Internal,
            size > 0 ? "encoded MP4 exceeds the 25 MiB response limit" :
                       "Media Foundation produced an empty MP4 file");
    }
    std::vector<std::byte> encoded(static_cast<std::size_t>(size));
    file.seekg(0, std::ios::beg);
    file.read(reinterpret_cast<char*>(encoded.data()), size);
    if (!file) return foundation::Err<std::vector<std::byte>>(
        foundation::ErrorCode::Internal, "could not read the complete temporary MP4 file");
    return foundation::Ok(std::move(encoded));
}
}
#endif

foundation::Result<std::vector<std::byte>> encode_ltx_video(
    sd_image_t* frames, int frame_count, int fps, const sd_audio_t* audio) {
#if defined(_WIN32)
    return encode_mp4(frames, frame_count, fps, audio);
#else
    if (frames == nullptr || frame_count <= 0 || fps <= 0) {
        return foundation::Err<std::vector<std::byte>>(
            foundation::ErrorCode::InvalidArgument,
            "video frames and fps must be valid");
    }
    const std::vector<std::uint8_t> encoded = create_video_from_sd_images_to_vector(
        "avi", frames, frame_count, fps, 90, audio);
    if (encoded.empty()) return foundation::Err<std::vector<std::byte>>(
        foundation::ErrorCode::Internal, "stable-diffusion.cpp AVI encoding failed");
    if (encoded.size() > 25u * 1024u * 1024u) return foundation::Err<std::vector<std::byte>>(
        foundation::ErrorCode::Unavailable, "encoded AVI exceeds the 25 MiB response limit");
    std::vector<std::byte> result(encoded.size());
    std::memcpy(result.data(), encoded.data(), encoded.size());
    return foundation::Ok(std::move(result));
#endif
}

}
