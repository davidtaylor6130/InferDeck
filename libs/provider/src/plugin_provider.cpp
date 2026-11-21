#include "provider/plugin_provider.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "foundation/logging.hpp"
#include "foundation/result.hpp"
#include "inferdeck/provider_abi.h"
#include "model/imodel.hpp"
#include "provider/codec.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace inferdeck::provider {

static_assert(INFERDECK_PROVIDER_V2_MIN_SIZE == offsetof(inferdeck_provider_v2, invoke) + sizeof(void*));
static_assert(sizeof(inferdeck_provider_v2) == INFERDECK_PROVIDER_V2_MIN_SIZE + sizeof(void*));
static_assert(offsetof(inferdeck_provider_v2, struct_size) == 0);
static_assert(offsetof(inferdeck_call_v2, struct_size) == 0);
static_assert(offsetof(inferdeck_host_v2, struct_size) == 0);
static_assert(static_cast<int>(foundation::ErrorCode::Ok) == INFERDECK_OK);
static_assert(static_cast<int>(foundation::ErrorCode::InvalidArgument) == INFERDECK_INVALID_ARGUMENT);
static_assert(static_cast<int>(foundation::ErrorCode::ContextLengthExceeded) == INFERDECK_CONTEXT_LENGTH_EXCEEDED);
static_assert(static_cast<int>(foundation::ErrorCode::NotFound) == INFERDECK_NOT_FOUND);
static_assert(static_cast<int>(foundation::ErrorCode::NotLoaded) == INFERDECK_NOT_LOADED);
static_assert(static_cast<int>(foundation::ErrorCode::AlreadyExists) == INFERDECK_ALREADY_EXISTS);
static_assert(static_cast<int>(foundation::ErrorCode::IoError) == INFERDECK_IO_ERROR);
static_assert(static_cast<int>(foundation::ErrorCode::ParseError) == INFERDECK_PARSE_ERROR);
static_assert(static_cast<int>(foundation::ErrorCode::OutOfMemory) == INFERDECK_OUT_OF_MEMORY);
static_assert(static_cast<int>(foundation::ErrorCode::ResourceBusy) == INFERDECK_RESOURCE_BUSY);
static_assert(static_cast<int>(foundation::ErrorCode::Timeout) == INFERDECK_TIMEOUT);
static_assert(static_cast<int>(foundation::ErrorCode::Cancelled) == INFERDECK_CANCELLED);
static_assert(static_cast<int>(foundation::ErrorCode::Unavailable) == INFERDECK_UNAVAILABLE);
static_assert(static_cast<int>(foundation::ErrorCode::Internal) == INFERDECK_INTERNAL);

namespace {

using json = nlohmann::json;
using foundation::ErrorCode;
using Clock = std::chrono::steady_clock;

const std::vector<std::string> k_v1_unsupported = {
    "logprobs", "logit_bias", "mirostat", "tfs_z", "prompt_keep_tokens",
    "add_generation_prompt", "reasoning_format"};

struct Library
{
    const inferdeck_provider* v1{nullptr};
    inferdeck_provider_v2 table{};
    const inferdeck_provider_v2* v2{nullptr};
    std::string runtime;
    std::vector<std::string> unsupported;
    std::deque<std::string> host_jsons;
    inferdeck_host_v2 host{};
    bool initialized{false};
    mutable std::atomic<bool> device_failed{false};
};

std::mutex g_libraries_mutex;
std::vector<std::shared_ptr<Library>> g_libraries;
json g_gateway_settings = json::object();

struct CallState
{
    CallState();
    CallState(const CallState&) = delete;
    CallState& operator=(const CallState&) = delete;

    std::function<bool()> cancelled;
    Clock::time_point deadline{Clock::time_point::max()};
    std::atomic<bool> stopped{false};
    /// <summary>Receives each streaming event. Returns false to stop the call.</summary>
    std::function<bool(const json&, codec::Blobs)> on_event;
    json reply = json::object();
    std::vector<std::vector<std::byte>> blobs;
    std::vector<inferdeck_blob> views;
    inferdeck_call v1;
    inferdeck_call_v2 v2;

