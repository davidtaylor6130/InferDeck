#include "gateway/media_routes.hpp"

#include "audio_decoder.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <limits>
#include <utility>

namespace inferdeck::gateway {

namespace {

void record_media(const GatewayDeps& deps, const std::string& model_name,
                  float duration_ms, int status, int slot,
                  double input_audio_seconds = 0.0,
                  std::int64_t input_characters = 0,
                  RequestObservation observation = {});

struct MediaJob {
    std::uint64_t id{0};
    std::string model;
    std::string modality;
    int progress{0};
    std::string state{"running"};
    std::shared_ptr<std::atomic<bool>> cancelled{std::make_shared<std::atomic<bool>>(false)};
};

std::mutex jobs_mutex;
std::unordered_map<std::uint64_t, MediaJob> jobs;
std::atomic<std::uint64_t> next_job_id{1};
std::mutex decode_mutex;
std::condition_variable decode_cv;
bool decode_busy{false};

class DecodePermit {
public:
    DecodePermit() = default;
    DecodePermit(const DecodePermit&) = delete;
    DecodePermit& operator=(const DecodePermit&) = delete;
    DecodePermit(DecodePermit&& other) noexcept
        : held_(std::exchange(other.held_, false)) {}
    DecodePermit& operator=(DecodePermit&&) = delete;
    ~DecodePermit() { release(); }
    void release() {
        if (!held_) return;
        {
            std::lock_guard lock(decode_mutex);
            decode_busy = false;
            held_ = false;
        }
        decode_cv.notify_one();
    }

private:
    friend foundation::Result<DecodePermit> acquire_decode_permit(
        const httplib::Request&, const std::shared_ptr<MediaJob>&);
    bool held_{true};
};

foundation::Result<DecodePermit> acquire_decode_permit(
    const httplib::Request& req, const std::shared_ptr<MediaJob>& job) {
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds{30};
    std::unique_lock lock(decode_mutex);
    while (decode_busy) {
        if (req.is_connection_closed() || job->cancelled->load()) {
            return foundation::Err<DecodePermit>(
                foundation::ErrorCode::Cancelled,
                "request cancelled while waiting for audio decoding");
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return foundation::Err<DecodePermit>(
                foundation::ErrorCode::Timeout,
                "timeout waiting for audio decoding");
        }
        decode_cv.wait_until(lock, std::min(
            deadline, std::chrono::steady_clock::now() +
                std::chrono::milliseconds{100}));
    }
    decode_busy = true;
    return foundation::Ok(DecodePermit{});
}

std::shared_ptr<MediaJob> begin_job(const std::string& model, const std::string& modality) {
    auto job = std::make_shared<MediaJob>();
    job->id = next_job_id.fetch_add(1);
    job->model = model;
    job->modality = modality;
    std::lock_guard lock(jobs_mutex);
    jobs[job->id] = *job;
    return job;
}

void update_job(const std::shared_ptr<MediaJob>& job, int progress) {
    job->progress = std::clamp(progress, 0, 100);
    std::lock_guard lock(jobs_mutex);
    jobs[job->id] = *job;
}

void finish_job(const std::shared_ptr<MediaJob>& job, const std::string& state) {
    job->state = state;
    if (state == "completed") job->progress = 100;
    std::lock_guard lock(jobs_mutex);
    jobs[job->id] = *job;
    if (jobs.size() > 100) {
        auto oldest = std::min_element(jobs.begin(), jobs.end(),
            [](const auto& left, const auto& right) { return left.first < right.first; });
        if (oldest != jobs.end()) jobs.erase(oldest);
    }
}

struct SlotGuard {
    model::BackendCoordinator* coordinator{};
    std::string model;
    int slot{-1};
    ~SlotGuard() { if (coordinator && slot >= 0) (void)coordinator->release_slot(model, slot); }
    void disarm() { coordinator = nullptr; }
};

class VoiceSessionGuard {
public:
    VoiceSessionGuard(const httplib::Request& req, const GatewayDeps& deps)
        : coordinator_(&deps.coordinator), key_(request_client_key(req)),
          duration_(deps.voice_session_grace_ms) {
        if (key_.empty() || deps.default_model.empty() ||
            deps.voice_session_grace_ms <= 0) {
            coordinator_ = nullptr;
            return;
        }
        token_ = coordinator_->reserve_priority_session(
            key_, deps.default_model,
            duration_);
    }
    VoiceSessionGuard(const VoiceSessionGuard&) = delete;
    VoiceSessionGuard& operator=(const VoiceSessionGuard&) = delete;
    ~VoiceSessionGuard() {
        if (coordinator_) coordinator_->release_priority_session(key_, token_);
    }
    void keep() { coordinator_ = nullptr; }
    void refresh_and_keep() {
        if (coordinator_) {
            (void)coordinator_->refresh_priority_session(key_, token_, duration_);
        }
        keep();
    }
    const std::string& key() const { return key_; }
    std::uint64_t token() const { return token_; }

private:
    model::BackendCoordinator* coordinator_{};
    std::string key_;
    std::chrono::milliseconds duration_{};
    std::uint64_t token_{0};
};

struct SpeechStreamState {
    static constexpr std::size_t max_pending_chunks = 64;
    static constexpr std::size_t max_pending_bytes = 2 * 1024 * 1024;
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::string> chunks;
    std::size_t pending_bytes{0};
    bool streamed_bytes{false};
    std::atomic<bool> aborted{false};
    std::atomic<bool> finished{false};
    std::atomic<bool> cleaned{false};
    std::thread worker;
    model::BackendCoordinator* coordinator{};
    GatewayDeps deps;
    std::string model;
    std::string requested_model;
    int slot{-1};
    float duration_ms{0};
    std::int64_t input_characters{0};
    double output_audio_seconds{0.0};
    RequestObservation observation;
    std::atomic<bool> failed{false};
    std::shared_ptr<MediaJob> job;
    std::string session_key;
    std::uint64_t session_token{0};

