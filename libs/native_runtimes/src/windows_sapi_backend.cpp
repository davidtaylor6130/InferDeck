#include "model/fixed_backend.hpp"
#include "model/imodel.hpp"
#include "foundation/logging.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <sapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace inferdeck::native_runtimes {

namespace {

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() {
        if (value_) value_->Release();
    }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    T* get() const noexcept { return value_; }
    T** put() noexcept {
        if (value_) {
            value_->Release();
            value_ = nullptr;
        }
        return &value_;
    }
    T* operator->() const noexcept { return value_; }

private:
    T* value_{nullptr};
};

class ComApartment {
public:
    ComApartment() : result_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ComApartment() {
        if (SUCCEEDED(result_)) CoUninitialize();
    }
    bool available() const noexcept {
        return SUCCEEDED(result_);
    }

private:
    HRESULT result_;
};

model::ModelInfo speech_info(model::ModelInfo info) {
    info.runtime = "windows_sapi";
    info.modality = "audio_speech";
    info.capabilities = {"audio_speech"};
    info.n_slots = 1;
    info.min_slots = 1;
    info.vram_required_mb = 0;
    return info;
}

void append16(std::vector<std::byte>& output, std::uint16_t value) {
    output.push_back(static_cast<std::byte>(value & 0xff));
    output.push_back(static_cast<std::byte>((value >> 8) & 0xff));
}

void append32(std::vector<std::byte>& output, std::uint32_t value) {
    append16(output, static_cast<std::uint16_t>(value & 0xffff));
    append16(output, static_cast<std::uint16_t>(value >> 16));
}

std::vector<std::byte> wave(
    const std::vector<std::byte>& pcm, std::uint32_t sample_rate) {
    std::vector<std::byte> output;
    output.reserve(44 + pcm.size());
    for (char value : std::string("RIFF")) {
        output.push_back(static_cast<std::byte>(value));
    }
    append32(output, static_cast<std::uint32_t>(36 + pcm.size()));
    for (char value : std::string("WAVEfmt ")) {
        output.push_back(static_cast<std::byte>(value));
    }
    append32(output, 16);
    append16(output, WAVE_FORMAT_PCM);
    append16(output, 1);
    append32(output, sample_rate);
    append32(output, sample_rate * 2);
    append16(output, 2);
    append16(output, 16);
    for (char value : std::string("data")) {
        output.push_back(static_cast<std::byte>(value));
    }
    append32(output, static_cast<std::uint32_t>(pcm.size()));
    output.insert(output.end(), pcm.begin(), pcm.end());
    return output;
}

foundation::Result<std::wstring> utf8_to_wide(const std::string& input) {
    const int count = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
        static_cast<int>(input.size()), nullptr, 0);
    if (count <= 0) {
        return foundation::Err<std::wstring>(
            foundation::ErrorCode::InvalidArgument, "speech input is not valid UTF-8");
    }
    std::wstring output(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
            static_cast<int>(input.size()), output.data(), count) != count) {
        return foundation::Err<std::wstring>(
            foundation::ErrorCode::Internal, "speech input conversion failed");
    }
    return foundation::Ok(std::move(output));
}

foundation::Result<void> select_voice(ISpVoice& voice, const std::string& name) {
    if (name.empty() || name == "default") return foundation::Ok();
    std::size_t consumed = 0;
    unsigned long index = 0;
    try {
        index = std::stoul(name, &consumed);
    } catch (...) {
        return foundation::Err<void>(
            foundation::ErrorCode::InvalidArgument,
            "voice must be default or a numeric Windows voice index");
    }
    if (consumed != name.size()) {
        return foundation::Err<void>(
            foundation::ErrorCode::InvalidArgument,
            "voice must be default or a numeric Windows voice index");
    }

    ComPtr<ISpObjectTokenCategory> category;
    if (FAILED(CoCreateInstance(
            CLSID_SpObjectTokenCategory, nullptr, CLSCTX_INPROC_SERVER,
            __uuidof(ISpObjectTokenCategory),
            reinterpret_cast<void**>(category.put()))) ||
        FAILED(category->SetId(SPCAT_VOICES, FALSE))) {
        return foundation::Err<void>(
            foundation::ErrorCode::Unavailable, "Windows voice enumeration failed");
    }
    ComPtr<IEnumSpObjectTokens> voices;
    if (FAILED(category->EnumTokens(nullptr, nullptr, voices.put()))) {
        return foundation::Err<void>(
            foundation::ErrorCode::Unavailable, "Windows voice enumeration failed");
    }
    ULONG count = 0;
    if (FAILED(voices->GetCount(&count)) || index >= count) {
        return foundation::Err<void>(
            foundation::ErrorCode::InvalidArgument,
            "Windows voice index is out of range");
    }
    ComPtr<ISpObjectToken> token;
    if (FAILED(voices->Item(index, token.put())) ||
        FAILED(voice.SetVoice(token.get()))) {
        return foundation::Err<void>(
            foundation::ErrorCode::Unavailable, "Windows voice selection failed");
    }
    return foundation::Ok();
}