    codec::Blobs reply_blobs()
    {
        views.clear();
        for (const std::vector<std::byte>& blob : blobs)
        {
            views.push_back({reinterpret_cast<const uint8_t*>(blob.data()), blob.size()});
        }
        return views;
    }
};

int32_t call_cancelled(void* context)
{
    try
    {
        CallState* state = static_cast<CallState*>(context);
        if (state->stopped.load() || (state->cancelled && state->cancelled()))
        {
            return INFERDECK_CALL_CANCELLED;
        }
        return Clock::now() >= state->deadline ? INFERDECK_CALL_DEADLINE : INFERDECK_CALL_RUNNING;
    }
    catch (...)
    {
        return INFERDECK_CALL_CANCELLED;
    }
}

int32_t call_emit(void* context, const char* event_json, const inferdeck_blob* blobs, size_t blob_count)
{
    CallState* state = static_cast<CallState*>(context);
    try
    {
        const json event = json::parse(event_json ? event_json : "", nullptr, false);
        if (event.is_discarded() || !event.is_object())
        {
            return 1;
        }
        const codec::Blobs payload(blobs, blobs ? blob_count : 0);
        if (state->on_event && !state->on_event(event, payload))
        {
            state->stopped.store(true);
            return 1;
        }
        return call_cancelled(context) != INFERDECK_CALL_RUNNING ? 1 : 0;
    }
    catch (...)
    {
        state->stopped.store(true);
        return 1;
    }
}

void call_reply(void* context, const char* result_json, const inferdeck_blob* blobs, size_t blob_count)
{
    try
    {
        CallState* state = static_cast<CallState*>(context);
        json reply = json::parse(result_json ? result_json : "", nullptr, false);
        state->reply = reply.is_object() ? std::move(reply) : json::object();
        state->blobs.clear();
        for (size_t index = 0; blobs && index < blob_count; ++index)
        {
            state->blobs.push_back(codec::copy_blob(blobs[index]));
        }
    }
    catch (...)
    {
    }
}

int64_t call_remaining_ms(void* context)
{
    const CallState* state = static_cast<const CallState*>(context);
    if (state->deadline == Clock::time_point::max())
    {
        return -1;
    }
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(state->deadline - Clock::now());
    return std::max<int64_t>(0, left.count());
}

CallState::CallState()
    : v1{this, call_cancelled, call_emit, call_reply},
      v2{sizeof(inferdeck_call_v2), this, call_cancelled, call_emit, call_reply, call_remaining_ms}
{
}

void host_log(void*, int32_t level, const char* event, const char* message)
{
    try
    {
        foundation::Logger::instance().log(
            static_cast<foundation::LogLevel>(std::clamp(level, 0, 5)),
            event ? event : "provider", message ? message : "");
    }
    catch (...)
    {
    }
}

/// <summary>Turns a provider status and its optional {"message"} reply into a Result.</summary>
foundation::Result<void> to_result(int32_t status, const CallState& state, const std::string& action)
{
    if (status == INFERDECK_OK)
    {
        return foundation::Ok();
    }
    if (status == INFERDECK_NOT_IMPLEMENTED)
    {
        return foundation::Err<void>(ErrorCode::Unavailable, "provider does not implement " + action);
    }
    const ErrorCode code = status > 0 && status <= static_cast<int32_t>(ErrorCode::Internal)
        ? static_cast<ErrorCode>(status) : ErrorCode::Internal;
    const json::const_iterator message = state.reply.find("message");
    return foundation::Err<void>(code, message != state.reply.end() && message->is_string()
        ? message->get<std::string>()
        : "provider " + action + " failed with status " + std::to_string(status));
}

std::mutex g_fatal_handler_mutex;
FatalDeviceHandler g_fatal_handler;

template <typename T>
foundation::Result<T> forward_error(const foundation::Result<void>& failed)
{
    return foundation::Err<T>(failed.error().code, failed.error().message);
}

template <typename Function>
int32_t guard(Function function)
{
    try
    {
        return function();
    }
    catch (...)
    {
        return INFERDECK_INTERNAL;
    }
}

std::string dump(const json& value)
{
    return value.dump(-1, ' ', false, json::error_handler_t::replace);
}

float elapsed_ms(Clock::time_point started)
{
    return std::chrono::duration<float, std::milli>(Clock::now() - started).count();
}

std::string event_type(const json& event)
{
    const json::const_iterator type = event.find("type");
    return type != event.end() && type->is_string() ? type->get<std::string>() : std::string{};
}

class PluginBackend final : public model::IModel,
                            public model::IEmbeddingBackend,
                            public model::IImageBackend,
                            public model::IAudioGenerationBackend,
                            public model::IVideoBackend,
                            public model::ISpeechBackend,
                            public model::ITranscriptionBackend,
                            public model::IDecisionBackend
{
public:
    PluginBackend(std::shared_ptr<const Library> library, void* instance, model::ModelInfo info)
        : library_(std::move(library)), v1_(library_->v1), v2_(library_->v2), instance_(instance),
          info_(std::move(info)), slots_(static_cast<std::size_t>(std::max(1, info_.n_slots)), false) {}

    ~PluginBackend() override
    {
        if (loaded_.load())
        {
            (void)unload();
        }
        if (v2_)
        {
            v2_->destroy(instance_);
        }
        else
        {
            v1_->destroy(instance_);
        }
    }

    const model::ModelInfo& info() const override { return info_; }

    foundation::Result<void> load() override { return load(model::LifecycleControl{}); }
    foundation::Result<void> unload() override { return unload(model::LifecycleControl{}); }

    foundation::Result<void> load(const model::LifecycleControl& control) override
    {
        if (library_->device_failed.load())
        {
            return foundation::Err<void>(ErrorCode::Unavailable,
                                         "GPU device lost; the gateway must restart before this runtime can load");
        }
        cancel_all_.store(false);
        CallState state;
        lifecycle(state, control);
        const foundation::Result<void> loaded = to_result(guard([&]
        {
            return v2_ ? v2_->load(instance_, &state.v2) : v1_->load(instance_, &state.v1);
        }), state, "load");
        if (loaded)
        {
            refresh_after_load();
        }
        set_loaded(static_cast<bool>(loaded));
        return loaded;
    }

    foundation::Result<void> unload(const model::LifecycleControl& control) override
    {
        CallState state;
        lifecycle(state, control);
        const foundation::Result<void> unloaded = to_result(guard([&]
        {
            return v2_ ? v2_->unload(instance_, &state.v2) : v1_->unload(instance_, &state.v1);
        }), state, "unload");
        if (unloaded)
        {
            set_loaded(false);
        }
        return unloaded;
    }

    bool is_loaded() const override { return query(INFERDECK_QUERY_LOADED, 0, loaded_.load() ? 1 : 0) != 0; }
    bool execution_healthy() const override
    {
        return !library_->device_failed.load() && query(INFERDECK_QUERY_HEALTHY, 0, 1) != 0;
    }

    int vram_usage_mb() const override
    {
        if (!loaded_.load())
        {
            return 0;
        }
        return static_cast<int>(query(INFERDECK_QUERY_VRAM_USAGE_MB, 0, v1_vram_mb_.load()));
    }

    int n_slots() const override
    {
        return static_cast<int>(query(INFERDECK_QUERY_N_SLOTS, 0, static_cast<int64_t>(slots_.size())));
    }

    int n_free_slots() const override
    {
        if (provider_slots())
        {
            return static_cast<int>(query(INFERDECK_QUERY_N_FREE_SLOTS, 0, 0));
        }
        std::lock_guard lock(mutex_);
        return static_cast<int>(std::count(slots_.begin(), slots_.end(), false));
    }

    int context_pool_capacity() const override
    {
        return static_cast<int>(query(INFERDECK_QUERY_CONTEXT_POOL_CAPACITY, 0, 0));
    }

    int min_slots() const override { return static_cast<int>(query(INFERDECK_QUERY_MIN_SLOTS, 0, n_slots())); }
    bool can_resize_slots() const override { return query(INFERDECK_QUERY_CAN_RESIZE_SLOTS, 0, 0) != 0; }
    bool can_reclaim_idle_context() const override
    {
        return query(INFERDECK_QUERY_CAN_RECLAIM_IDLE_CONTEXT, 0, 0) != 0;
    }

    int estimate_vram_mb(int slots) const override
    {
        return static_cast<int>(query(INFERDECK_QUERY_ESTIMATE_VRAM_MB, slots, vram_usage_mb()));
    }

    int additional_vram_reserve_mb() const override
    {
        return static_cast<int>(query(INFERDECK_QUERY_ADDITIONAL_VRAM_RESERVE_MB, 0, 0));
    }

    bool live_vram_accounting_complete() const override
    {
        return query(INFERDECK_QUERY_LIVE_VRAM_ACCOUNTING_COMPLETE, 0, 0) != 0;
    }

    foundation::Result<bool> reclaim_idle_context(int additional_reserve_mb,
                                                  const model::LifecycleControl& control) override
    {
        if (!v2_ || !v2_->reclaim_idle_context)
        {
            return false;
        }
        CallState state;
        lifecycle(state, control);
        const foundation::Result<void> ran = to_result(guard([&]
        {
            return v2_->reclaim_idle_context(instance_, additional_reserve_mb, &state.v2);
        }), state, "reclaim_idle_context");
        if (!ran)
        {
            return forward_error<bool>(ran);
        }
        return state.reply.value("reclaimed", false);
    }

    foundation::Result<void> resize_slots(int slots) override
    {
        return resize_slots(slots, model::LifecycleControl{});
    }

    foundation::Result<void> resize_slots(int slots, const model::LifecycleControl& control) override
    {
        if (!v2_ || !v2_->resize_slots)
        {
            return foundation::Err<void>(ErrorCode::Unavailable, "backend capacity is fixed");
        }
        CallState state;
        lifecycle(state, control);
        return to_result(guard([&] { return v2_->resize_slots(instance_, slots, &state.v2); }),
                         state, "resize_slots");
    }

    foundation::Result<int> acquire_slot() override
    {
        if (provider_slots())
        {
            int32_t slot = -1;
            const int32_t status = guard([&] { return v2_->acquire_slot(instance_, &slot); });
            CallState state;
            const foundation::Result<void> acquired = to_result(status, state, "acquire_slot");
            return acquired ? foundation::Result<int>(static_cast<int>(slot)) : forward_error<int>(acquired);
        }
        std::lock_guard lock(mutex_);
        if (!loaded_.load())
        {
            return foundation::Err<int>(ErrorCode::NotLoaded, "backend is not loaded");
        }
        const std::vector<bool>::iterator free_slot = std::find(slots_.begin(), slots_.end(), false);
        if (free_slot == slots_.end())
        {
            return foundation::Err<int>(ErrorCode::Unavailable, "no free slots");
        }
        *free_slot = true;
        return foundation::Ok(static_cast<int>(free_slot - slots_.begin()));
    }

    foundation::Result<void> release_slot(int slot_id) override
    {
        if (provider_slots())
        {
            CallState state;
            return to_result(guard([&] { return v2_->release_slot(instance_, slot_id); }), state, "release_slot");
        }
        std::lock_guard lock(mutex_);
        if (slot_id < 0 || static_cast<std::size_t>(slot_id) >= slots_.size())
        {
            return foundation::Err<void>(ErrorCode::InvalidArgument, "invalid slot");
        }
        slots_[static_cast<std::size_t>(slot_id)] = false;
        return foundation::Ok();
    }

    bool slot_busy(int slot_id) const override
    {
        if (provider_slots())
        {
            return query(INFERDECK_QUERY_SLOT_BUSY, slot_id, 0) != 0;
        }
        std::lock_guard lock(mutex_);
        return slot_id >= 0 && static_cast<std::size_t>(slot_id) < slots_.size() &&
               slots_[static_cast<std::size_t>(slot_id)];
    }

    foundation::Result<void> reset_all_slots() override
    {
        if (!v2_ || !v2_->reset_all_slots)
        {
            return foundation::Ok();
        }
        CallState state;
        return to_result(guard([&] { return v2_->reset_all_slots(instance_, &state.v2); }), state,
                         "reset_all_slots");
    }

    void request_cancel() override
    {
        cancel_all_.store(true);
        if (v2_ && v2_->request_cancel)
        {
            (void)guard([&] { v2_->request_cancel(instance_); return 0; });
        }
    }

    foundation::Result<model::RequestDemand> estimate_request_demand(
        const model::InferenceRequest& request) const override
    {
        const foundation::Result<void> supported = reject_unsupported(request);
        if (!supported)
        {
            return forward_error<model::RequestDemand>(supported);
        }
        if (!v2_)
        {
            return model::RequestDemand{};
        }
        std::vector<inferdeck_blob> blobs;
        CallState state;
        const json body = codec::encode(request, -1, false, blobs);
        const int32_t status = invoke("estimate_request_demand", body, blobs, state);
        if (status == INFERDECK_NOT_IMPLEMENTED)
        {
            return model::RequestDemand{};
        }
        const foundation::Result<void> ran = to_result(status, state, "estimate_request_demand");
        return ran ? codec::decode_demand(state.reply) : forward_error<model::RequestDemand>(ran);
    }

    foundation::Result<void> ensure_request_capacity(const model::RequestDemand& demand,
                                                     const model::LifecycleControl& control) override
    {
        if (!v2_)
        {
            return foundation::Ok();
        }
        CallState state;
        lifecycle(state, control);
        const int32_t status = invoke("ensure_request_capacity", codec::encode(demand), {}, state);
        return status == INFERDECK_NOT_IMPLEMENTED ? foundation::Ok()
                                                   : to_result(status, state, "ensure_request_capacity");
    }

    const model::ChatTemplateMeta& chat_template_meta() const override { return template_meta_; }

    foundation::Result<model::InferenceResult> predict(
        int slot_id, const model::InferenceRequest& request) override
    {
        return generate(slot_id, request, nullptr, nullptr);
    }

    foundation::Result<model::InferenceResult> predict_cancellable(
        int slot_id, const model::InferenceRequest& request, const std::atomic<bool>* cancel) override
    {
        return generate(slot_id, request, nullptr, cancel);
    }

    foundation::Result<model::InferenceResult> predict_stream(
        int slot_id, const model::InferenceRequest& request, const TokenCallback& callback,
        const std::atomic<bool>* cancel) override
    {
        return generate(slot_id, request, &callback, cancel);
    }

    foundation::Result<model::EmbeddingResult> embed(
        int slot_id, const model::EmbeddingRequest& request, const std::function<bool()>& cancelled) override
    {
        CallState state;
        state.cancelled = [this, &cancelled] { return cancel_all_.load() || (cancelled && cancelled()); };
        const Clock::time_point started = Clock::now();
        const foundation::Result<void> ran = run("embed", codec::encode(request, slot_id), {}, state);
        if (!ran)
        {
            return forward_error<model::EmbeddingResult>(ran);
        }
        return with_duration(codec::decode_embedding_result(state.reply), started);
    }

    foundation::Result<model::ImageGenerationResult> generate_images(
        int slot_id, const model::ImageGenerationRequest& request,
        const std::function<bool(int)>& progress) override
    {
        std::vector<inferdeck_blob> blobs;
        const json body = codec::encode(request, slot_id, blobs);
        CallState state;
        track_progress(state, progress);
        const Clock::time_point started = Clock::now();
        const foundation::Result<void> ran = run("image", body, blobs, state);
        if (!ran)
        {
            return forward_error<model::ImageGenerationResult>(ran);
        }
        return with_duration(codec::decode_image_result(state.reply, state.reply_blobs()), started);
    }

    foundation::Result<model::AudioGenerationResult> generate_audio(
        int slot_id, const model::AudioGenerationRequest& request,
        const std::function<bool(int)>& progress) override
    {
        CallState state;
        track_progress(state, progress);
        const Clock::time_point started = Clock::now();
        const foundation::Result<void> ran = run("audio_generation", codec::encode(request, slot_id), {}, state);
        if (!ran)
        {
            return forward_error<model::AudioGenerationResult>(ran);
        }
        foundation::Result<model::AudioGenerationResult> result =
            codec::decode_audio_generation_result(state.reply, state.reply_blobs());
        if (result && result->seed < 0)
        {
            result->seed = request.seed;
        }
        return with_duration(std::move(result), started);
    }

    foundation::Result<model::VideoGenerationResult> generate_video(
        int slot_id, const model::VideoGenerationRequest& request,
        const std::function<bool(int)>& progress) override
    {
        CallState state;
        track_progress(state, progress);
        const Clock::time_point started = Clock::now();
        const foundation::Result<void> ran = run("video", codec::encode(request, slot_id), {}, state);
        if (!ran)
        {
            return forward_error<model::VideoGenerationResult>(ran);
        }
        return with_duration(codec::decode_video_result(state.reply, state.reply_blobs()), started);
    }

    foundation::Result<void> validate_speech_request(const model::SpeechRequest& request) override
    {
        if (!v2_)
        {
            return foundation::Ok();
        }
        CallState state;
        const int32_t status = invoke("speech_validate", codec::encode(request, -1, false), {}, state);
        return status == INFERDECK_NOT_IMPLEMENTED ? foundation::Ok() : to_result(status, state, "speech_validate");
    }

    foundation::Result<model::AudioResult> synthesize(
        int slot_id, const model::SpeechRequest& request,
        const std::function<bool(const std::byte*, std::size_t)>& stream) override
    {
        CallState state;
        state.cancelled = [this] { return cancel_all_.load(); };
        state.on_event = [&stream](const json& event, codec::Blobs blobs)
        {
            if (event_type(event) != "audio" || blobs.empty() || !stream)
            {
                return true;
            }
            return stream(reinterpret_cast<const std::byte*>(blobs[0].data), blobs[0].size);
        };
        const Clock::time_point started = Clock::now();
        const foundation::Result<void> ran =
            run("speech", codec::encode(request, slot_id, static_cast<bool>(stream)), {}, state);
        if (!ran)
        {
            return forward_error<model::AudioResult>(ran);
        }
        return with_duration(codec::decode_speech_result(state.reply, state.reply_blobs()), started);
    }

    foundation::Result<model::TranscriptionResult> transcribe(
        int slot_id, const model::TranscriptionRequest& request,
        const std::function<bool(int)>& progress) override
    {
        std::vector<inferdeck_blob> blobs;
        const json body = codec::encode(request, slot_id, blobs);
        CallState state;
        track_progress(state, progress);
        const Clock::time_point started = Clock::now();
        const foundation::Result<void> ran = run("transcribe", body, blobs, state);
        if (!ran)
        {
            return forward_error<model::TranscriptionResult>(ran);
        }
        foundation::Result<model::TranscriptionResult> result = codec::decode_transcription_result(state.reply);
        if (result && result->inference_ms <= 0.0f)
        {
            result->inference_ms = elapsed_ms(started);
        }
        if (result && result->language.empty())
        {
            result->language = request.language;
        }
        return result;
    }

    foundation::Result<model::DecisionResult> decide(
        int slot_id, const model::DecisionRequest& request, const std::function<bool()>& cancelled) override
    {
        CallState state;
        state.cancelled = [this, &cancelled] { return cancel_all_.load() || (cancelled && cancelled()); };
        const Clock::time_point started = Clock::now();
        const foundation::Result<void> ran = run("decide", codec::encode(request, slot_id), {}, state);
        if (!ran)
        {
            return forward_error<model::DecisionResult>(ran);
        }
        return with_duration(codec::decode_decision_result(state.reply), started);
    }

private:
    bool provider_slots() const { return v2_ && v2_->acquire_slot; }

    int64_t query(int32_t what, int64_t argument, int64_t fallback) const
    {
        if (!v2_ || !v2_->query)
        {
            return fallback;
        }
        try
        {
            const int64_t value = v2_->query(instance_, what, argument);
            return value == INFERDECK_QUERY_DEFAULT ? fallback : value;
        }
        catch (...)
        {
            return fallback;
        }
    }

    static void lifecycle(CallState& state, const model::LifecycleControl& control)
    {
        state.cancelled = control.cancelled;
        state.deadline = control.deadline;
    }

    void set_loaded(bool loaded)
    {
        std::lock_guard lock(mutex_);
        loaded_.store(loaded);
        if (!loaded)
        {
            std::fill(slots_.begin(), slots_.end(), false);
        }
    }

    void refresh_after_load()
    {
        if (v1_)
        {
            const int32_t reported = guard([&] { return v1_->vram_usage_mb(instance_); });
            v1_vram_mb_.store(reported < 0 ? info_.vram_required_mb : reported);
            return;
        }
        v1_vram_mb_.store(info_.vram_required_mb);
        CallState state;
        template_meta_ = {};
        if (invoke("chat_template_meta", json::object(), {}, state) == INFERDECK_OK)
        {
            const foundation::Result<model::ChatTemplateMeta> meta = codec::decode_template_meta(state.reply);
            if (meta)
            {
                template_meta_ = *meta;
            }
        }
    }

    foundation::Result<void> reject_unsupported(const model::InferenceRequest& request) const
    {
        for (const std::string& option : codec::generation_options_used(request))
        {
            if (std::find(library_->unsupported.begin(), library_->unsupported.end(), option) !=
                library_->unsupported.end())
            {
                return foundation::Err<void>(ErrorCode::InvalidArgument,
                    "runtime " + library_->runtime + " does not support the request option " + option);
            }
        }
        return foundation::Ok();
    }

    int32_t invoke(const char* operation, const json& body, std::span<const inferdeck_blob> blobs,
                   CallState& state) const
    {
        const std::string text = dump(body);
        return guard([&]
        {
            return v2_ ? v2_->invoke(instance_, operation, text.c_str(), blobs.data(), blobs.size(), &state.v2)
                       : v1_->invoke(instance_, operation, text.c_str(), blobs.data(), blobs.size(), &state.v1);
        });
    }

    foundation::Result<void> run(const char* operation, const json& body,
                                 std::span<const inferdeck_blob> blobs, CallState& state)
    {
        foundation::Result<void> result = to_result(invoke(operation, body, blobs, state), state, operation);
        if (!result && (result.error().code == ErrorCode::Internal || result.error().code == ErrorCode::Unavailable) &&
            is_fatal_device_error(result.error().message) && !library_->device_failed.exchange(true))
        {
            foundation::LOG_ERROR("provider_device_failed", "runtime={} model={} operation={} error={}",
                                  info_.runtime, info_.name, operation, result.error().message);
            FatalDeviceHandler handler;
            {
                std::lock_guard lock(g_fatal_handler_mutex);
                handler = g_fatal_handler;
            }
            if (handler) handler(info_.runtime, result.error().message);
        }
        return result;
    }

    void track_progress(CallState& state, const std::function<bool(int)>& progress)
    {
        state.cancelled = [this] { return cancel_all_.load(); };
        state.on_event = [&progress](const json& event, codec::Blobs)
        {
            if (event_type(event) != "progress" || !progress)
            {
                return true;
            }
            const json::const_iterator percent = event.find("percent");
            return progress(std::clamp(percent != event.end() && percent->is_number() ? percent->get<int>() : 0,
                                       0, 100));
        };
    }

    template <typename T>
    static foundation::Result<T> with_duration(foundation::Result<T> result, Clock::time_point started)
    {
        if (result && result->duration_ms <= 0.0f)
        {
            result->duration_ms = elapsed_ms(started);
        }
        return result;
    }

    foundation::Result<model::InferenceResult> generate(
        int slot_id, const model::InferenceRequest& request,
        const TokenCallback* callback, const std::atomic<bool>* cancel)
    {
        const foundation::Result<void> supported = reject_unsupported(request);
        if (!supported)
        {
            return forward_error<model::InferenceResult>(supported);
        }
        std::vector<inferdeck_blob> blobs;
        const json body = codec::encode(request, slot_id, callback != nullptr, blobs);
        const Clock::time_point started = Clock::now();
        float first_token_ms = 0.0f;
        CallState state;
        state.cancelled = [this, cancel] { return cancel_all_.load() || (cancel && cancel->load()); };
        state.on_event = [&](const json& event, codec::Blobs)
        {
            const std::string type = event_type(event);
            if (type == "request_progress" && request.progress)
            {
                codec::apply_progress(event, *request.progress);
                return true;
            }
            if (type != "delta" || !callback || !*callback)
            {
                return true;
            }
            const foundation::Result<model::InferenceDelta> delta = codec::decode_delta(event);
            if (!delta)
            {
                return false;
            }
            if (first_token_ms == 0.0f)
            {
                first_token_ms = elapsed_ms(started);
            }
            return (*callback)(*delta);
        };
        const foundation::Result<void> ran = run("chat", body, blobs, state);
        if (!ran)
        {
            return forward_error<model::InferenceResult>(ran);
        }
        foundation::Result<model::InferenceResult> result = codec::decode_inference_result(state.reply);
        if (result && result->duration_ms <= 0.0f)
        {
            result->duration_ms = elapsed_ms(started);
            result->first_token_duration_ms = first_token_ms;
            result->prompt_duration_ms = first_token_ms;
            result->generation_duration_ms = result->duration_ms - first_token_ms;
            if (result->generation_duration_ms > 0.0f)
            {
                result->tokens_per_second =
                    static_cast<float>(result->completion_tokens) * 1000.0f / result->generation_duration_ms;
            }
        }
        return result;
    }

    std::shared_ptr<const Library> library_;
    const inferdeck_provider* v1_;
    const inferdeck_provider_v2* v2_;
    void* instance_;
    model::ModelInfo info_;
    model::ChatTemplateMeta template_meta_;
    mutable std::mutex mutex_;
    std::atomic<bool> loaded_{false};
    std::vector<bool> slots_;
    std::atomic<bool> cancel_all_{false};
    std::atomic<int> v1_vram_mb_{0};
};

std::unique_ptr<model::IBackend> create_backend(const std::shared_ptr<const Library>& library,
                                                const model::ModelInfo& info)
{
    CallState state;
    void* instance = nullptr;
    const std::string model_json = dump(codec::encode(info));
    const int32_t status = guard([&]
    {
        return library->v2 ? library->v2->create(model_json.c_str(), &state.v2, &instance)
                           : library->v1->create(model_json.c_str(), &state.v1, &instance);
    });
    const foundation::Result<void> created = to_result(status, state, "create");
    if (!created || !instance)
    {
        foundation::LOG_ERROR("provider_create_failed", "runtime={} model={} error={}",
                              info.runtime, info.name,
                              created ? "provider returned no instance" : created.error().message);
        return nullptr;
    }
    return std::make_unique<PluginBackend>(library, instance, info);
}

void shutdown_libraries()
{
    std::lock_guard lock(g_libraries_mutex);
    for (const std::shared_ptr<Library>& library : g_libraries)
    {
        if (library->initialized && library->v2->shutdown)
        {
            (void)guard([&] { library->v2->shutdown(); return 0; });
        }
    }
}

foundation::Result<void> initialize_library(Library& library, const std::vector<model::ModelInfo>& models)
{
    if (!library.v2 || !library.v2->init)
    {
        return foundation::Ok();
    }
    json configured = json::array();
    for (const model::ModelInfo& info : models)
    {
        if (info.runtime == library.runtime)
        {
            configured.push_back(codec::encode(info));
        }
    }
    {
        std::lock_guard lock(g_libraries_mutex);
        library.host_jsons.push_back(dump({{"abi_version", INFERDECK_PROVIDER_ABI_VERSION_2},
                                           {"gateway", g_gateway_settings}, {"models", std::move(configured)}}));
    }
    library.host = {sizeof(inferdeck_host_v2), library.host_jsons.back().c_str(), nullptr, host_log};
    CallState state;
    const foundation::Result<void> initialized =
        to_result(guard([&] { return library.v2->init(&library.host, &state.v2); }), state, "init");
    if (initialized && !library.initialized)
    {
        library.initialized = true;
        static std::once_flag registered;
        std::call_once(registered, [] { std::atexit(shutdown_libraries); });
    }
    return initialized;
}

struct OpenedLibrary
{
    void* module{nullptr};
    void* v2{nullptr};
    void* v1{nullptr};
};

void close_library(void* module)
{
    if (!module)
    {
        return;
    }
#ifdef _WIN32
    FreeLibrary(static_cast<HMODULE>(module));
#else
    dlclose(module);
#endif
}

class ModuleGuard
{
public:
    explicit ModuleGuard(void* module) : module_(module) {}
    ModuleGuard(const ModuleGuard&) = delete;
    ModuleGuard& operator=(const ModuleGuard&) = delete;
    ~ModuleGuard() { close_library(module_); }
    void release() { module_ = nullptr; }

private:
    void* module_;
};

foundation::Result<OpenedLibrary> open_library(const std::filesystem::path& library)
{
    std::error_code error;
    const std::filesystem::path absolute = std::filesystem::absolute(library, error);
    if (error)
    {
        return foundation::Err<OpenedLibrary>(ErrorCode::IoError, error.message());
    }
    OpenedLibrary opened;
#ifdef _WIN32
    DWORD previous_mode = 0;
    SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &previous_mode);
    const HMODULE module = LoadLibraryExW(absolute.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    const DWORD load_error = GetLastError();
    SetThreadErrorMode(previous_mode, nullptr);
    if (!module)
    {
        return foundation::Err<OpenedLibrary>(
            ErrorCode::IoError, "LoadLibrary failed with error " + std::to_string(load_error));
    }
    opened.module = module;
    opened.v2 = reinterpret_cast<void*>(GetProcAddress(module, INFERDECK_PROVIDER_ENTRY_POINT_V2));
    opened.v1 = reinterpret_cast<void*>(GetProcAddress(module, INFERDECK_PROVIDER_ENTRY_POINT));
#else
    void* module = dlopen(absolute.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!module)
    {
        return foundation::Err<OpenedLibrary>(ErrorCode::IoError, dlerror());
    }
    opened.module = module;
    opened.v2 = dlsym(module, INFERDECK_PROVIDER_ENTRY_POINT_V2);
    opened.v1 = dlsym(module, INFERDECK_PROVIDER_ENTRY_POINT);
#endif
    if (!opened.v2 && !opened.v1)
    {
        close_library(module);
        return foundation::Err<OpenedLibrary>(
            ErrorCode::NotFound, std::string("library exports neither ") + INFERDECK_PROVIDER_ENTRY_POINT_V2 +
                                     " nor " + INFERDECK_PROVIDER_ENTRY_POINT);
    }
    return opened;
}

bool is_provider_library(const std::filesystem::path& path)
{
    const std::string name = path.filename().string();
#ifdef _WIN32
    constexpr std::string_view suffix = ".provider.dll";
#elif defined(__APPLE__)
    constexpr std::string_view suffix = ".provider.dylib";
#else
    constexpr std::string_view suffix = ".provider.so";
#endif
    return name.size() > suffix.size() &&
           std::equal(suffix.rbegin(), suffix.rend(), name.rbegin(), [](char left, char right)
           {
               return left == std::tolower(static_cast<unsigned char>(right));
           });
}

}