    SpeechStreamState(const GatewayDeps& source) : deps(source) {}
    ~SpeechStreamState() { if (worker.joinable()) worker.join(); }
    void finish(int status) {
        bool expected = false;
        if (!cleaned.compare_exchange_strong(expected, true)) return;
        aborted.store(status == 499);
        cv.notify_all();
        if (worker.joinable() && worker.get_id() != std::this_thread::get_id()) worker.join();
        record_media(deps, requested_model.empty() ? model : requested_model,
                     duration_ms, status, slot, 0.0,
                     status == 200 ? input_characters : 0,
                     [&] {
                         auto value = observation;
                         if (status == 200) {
                             value.output_audio_seconds = output_audio_seconds;
                         }
                         return value;
                     }());
        finish_job(job, status == 200 ? "completed" : status == 499 ? "cancelled" : "failed");
        if (coordinator) {
            (void)coordinator->release_slot(model, slot);
            if (!session_key.empty() && session_token != 0) {
                coordinator->release_priority_session(session_key, session_token);
            }
        }
    }
};

foundation::Result<int> acquire_media_slot(const httplib::Request& req,
                                            const GatewayDeps& deps,
                                            const std::string& model_name,
                                            const std::shared_ptr<MediaJob>& job) {
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds{30};
    const std::function<bool()> cancelled = [&req, job] {
        return req.is_connection_closed() || job->cancelled->load();
    };
    model::AcquireSlotOptions options;
    options.priority = 100;
    options.cancelled = cancelled;
    options.prepare = [&deps, model_name, deadline, cancelled] {
        auto loaded = ensure_model_loaded(
            deps, model_name, deadline, cancelled);
        if (loaded.ok) return foundation::Ok();
        return foundation::Err<void>(loaded.error_code, loaded.message);
    };
    return deps.coordinator.acquire_slot(model_name, options);
}

int status_for(foundation::ErrorCode code) {
    if (code == foundation::ErrorCode::InvalidArgument) return 400;
    if (code == foundation::ErrorCode::NotFound) return 404;
    if (code == foundation::ErrorCode::Cancelled) return 408;
    if (code == foundation::ErrorCode::Timeout) return 504;
    if (code == foundation::ErrorCode::ResourceBusy) return 503;
    if (code == foundation::ErrorCode::Unavailable || code == foundation::ErrorCode::NotLoaded) return 503;
    return 500;
}

int internal_status_for(foundation::ErrorCode code) {
    return code == foundation::ErrorCode::Cancelled ? 499 : status_for(code);
}

void record_media(const GatewayDeps& deps, const std::string& model_name,
                  float duration_ms, int status, int slot,
                  double input_audio_seconds,
                  std::int64_t input_characters,
                  RequestObservation observation) {
    model::InferenceResult metrics;
    metrics.duration_ms = duration_ms;
    const auto resolved = deps.coordinator.registry().resolve(model_name);
    if (observation.modality.empty()) observation.modality = "media";
    record_request(deps, model_name, metrics, status, slot,
                   input_audio_seconds, input_characters,
                   resolved ? *resolved : model_name, observation);
}

std::int64_t utf8_character_count(const std::string& text) {
    return static_cast<std::int64_t>(std::count_if(
        text.begin(), text.end(), [](unsigned char byte) {
            return (byte & 0xc0U) != 0x80U;
        }));
}

std::string base64(const std::vector<std::byte>& bytes) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve((bytes.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const auto a = std::to_integer<unsigned int>(bytes[i]);
        const auto b = i + 1 < bytes.size() ? std::to_integer<unsigned int>(bytes[i + 1]) : 0;
        const auto c = i + 2 < bytes.size() ? std::to_integer<unsigned int>(bytes[i + 2]) : 0;
        const unsigned int value = (a << 16) | (b << 8) | c;
        output += alphabet[(value >> 18) & 63];
        output += alphabet[(value >> 12) & 63];
        output += i + 1 < bytes.size() ? alphabet[(value >> 6) & 63] : '=';
        output += i + 2 < bytes.size() ? alphabet[value & 63] : '=';
    }
    return output;
}

std::uint16_t u16(const char* data) {
    return static_cast<std::uint16_t>(static_cast<unsigned char>(data[0]) |
                                      static_cast<unsigned char>(data[1]) << 8);
}

std::uint32_t u32(const char* data) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(data[0]) |
                                      static_cast<unsigned char>(data[1]) << 8 |
                                      static_cast<unsigned char>(data[2]) << 16 |
                                      static_cast<unsigned char>(data[3]) << 24);
}