class WindowsSapiBackend final
    : public model::FixedBackend, public model::ISpeechBackend {
public:
    explicit WindowsSapiBackend(model::ModelInfo info)
        : FixedBackend(speech_info(std::move(info))) {}

    foundation::Result<void> load() override {
        set_loaded(true);
        return foundation::Ok();
    }

    foundation::Result<void> unload() override {
        set_loaded(false);
        return foundation::Ok();
    }

    foundation::Result<model::AudioResult> synthesize(
        int, const model::SpeechRequest& request,
        const std::function<bool(const std::byte*, std::size_t)>& stream) override {
        if (!is_loaded()) {
            return foundation::Err<model::AudioResult>(
                foundation::ErrorCode::NotLoaded, "Windows speech backend is not loaded");
        }
        if (request.format != "wav" && request.format != "pcm") {
            return foundation::Err<model::AudioResult>(
                foundation::ErrorCode::InvalidArgument,
                "Windows speech runtime supports wav and pcm responses");
        }
        if (!std::isfinite(request.speed) ||
            request.speed < 0.25f || request.speed > 4.0f) {
            return foundation::Err<model::AudioResult>(
                foundation::ErrorCode::InvalidArgument,
                "speech speed must be finite and between 0.25 and 4");
        }
        foundation::LOG_INFO("windows_sapi_synthesis_start",
                             "format={} speed={:.2f}",
                             request.format, request.speed);

        auto text = utf8_to_wide(request.input);
        if (!text) {
            return foundation::Result<model::AudioResult>(
                std::unexpect, text.error());
        }

        ComApartment apartment;
        if (!apartment.available()) {
            return foundation::Err<model::AudioResult>(
                foundation::ErrorCode::Unavailable,
                "Windows speech COM initialization failed");
        }
        foundation::LOG_INFO("windows_sapi_com_ready", "status=ok");

        ComPtr<ISpVoice> voice;
        if (FAILED(CoCreateInstance(
                CLSID_SpVoice, nullptr, CLSCTX_INPROC_SERVER,
                __uuidof(ISpVoice), reinterpret_cast<void**>(voice.put())))) {
            return foundation::Err<model::AudioResult>(
                foundation::ErrorCode::Unavailable,
                "Windows speech voice is unavailable");
        }
        foundation::LOG_INFO("windows_sapi_voice_ready", "status=ok");
        auto voice_result = select_voice(*voice.get(), request.voice);
        if (!voice_result) {
            return foundation::Result<model::AudioResult>(
                std::unexpect, voice_result.error());
        }
        foundation::LOG_INFO("windows_sapi_voice_selected", "voice={}", request.voice);

        constexpr std::uint32_t sample_rate = 22050;
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = 1;
        format.nSamplesPerSec = sample_rate;
        format.wBitsPerSample = 16;
        format.nBlockAlign =
            static_cast<WORD>(format.nChannels * format.wBitsPerSample / 8);
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

        ComPtr<IStream> memory;
        if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, memory.put()))) {
            return foundation::Err<model::AudioResult>(
                foundation::ErrorCode::Internal,
                "Windows speech memory stream creation failed");
        }
        foundation::LOG_INFO("windows_sapi_memory_ready", "status=ok");
        ComPtr<ISpStream> output;
        if (FAILED(CoCreateInstance(
                CLSID_SpStream, nullptr, CLSCTX_INPROC_SERVER,
                __uuidof(ISpStream), reinterpret_cast<void**>(output.put())))) {
            return foundation::Err<model::AudioResult>(
                foundation::ErrorCode::Unavailable,
                "Windows speech output initialization failed");
        }
        foundation::LOG_INFO("windows_sapi_stream_ready", "status=ok");
        if (FAILED(output->SetBaseStream(
                memory.get(), SPDFID_WaveFormatEx, &format))) {
            return foundation::Err<model::AudioResult>(
                foundation::ErrorCode::Unavailable,
                "Windows speech output format initialization failed");
        }
        foundation::LOG_INFO("windows_sapi_format_ready", "status=ok");
        if (FAILED(voice->SetOutput(output.get(), FALSE))) {
            return foundation::Err<model::AudioResult>(
                foundation::ErrorCode::Unavailable,
                "Windows speech voice output initialization failed");
        }
        foundation::LOG_INFO("windows_sapi_output_ready", "sample_rate={}", sample_rate);

        const long rate = std::clamp(
            static_cast<long>(std::lround(std::log2(request.speed) * 5.0)),
            -10L, 10L);
        if (FAILED(voice->SetRate(rate))) {
            return foundation::Err<model::AudioResult>(
                foundation::ErrorCode::Unavailable,
                "Windows speech rate configuration failed");
        }
        foundation::LOG_INFO("windows_sapi_rate_ready", "rate={}", rate);

        const auto started = std::chrono::steady_clock::now();
        if (FAILED(voice->Speak(text->c_str(), SPF_ASYNC, nullptr))) {
            return foundation::Err<model::AudioResult>(
                foundation::ErrorCode::Internal, "Windows speech synthesis failed");
        }
        foundation::LOG_INFO("windows_sapi_speak_started", "rate={}", rate);
        while (true) {
            const HRESULT wait = voice->WaitUntilDone(50);
            if (wait == S_OK) break;
            if (FAILED(wait)) {
                return foundation::Err<model::AudioResult>(
                    foundation::ErrorCode::Internal,
                    "Windows speech synthesis wait failed");
            }
            if (stream && !stream(nullptr, 0)) {
                voice->Speak(nullptr, SPF_PURGEBEFORESPEAK, nullptr);
                return foundation::Err<model::AudioResult>(
                    foundation::ErrorCode::Cancelled,
                "speech generation cancelled");
            }
        }
        foundation::LOG_INFO("windows_sapi_speak_finished", "status=ok");

        STATSTG stats{};
        if (FAILED(memory->Stat(&stats, STATFLAG_NONAME)) ||
            stats.cbSize.QuadPart <= 0 ||
            stats.cbSize.QuadPart >
                static_cast<ULONGLONG>(std::numeric_limits<ULONG>::max())) {
            return foundation::Err<model::AudioResult>(
                foundation::ErrorCode::Internal,
                "Windows speech returned no audio");
        }
        LARGE_INTEGER start{};
        if (FAILED(memory->Seek(start, STREAM_SEEK_SET, nullptr))) {
            return foundation::Err<model::AudioResult>(
                foundation::ErrorCode::Internal,
                "Windows speech audio rewind failed");
        }
        std::vector<std::byte> pcm(
            static_cast<std::size_t>(stats.cbSize.QuadPart));
        ULONG read = 0;
        if (FAILED(memory->Read(
                pcm.data(), static_cast<ULONG>(pcm.size()), &read)) ||
            read != pcm.size()) {
            return foundation::Err<model::AudioResult>(
                foundation::ErrorCode::Internal,
                "Windows speech audio read failed");
        }

        model::AudioResult result;
        result.bytes = request.format == "wav" ? wave(pcm, sample_rate) : std::move(pcm);
        result.content_type = request.format == "wav" ? "audio/wav" : "audio/pcm";
        result.duration_ms = std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        foundation::LOG_INFO("windows_sapi_audio_ready",
                             "bytes={} duration_ms={:.2f}",
                             result.bytes.size(), result.duration_ms);
        if (stream && !stream(result.bytes.data(), result.bytes.size())) {
            return foundation::Err<model::AudioResult>(
                foundation::ErrorCode::Cancelled,
                "speech generation cancelled");
        }
        return foundation::Ok(std::move(result));
    }
};

}

std::unique_ptr<model::IBackend> make_windows_sapi_backend(
    const model::ModelInfo& info) {
    return std::make_unique<WindowsSapiBackend>(info);
}

}