void set_plugin_host_settings(const std::string& gateway_json)
{
    json settings = json::parse(gateway_json, nullptr, false);
    std::lock_guard lock(g_libraries_mutex);
    g_gateway_settings = settings.is_object() ? std::move(settings) : json::object();
}

foundation::Result<model::ProviderDescriptor> load_plugin_provider(const std::filesystem::path& library_path)
{
    using DescriptorResult = foundation::Result<model::ProviderDescriptor>;
    const auto invalid = [](std::string message)
    {
        return foundation::Err<model::ProviderDescriptor>(ErrorCode::InvalidArgument, std::move(message));
    };
    const foundation::Result<OpenedLibrary> entry = open_library(library_path);
    if (!entry)
    {
        return foundation::Err<model::ProviderDescriptor>(entry.error().code, entry.error().message);
    }
    ModuleGuard module_guard(entry->module);
    auto library = std::make_shared<Library>();
    const char* manifest_text = nullptr;
    if (entry->v2)
    {
        const inferdeck_provider_v2* api = reinterpret_cast<inferdeck_provider_entry_v2>(entry->v2)();
        if (!api)
        {
            return invalid("entry point returned no provider table");
        }
        if (api->abi_version != INFERDECK_PROVIDER_ABI_VERSION_2 || api->struct_size < INFERDECK_PROVIDER_V2_MIN_SIZE)
        {
            return invalid("provider table is ABI version " + std::to_string(api->abi_version) + " with size " +
                           std::to_string(api->struct_size) + "; this gateway needs version 2 and size >= " +
                           std::to_string(INFERDECK_PROVIDER_V2_MIN_SIZE));
        }
        if (!api->manifest_json || !api->create || !api->destroy || !api->load || !api->unload || !api->invoke)
        {
            return invalid("provider table has a missing required function or manifest");
        }
        if ((api->acquire_slot || api->release_slot) && !(api->acquire_slot && api->release_slot && api->query))
        {
            return invalid("acquire_slot, release_slot, and query must be provided together");
        }
        std::memcpy(&library->table, api, std::min<size_t>(api->struct_size, sizeof(inferdeck_provider_v2)));
        library->table.struct_size = static_cast<uint32_t>(std::min<size_t>(api->struct_size, sizeof(inferdeck_provider_v2)));
        library->v2 = &library->table;
        manifest_text = api->manifest_json;
    }
    else
    {
        const inferdeck_provider* api = reinterpret_cast<inferdeck_provider_entry>(entry->v1)();
        if (!api)
        {
            return invalid("entry point returned no provider table");
        }
        if (api->abi_version != INFERDECK_PROVIDER_ABI_VERSION)
        {
            return invalid("provider ABI version " + std::to_string(api->abi_version) +
                           " is not supported by the v1 entry point");
        }
        if (!api->manifest_json || !api->create || !api->destroy || !api->load ||
            !api->unload || !api->vram_usage_mb || !api->invoke)
        {
            return invalid("provider table has a missing function or manifest");
        }
        library->v1 = api;
        library->unsupported = k_v1_unsupported;
        manifest_text = api->manifest_json;
    }
    const json manifest = json::parse(manifest_text, nullptr, false);
    if (!manifest.is_object())
    {
        return invalid("manifest is not a JSON object");
    }
    model::ProviderDescriptor descriptor;
    try
    {
        descriptor.contract.name = manifest.value("runtime", "");
        const std::string& name = descriptor.contract.name;
        if (name.empty() || !std::all_of(name.begin(), name.end(), [](unsigned char value)
            {
                return std::islower(value) || std::isdigit(value) || value == '_';
            }))
        {
            return invalid("manifest runtime must use lowercase letters, digits, and underscores");
        }
        const json modalities = manifest.value("modalities", json::object());
        if (!modalities.is_object() || modalities.empty())
        {
            return invalid("manifest modalities must map each modality to its capabilities");
        }
        for (const auto& [modality, capabilities] : modalities.items())
        {
            if (!capabilities.is_array() || capabilities.empty())
            {
                return invalid("manifest modality " + modality + " lists no capabilities");
            }
            for (const json& capability : capabilities)
            {
                descriptor.contract.modality_capabilities[modality].insert(capability.get<std::string>());
            }
        }
        const std::string model_source = manifest.value("model_source", "artifacts");
        if (model_source == "gguf_path")
        {
            descriptor.contract.artifact_policy = model::RuntimeArtifactPolicy::Gguf;
        }
        else if (model_source == "none")
        {
            descriptor.contract.artifact_policy = model::RuntimeArtifactPolicy::None;
        }
        else if (model_source != "artifacts")
        {
            return invalid("manifest model_source must be gguf_path, artifacts, or none");
        }
        descriptor.contract.vision = manifest.value("vision", false);
        descriptor.contract.reasoning = manifest.value("reasoning", false);
        const std::vector<std::string> speculative_types =
            manifest.value("speculative_types", std::vector<std::string>{});
        descriptor.contract.speculative_types.insert(speculative_types.begin(), speculative_types.end());
        if (speculative_types.empty() && manifest.value("speculative", false))
        {
            descriptor.contract.speculative_types.insert("mtp");
        }
        descriptor.contract.speculative = !descriptor.contract.speculative_types.empty();
        descriptor.version = manifest.value("version", "");
        for (const std::string& option : manifest.value("unsupported_options", std::vector<std::string>{}))
        {
            library->unsupported.push_back(option);
        }
    }
    catch (const json::exception& error)
    {
        return invalid(std::string("manifest has a value of the wrong type: ") + error.what());
    }
    library->runtime = descriptor.contract.name;
    module_guard.release();
    std::error_code error;
    descriptor.origin = std::filesystem::absolute(library_path, error).string();
    {
        std::lock_guard lock(g_libraries_mutex);
        g_libraries.push_back(library);
    }
    descriptor.factory = [library](const model::ModelInfo& info)
    {
        return create_backend(library, info);
    };
    if (library->v2 && library->v2->provider_invoke)
    {
        descriptor.operation = [library](const std::string& operation, const std::string& request_json)
            -> foundation::Result<std::string>
        {
            CallState state;
            const foundation::Result<void> ran = to_result(guard([&]
            {
                return library->v2->provider_invoke(operation.c_str(), request_json.c_str(), nullptr, 0, &state.v2);
            }), state, operation);
            if (!ran)
            {
                return foundation::Err<std::string>(ran.error().code, ran.error().message);
            }
            return dump(state.reply);
        };
    }
    if (library->v2 && library->v2->init)
    {
        descriptor.initialize = [library](const std::vector<model::ModelInfo>& models)
        {
            return initialize_library(*library, models);
        };
    }
    return DescriptorResult(std::move(descriptor));
}