foundation::Result<model::TranscriptionRequest> decode_wav(
    const std::string& content) {
    if (content.size() < 44 || std::memcmp(content.data(), "RIFF", 4) != 0 ||
        std::memcmp(content.data() + 8, "WAVE", 4) != 0) {
        return foundation::Err<model::TranscriptionRequest>(
            foundation::ErrorCode::InvalidArgument,
            "audio is not RIFF/WAVE");
    }
    std::uint16_t format = 0;
    std::uint16_t channels = 0;
    std::uint16_t bits = 0;
    std::uint32_t sample_rate = 0;
    const char* samples = nullptr;
    std::size_t sample_bytes = 0;
    for (std::size_t position = 12; position + 8 <= content.size();) {
        const char* chunk = content.data() + position;
        const std::uint32_t size = u32(chunk + 4);
        if (position + 8ULL + size > content.size()) break;
        if (std::memcmp(chunk, "fmt ", 4) == 0 && size >= 16) {
            format = u16(chunk + 8);
            channels = u16(chunk + 10);
            sample_rate = u32(chunk + 12);
            bits = u16(chunk + 22);
            if (format == 0xfffe && size >= 40) {
                const std::uint16_t subformat = u16(chunk + 32);
                if (subformat == 1 || subformat == 3) format = subformat;
            }
        } else if (std::memcmp(chunk, "data", 4) == 0) {
            samples = chunk + 8;
            sample_bytes = size;
        }
        position += 8 + size + (size & 1U);
    }
    if (!samples || channels < 1 || channels > 8 || sample_rate < 8000 || sample_rate > 192000 ||
        !((format == 1 && bits == 16) || (format == 3 && bits == 32))) {
        return foundation::Err<model::TranscriptionRequest>(foundation::ErrorCode::InvalidArgument,
                                                             "WAVE must contain PCM16 or float32 audio");
    }
    const std::size_t frame_size = channels * (bits / 8);
    if (sample_bytes % frame_size != 0) {
        return foundation::Err<model::TranscriptionRequest>(foundation::ErrorCode::InvalidArgument,
                                                             "WAVE data is not aligned to complete sample frames");
    }
    const std::size_t frames = sample_bytes / frame_size;
    if (frames == 0 || frames > static_cast<std::size_t>(sample_rate) * 60ULL * 30ULL) {
        return foundation::Err<model::TranscriptionRequest>(foundation::ErrorCode::InvalidArgument,
                                                             "audio duration is invalid or exceeds 30 minutes");
    }
    model::TranscriptionRequest request;
    request.sample_rate = static_cast<int>(sample_rate);
    request.pcm.resize(frames);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        float mixed = 0.0f;
        for (std::size_t channel = 0; channel < channels; ++channel) {
            const char* value = samples + frame * frame_size + channel * (bits / 8);
            if (format == 1) {
                mixed += static_cast<float>(static_cast<std::int16_t>(u16(value))) / 32768.0f;
            } else {
                float decoded = 0.0f;
                std::memcpy(&decoded, value, sizeof(decoded));
                if (!std::isfinite(decoded)) {
                    return foundation::Err<model::TranscriptionRequest>(
                        foundation::ErrorCode::InvalidArgument,
                        "float32 WAVE samples must be finite");
                }
                mixed += std::clamp(decoded, -1.0f, 1.0f);
            }
        }
        request.pcm[frame] = mixed / channels;
    }
    return foundation::Ok(std::move(request));
}

foundation::Result<model::TranscriptionRequest> apply_transcription_parameters(
    model::TranscriptionRequest request,
    const httplib::MultipartFormData& form) {
    if (form.has_field("language")) request.language = form.get_field("language");
    if (form.has_field("prompt")) request.prompt = form.get_field("prompt");
    if (form.has_field("temperature")) {
        try {
            const auto value = form.get_field("temperature");
            std::size_t parsed = 0;
            request.temperature = std::stof(value, &parsed);
            if (parsed != value.size()) {
                return foundation::Err<model::TranscriptionRequest>(
                    foundation::ErrorCode::InvalidArgument, "temperature must be numeric");
            }
        }
        catch (...) { return foundation::Err<model::TranscriptionRequest>(foundation::ErrorCode::InvalidArgument, "temperature must be numeric"); }
    }
    if (!std::isfinite(request.temperature) || request.temperature < 0.0f ||
        request.temperature > 1.0f || request.prompt.size() > 4096 ||
        request.language.size() > 32) {
        return foundation::Err<model::TranscriptionRequest>(foundation::ErrorCode::InvalidArgument,
                                                             "invalid transcription parameters");
    }
    return foundation::Ok(std::move(request));
}

foundation::Result<model::TranscriptionRequest> decode_audio(
    const std::string& content) {
    foundation::Result<model::TranscriptionRequest> decoded =
        content.size() >= 12 && std::memcmp(content.data(), "RIFF", 4) == 0 &&
                std::memcmp(content.data() + 8, "WAVE", 4) == 0
            ? decode_wav(content)
            : decode_compressed_audio(content);
    return decoded;
}

std::string timestamp(float seconds, char separator) {
    const auto total_milliseconds = static_cast<std::uint64_t>(
        std::llround(std::max(0.0f, seconds) * 1000.0f));
    const auto milliseconds = total_milliseconds % 1000;
    const auto total_seconds = total_milliseconds / 1000;
    const auto second = total_seconds % 60;
    const auto total_minutes = total_seconds / 60;
    const auto minute = total_minutes % 60;
    const auto hour = total_minutes / 60;
    std::ostringstream output;
    output << std::setfill('0') << std::setw(2) << hour << ':'
           << std::setw(2) << minute << ':' << std::setw(2) << second
           << separator << std::setw(3) << milliseconds;
    return output.str();
}

std::string subtitle_text(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::string subtitles(const model::TranscriptionResult& result, bool vtt) {
    std::ostringstream output;
    if (vtt) output << "WEBVTT\n\n";
    for (const auto& segment : result.segments) {
        if (!vtt) output << segment.id + 1 << '\n';
        output << timestamp(segment.start_seconds, vtt ? '.' : ',') << " --> "
               << timestamp(segment.end_seconds, vtt ? '.' : ',') << '\n'
               << subtitle_text(segment.text) << "\n\n";
    }
    return output.str();
}

nlohmann::json verbose_transcription(const model::TranscriptionResult& result,
                                     float temperature) {
    nlohmann::json segments = nlohmann::json::array();
    for (const auto& segment : result.segments) {
        segments.push_back({
            {"id", segment.id},
            {"seek", static_cast<int>(std::llround(segment.start_seconds * 100.0f))},
            {"start", segment.start_seconds},
            {"end", segment.end_seconds},
            {"text", segment.text},
            {"tokens", segment.tokens},
            {"temperature", temperature},
            {"avg_logprob", segment.avg_logprob},
            {"no_speech_prob", segment.no_speech_probability},
        });
    }
    return {
        {"task", "transcribe"},
        {"language", result.language},
        {"duration", result.duration_seconds},
        {"text", result.text},
        {"segments", std::move(segments)},
    };
}

}

nlohmann::json media_jobs() {
    std::lock_guard lock(jobs_mutex);
    nlohmann::json result = nlohmann::json::array();
    std::vector<std::uint64_t> ids;
    ids.reserve(jobs.size());
    for (const auto& [id, _] : jobs) ids.push_back(id);
    std::sort(ids.begin(), ids.end(), std::greater<>());
    for (const auto id : ids) {
        const auto& job = jobs.at(id);
        result.push_back({{"id", job.id}, {"model", job.model}, {"modality", job.modality},
                          {"progress", job.progress}, {"state", job.state}});
    }
    return result;
}

foundation::Result<void> cancel_media_job(std::uint64_t id) {
    std::lock_guard lock(jobs_mutex);
    const auto job = jobs.find(id);
    if (job == jobs.end()) return foundation::Err<void>(foundation::ErrorCode::NotFound, "media job not found");
    if (job->second.state != "running") {
        return foundation::Err<void>(foundation::ErrorCode::InvalidArgument, "media job is not running");
    }
    job->second.cancelled->store(true);
    return foundation::Ok();
}

#include "image_routes.ipp"

#include "speech_routes.ipp"

#include "transcription_routes.ipp"

}