PluginLoadReport load_plugin_providers(const std::filesystem::path& directory)
{
    PluginLoadReport report;
    std::error_code error;
    std::vector<std::filesystem::path> libraries;
    std::filesystem::directory_iterator entry(directory, error);
    for (; !error && entry != std::filesystem::directory_iterator(); entry.increment(error))
    {
        std::error_code file_error;
        if (entry->is_regular_file(file_error) && is_provider_library(entry->path()))
        {
            libraries.push_back(entry->path());
        }
    }
    std::sort(libraries.begin(), libraries.end());
    for (const std::filesystem::path& library : libraries)
    {
        foundation::Result<model::ProviderDescriptor> loaded = load_plugin_provider(library);
        if (!loaded)
        {
            report.rejected.push_back(library.string() + ": " + loaded.error().message);
            continue;
        }
        model::runtime_contracts().register_runtime(loaded->contract);
        report.providers.push_back(std::move(*loaded));
    }
    return report;
}

bool is_fatal_device_error(std::string_view message)
{
    std::string lowered(message);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const char* marker : {"device lost", "device_lost", "devicelost", "device hung", "device_hung",
                               "device removed", "device_removed", "gpu hang", "hiperrorlaunchfailure"})
    {
        if (lowered.find(marker) != std::string::npos) return true;
    }
    return false;
}

void set_fatal_device_handler(FatalDeviceHandler handler)
{
    std::lock_guard lock(g_fatal_handler_mutex);
    g_fatal_handler = std::move(handler);
}

}
