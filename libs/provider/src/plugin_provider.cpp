#include "provider/plugin_provider.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "foundation/logging.hpp"
#include "inferdeck/provider_abi.h"
#include "model/imodel.hpp"

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

namespace {

using json = nlohmann::json;
using foundation::ErrorCode;
using Clock = std::chrono::steady_clock;

/// <summary>Host side of one ABI call: cancellation, streaming events, and the copied reply.</summary>
struct CallState
{
    std::function<bool()> cancelled;
    /// <summary>Receives each streaming event. Returns false to stop the call.</summary>
    std::function<bool(const json&, std::span<const inferdeck_blob>)> on_event;
    json reply = json::object();
    std::vector<std::vector<std::byte>> blobs;
};

std::vector<std::byte> copy_blob(const inferdeck_blob& blob)
{
    const std::byte* begin = reinterpret_cast<const std::byte*>(blob.data);
    return blob.data ? std::vector<std::byte>(begin, begin + blob.size) : std::vector<std::byte>{};
}

int32_t call_cancelled(void* context)
{
    try
    {
        const CallState* state = static_cast<const CallState*>(context);
        return state->cancelled && state->cancelled() ? 1 : 0;
    }
    catch (...)
    {
        return 1;
    }
}

int32_t call_emit(void* context, const char* event_json,
                  const inferdeck_blob* blobs, size_t blob_count)
{
    try
    {
        CallState* state = static_cast<CallState*>(context);
        const json event = json::parse(event_json ? event_json : "", nullptr, false);
        if (event.is_discarded() || !event.is_object())
        {
            return 1;
        }
        const std::span<const inferdeck_blob> payload(blobs, blobs ? blob_count : 0);
        if (state->on_event && !state->on_event(event, payload))
        {
            return 1;
        }
        return call_cancelled(context);
    }
    catch (...)
    {
        return 1;
    }
}

void call_reply(void* context, const char* result_json,
                const inferdeck_blob* blobs, size_t blob_count)
{
    try
    {
        CallState* state = static_cast<CallState*>(context);
        json reply = json::parse(result_json ? result_json : "", nullptr, false);
        state->reply = reply.is_object() ? std::move(reply) : json::object();
        state->blobs.clear();
        for (size_t index = 0; blobs && index < blob_count; ++index)
        {
            state->blobs.push_back(copy_blob(blobs[index]));
        }
    }
    catch (...)
    {
    }
}

ErrorCode to_error_code(int32_t status)
{
    return status > 0 && status <= static_cast<int32_t>(ErrorCode::Internal)
        ? static_cast<ErrorCode>(status) : ErrorCode::Internal;
}

/// <summary>Turns a provider status and its optional {"message"} reply into a Result.</summary>
foundation::Result<void> to_result(int32_t status, const CallState& state, const std::string& action)
{
    if (status == INFERDECK_OK)
    {
        return foundation::Ok();
    }
    const json::const_iterator message = state.reply.find("message");
    return foundation::Err<void>(
        to_error_code(status),
        message != state.reply.end() && message->is_string()
            ? message->get<std::string>()
            : "provider " + action + " failed with status " + std::to_string(status));
}

template <typename T>
foundation::Result<T> forward_error(const foundation::Result<void>& failed)
{
    return foundation::Err<T>(failed.error().code, failed.error().message);
}

std::string dump(const json& value)
{
    return value.dump(-1, ' ', false, json::error_handler_t::replace);
}

float elapsed_ms(Clock::time_point started)
{
    return std::chrono::duration<float, std::milli>(Clock::now() - started).count();
}

const char* role_name(inference::MessageRole role)
{
    switch (role)
    {
    case inference::MessageRole::Developer: return "developer";
    case inference::MessageRole::System: return "system";
    case inference::MessageRole::Assistant: return "assistant";
    case inference::MessageRole::Tool: return "tool";
    case inference::MessageRole::Function: return "function";
    default: return "user";
    }
}

template <typename T>
void put_optional(json& target, const char* key, const std::optional<T>& value)
{
    if (value)
    {
        target[key] = *value;
    }
}

/// <summary>Appends a binary payload and returns its index for the JSON to reference.</summary>
std::size_t add_blob(std::vector<inferdeck_blob>& blobs, const std::vector<std::byte>& bytes)
{
    blobs.push_back({reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()});
    return blobs.size() - 1;
}

json model_json(const model::ModelInfo& info)
{
    json value = {
        {"name", info.name},
        {"family", info.family},
        {"runtime", info.runtime},
        {"modality", info.modality},
        {"capabilities", info.capabilities},
        {"compute", model::to_string(info.compute)},
        {"model_path", info.gguf_path},
        {"mmproj_path", info.mmproj_path},
        {"chat_template_path", info.chat_template_path},
        {"n_slots", info.n_slots},
        {"context_size", info.context_size},
        {"default_max_output_tokens", info.default_max_output_tokens},
        {"vram_required_mb", info.vram_required_mb},
        {"has_vision", info.has_vision},
        {"artifacts", info.artifacts},
        {"sampling", {
            {"temperature", info.sampling.temperature},
            {"top_p", info.sampling.top_p},
            {"top_k", info.sampling.top_k},
            {"min_p", info.sampling.min_p},
            {"repeat_penalty", info.sampling.repeat_penalty},
            {"repeat_last_n", info.sampling.repeat_last_n},
        }},
    };
    put_optional(value, "n_gpu_layers", info.n_gpu_layers);
    return value;
}

json chat_request_json(int slot, const model::InferenceRequest& request, bool stream,
                       std::vector<inferdeck_blob>& blobs)
{
    json messages = json::array();
    for (const inference::Message& message : request.messages)
    {
        json content = json::array();
        for (const inference::Content& part : message.content)
        {
            if (const inference::TextContent* text = std::get_if<inference::TextContent>(&part))
            {
                content.push_back({{"type", "text"}, {"text", text->text}});
            }
            else if (const inference::ImageContent* image = std::get_if<inference::ImageContent>(&part))
            {
                content.push_back({{"type", "image"}, {"blob", add_blob(blobs, image->bytes)},
                                   {"media_type", image->media_type}, {"detail", image->detail}});
            }
            else if (const inference::AudioContent* audio = std::get_if<inference::AudioContent>(&part))
            {
                content.push_back({{"type", "audio"}, {"blob", add_blob(blobs, audio->bytes)},
                                   {"media_type", audio->media_type}});
            }
        }
        json tool_calls = json::array();
        for (const inference::FunctionCall& call : message.tool_calls)
        {
            tool_calls.push_back({{"id", call.id}, {"name", call.name}, {"arguments", call.arguments}});
        }
        messages.push_back({
            {"role", role_name(message.role)},
            {"content", std::move(content)},
            {"tool_calls", std::move(tool_calls)},
            {"reasoning", message.reasoning},
            {"tool_call_id", message.tool_call_id},
            {"name", message.name},
        });
    }
    json tools = json::array();
    for (const inference::FunctionTool& tool : request.tools)
    {
        tools.push_back({{"name", tool.name}, {"description", tool.description},
                         {"parameters_schema", tool.parameters_schema}});
    }
    static constexpr const char* tool_choices[] = {"auto", "none", "required", "function"};
    static constexpr const char* output_kinds[] = {"text", "json_object", "json_schema", "grammar"};
    json sampling = {{"seed", request.sampling.seed}};
    put_optional(sampling, "temperature", request.sampling.temperature);
    put_optional(sampling, "top_p", request.sampling.top_p);
    put_optional(sampling, "top_k", request.sampling.top_k);
    put_optional(sampling, "min_p", request.sampling.min_p);
    put_optional(sampling, "repeat_penalty", request.sampling.repeat_penalty);
    put_optional(sampling, "repeat_last_n", request.sampling.repeat_last_n);
    put_optional(sampling, "frequency_penalty", request.sampling.frequency_penalty);
    put_optional(sampling, "presence_penalty", request.sampling.presence_penalty);
    json value = {
        {"slot", slot},
        {"stream", stream},
        {"prompt", request.prompt},
        {"messages", std::move(messages)},
        {"tools", std::move(tools)},
        {"tool_choice", {{"kind", tool_choices[static_cast<int>(request.tool_choice.kind)]},
                         {"function_name", request.tool_choice.function_name}}},
        {"output", {{"kind", output_kinds[static_cast<int>(request.output.kind)]},
                    {"name", request.output.name},
                    {"schema", request.output.schema},
                    {"strict", request.output.strict}}},
        {"sampling", std::move(sampling)},
        {"stop", request.stop},
        {"max_output_tokens", request.max_output_tokens},
        {"parallel_tool_calls", request.parallel_tool_calls},
    };
    put_optional(value, "reasoning_effort", request.reasoning_effort);
    put_optional(value, "enable_reasoning", request.enable_reasoning);
    put_optional(value, "context_window", request.context_window);
    return value;
}

model::InferenceDelta delta_from_json(const json& event)
{
    model::InferenceDelta delta;
    delta.content = event.value("content", "");
    delta.reasoning_text = event.value("reasoning", "");
    for (const json& call : event.value("tool_calls", json::array()))
    {
        delta.tool_calls.push_back({call.value("index", std::size_t{0}), call.value("id", ""),
                                    "function", call.value("name", ""), call.value("arguments", "")});
    }
    return delta;
}

model::InferenceResult result_from_json(const json& reply)
{
    model::InferenceResult result;
    result.text = reply.value("text", "");
    result.reasoning_text = reply.value("reasoning_text", "");
    result.finish_reason = reply.value("finish_reason", "stop");
    result.prompt_tokens = reply.value("prompt_tokens", 0);
    result.cached_prompt_tokens = reply.value("cached_prompt_tokens", 0);
    result.completion_tokens = reply.value("completion_tokens", 0);
    result.reasoning_tokens = reply.value("reasoning_tokens", 0);
    for (const json& call : reply.value("tool_calls", json::array()))
    {
        result.tool_calls.push_back({call.value("id", ""), "function",
                                     call.value("name", ""), call.value("arguments", "")});
    }
    return result;
}

/// <summary>
/// Adapts one provider-library instance to the backend interfaces the
/// coordinator uses. Slot bookkeeping lives here; everything else crosses the
/// C ABI as JSON plus binary blobs.
/// </summary>
class PluginBackend final : public model::IModel,
                            public model::IEmbeddingBackend,
                            public model::IImageBackend,
                            public model::IAudioGenerationBackend,
                            public model::IVideoBackend,
                            public model::ISpeechBackend,
                            public model::ITranscriptionBackend
{
public:
    PluginBackend(const inferdeck_provider* api, void* instance, model::ModelInfo info)
        : api_(api), instance_(instance), info_(std::move(info)),
          slots_(static_cast<std::size_t>(std::max(1, info_.n_slots)), false) {}

    ~PluginBackend() override
    {
        if (is_loaded())
        {
            (void)unload();
        }
        api_->destroy(instance_);
    }

    const model::ModelInfo& info() const override { return info_; }

    foundation::Result<void> load() override { return load(model::LifecycleControl{}); }
    foundation::Result<void> unload() override { return unload(model::LifecycleControl{}); }

    foundation::Result<void> load(const model::LifecycleControl& control) override
    {
        cancel_all_.store(false);
        const foundation::Result<void> loaded = lifecycle(api_->load, control, "load");
        set_loaded(static_cast<bool>(loaded));
        return loaded;
    }

    foundation::Result<void> unload(const model::LifecycleControl& control) override
    {
        const foundation::Result<void> unloaded = lifecycle(api_->unload, control, "unload");
        set_loaded(false);
        return unloaded;
    }

    bool is_loaded() const override
    {
        std::lock_guard lock(mutex_);
        return loaded_;
    }

    int vram_usage_mb() const override
    {
        if (!is_loaded())
        {
            return 0;
        }
        const int32_t reported = api_->vram_usage_mb(instance_);
        return reported < 0 ? info_.vram_required_mb : reported;
    }

    int n_slots() const override { return static_cast<int>(slots_.size()); }

    int n_free_slots() const override
    {
        std::lock_guard lock(mutex_);
        return static_cast<int>(std::count(slots_.begin(), slots_.end(), false));
    }

    foundation::Result<int> acquire_slot() override
    {
        std::lock_guard lock(mutex_);
        if (!loaded_)
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
        std::lock_guard lock(mutex_);
        return slot_id >= 0 && static_cast<std::size_t>(slot_id) < slots_.size() &&
               slots_[static_cast<std::size_t>(slot_id)];
    }

    void request_cancel() override { cancel_all_.store(true); }

    foundation::Result<model::InferenceResult> predict(
        int slot_id, const model::InferenceRequest& request) override
    {
        return generate(slot_id, request, nullptr, nullptr);
    }

    foundation::Result<model::InferenceResult> predict_cancellable(
        int slot_id, const model::InferenceRequest& request,
        const std::atomic<bool>* cancel) override
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
        int slot_id, const model::EmbeddingRequest& request,
        const std::function<bool()>& cancelled) override
    {
        json inputs = json::array();
        for (const model::EmbeddingInput& input : request.inputs)
        {
            if (const model::EmbeddingTextInput* text = std::get_if<model::EmbeddingTextInput>(&input))
            {
                inputs.push_back(text->text);
            }
            else
            {
                inputs.push_back(std::get<model::EmbeddingTokenInput>(input).tokens);
            }
        }
        json body = {{"slot", slot_id}, {"inputs", std::move(inputs)}};
        put_optional(body, "dimensions", request.dimensions);
        CallState state;
        state.cancelled = cancelled;
        const Clock::time_point started = Clock::now();
        const foundation::Result<void> ran = invoke("embed", body, {}, state);
        if (!ran)
        {
            return forward_error<model::EmbeddingResult>(ran);
        }
        model::EmbeddingResult result;
        result.embeddings = state.reply.value("embeddings", std::vector<std::vector<float>>{});
        result.prompt_tokens = state.reply.value("prompt_tokens", 0);
        result.duration_ms = elapsed_ms(started);
        return foundation::Ok(std::move(result));
    }

    foundation::Result<model::ImageGenerationResult> generate_images(
        int slot_id, const model::ImageGenerationRequest& request,
        const std::function<bool(int)>& progress) override
    {
        const json body = {
            {"slot", slot_id}, {"prompt", request.prompt},
            {"negative_prompt", request.negative_prompt},
            {"width", request.width}, {"height", request.height}, {"count", request.count},
            {"steps", request.steps}, {"seed", request.seed},
            {"guidance_scale", request.guidance_scale},
            {"reference_image_count", request.reference_images.size()},
        };
        std::vector<inferdeck_blob> blobs;
        for (const std::vector<std::byte>& reference : request.reference_images)
        {
            add_blob(blobs, reference);
        }
        CallState state = progress_state(progress);
        const Clock::time_point started = Clock::now();
        const foundation::Result<void> ran = invoke("image", body, blobs, state);
        if (!ran)
        {
            return forward_error<model::ImageGenerationResult>(ran);
        }
        model::ImageGenerationResult result;
        result.png_images = std::move(state.blobs);
        result.duration_ms = elapsed_ms(started);
        return foundation::Ok(std::move(result));
    }

    foundation::Result<model::AudioGenerationResult> generate_audio(
        int slot_id, const model::AudioGenerationRequest& request,
        const std::function<bool(int)>& progress) override
    {
        const json body = {
            {"slot", slot_id}, {"prompt", request.prompt}, {"lyrics", request.lyrics},
            {"duration_seconds", request.duration_seconds}, {"seed", request.seed},
            {"steps", request.steps}, {"guidance_scale", request.guidance_scale},
        };
        CallState state = progress_state(progress);
        const Clock::time_point started = Clock::now();
        const foundation::Result<void> ran = invoke("audio_generation", body, {}, state);
        if (!ran)
        {
            return forward_error<model::AudioGenerationResult>(ran);
        }
        model::AudioGenerationResult result;
        result.wav_bytes = first_blob(state);
        result.seed = state.reply.value("seed", request.seed);
        result.output_audio_seconds = state.reply.value("output_audio_seconds", 0.0);
        result.duration_ms = elapsed_ms(started);
        return foundation::Ok(std::move(result));
    }

    foundation::Result<model::VideoGenerationResult> generate_video(
        int slot_id, const model::VideoGenerationRequest& request,
        const std::function<bool(int)>& progress) override
    {
        const json body = {
            {"slot", slot_id}, {"prompt", request.prompt},
            {"negative_prompt", request.negative_prompt},
            {"width", request.width}, {"height", request.height},
            {"frames", request.frames}, {"fps", request.fps}, {"steps", request.steps},
            {"seed", request.seed}, {"guidance_scale", request.guidance_scale},
        };
        CallState state = progress_state(progress);
        const Clock::time_point started = Clock::now();
        const foundation::Result<void> ran = invoke("video", body, {}, state);
        if (!ran)
        {
            return forward_error<model::VideoGenerationResult>(ran);
        }
        model::VideoGenerationResult result;
        result.video_bytes = first_blob(state);
        result.content_type = state.reply.value("content_type", result.content_type);
        result.output_video_seconds = state.reply.value("output_video_seconds", 0.0);
        result.duration_ms = elapsed_ms(started);
        return foundation::Ok(std::move(result));
    }

    foundation::Result<void> validate_speech_request(const model::SpeechRequest&) override
    {
        return foundation::Ok();
    }

    foundation::Result<model::AudioResult> synthesize(
        int slot_id, const model::SpeechRequest& request,
        const std::function<bool(const std::byte*, std::size_t)>& stream) override
    {
        const json body = {
            {"slot", slot_id}, {"input", request.input}, {"voice", request.voice},
            {"format", request.format}, {"speed", request.speed},
            {"stream", static_cast<bool>(stream)},
        };
        CallState state;
        state.cancelled = [this] { return cancel_all_.load(); };
        state.on_event = [&stream](const json& event, std::span<const inferdeck_blob> blobs)
        {
            if (event.value("type", "") != "audio" || blobs.empty() || !stream)
            {
                return true;
            }
            return stream(reinterpret_cast<const std::byte*>(blobs[0].data), blobs[0].size);
        };
        const Clock::time_point started = Clock::now();
        const foundation::Result<void> ran = invoke("speech", body, {}, state);
        if (!ran)
        {
            return forward_error<model::AudioResult>(ran);
        }
        model::AudioResult result;
        result.bytes = first_blob(state);
        result.content_type = state.reply.value("content_type", "audio/wav");
        result.output_audio_seconds = state.reply.value("output_audio_seconds", 0.0);
        result.duration_ms = elapsed_ms(started);
        return foundation::Ok(std::move(result));
    }

    foundation::Result<model::TranscriptionResult> transcribe(
        int slot_id, const model::TranscriptionRequest& request,
        const std::function<bool(int)>& progress) override
    {
        const json body = {
            {"slot", slot_id}, {"sample_rate", request.sample_rate},
            {"language", request.language}, {"prompt", request.prompt},
            {"temperature", request.temperature}, {"pcm_blob", 0},
        };
        const inferdeck_blob pcm{reinterpret_cast<const uint8_t*>(request.pcm.data()),
                                 request.pcm.size() * sizeof(float)};
        CallState state = progress_state(progress);
        const Clock::time_point started = Clock::now();
        const foundation::Result<void> ran = invoke("transcribe", body, {&pcm, 1}, state);
        if (!ran)
        {
            return forward_error<model::TranscriptionResult>(ran);
        }
        model::TranscriptionResult result;
        result.text = state.reply.value("text", "");
        result.language = state.reply.value("language", request.language);
        result.duration_seconds = state.reply.value("duration_seconds", 0.0f);
        for (const json& segment : state.reply.value("segments", json::array()))
        {
            model::TranscriptionSegment parsed;
            parsed.id = segment.value("id", static_cast<int>(result.segments.size()));
            parsed.start_seconds = segment.value("start_seconds", 0.0f);
            parsed.end_seconds = segment.value("end_seconds", 0.0f);
            parsed.text = segment.value("text", "");
            result.segments.push_back(std::move(parsed));
        }
        result.inference_ms = elapsed_ms(started);
        return foundation::Ok(std::move(result));
    }

private:
    using LifecycleFunction = int32_t (*)(void*, const inferdeck_call*);

    void set_loaded(bool loaded)
    {
        std::lock_guard lock(mutex_);
        loaded_ = loaded;
        if (!loaded)
        {
            std::fill(slots_.begin(), slots_.end(), false);
        }
    }

    foundation::Result<void> lifecycle(LifecycleFunction function,
                                       const model::LifecycleControl& control,
                                       const std::string& action)
    {
        CallState state;
        state.cancelled = [&control] { return control.is_cancelled() || control.is_expired(); };
        const inferdeck_call call{&state, call_cancelled, call_emit, call_reply};
        int32_t status = INFERDECK_INTERNAL;
        try
        {
            status = function(instance_, &call);
        }
        catch (...)
        {
        }
        return to_result(status, state, action);
    }

    foundation::Result<void> invoke(const char* operation, const json& body,
                                    std::span<const inferdeck_blob> blobs, CallState& state)
    {
        const inferdeck_call call{&state, call_cancelled, call_emit, call_reply};
        const std::string text = dump(body);
        int32_t status = INFERDECK_INTERNAL;
        try
        {
            status = api_->invoke(instance_, operation, text.c_str(),
                                  blobs.data(), blobs.size(), &call);
        }
        catch (...)
        {
        }
        return to_result(status, state, operation);
    }

    /// <summary>Call state for media operations that report {"type":"progress","percent":n}.</summary>
    CallState progress_state(const std::function<bool(int)>& progress)
    {
        CallState state;
        state.cancelled = [this] { return cancel_all_.load(); };
        state.on_event = [&progress](const json& event, std::span<const inferdeck_blob>)
        {
            return event.value("type", "") != "progress" || !progress ||
                   progress(std::clamp(event.value("percent", 0), 0, 100));
        };
        return state;
    }

    static std::vector<std::byte> first_blob(CallState& state)
    {
        return state.blobs.empty() ? std::vector<std::byte>{} : std::move(state.blobs.front());
    }

    foundation::Result<model::InferenceResult> generate(
        int slot_id, const model::InferenceRequest& request,
        const TokenCallback* callback, const std::atomic<bool>* cancel)
    {
        std::vector<inferdeck_blob> blobs;
        const json body = chat_request_json(slot_id, request, callback != nullptr, blobs);
        const Clock::time_point started = Clock::now();
        float first_token_ms = 0.0f;
        CallState state;
        state.cancelled = [this, cancel] { return cancel_all_.load() || (cancel && cancel->load()); };
        state.on_event = [&](const json& event, std::span<const inferdeck_blob>)
        {
            if (event.value("type", "") != "delta" || !callback || !*callback)
            {
                return true;
            }
            if (first_token_ms == 0.0f)
            {
                first_token_ms = elapsed_ms(started);
            }
            return (*callback)(delta_from_json(event));
        };
        const foundation::Result<void> ran = invoke("chat", body, blobs, state);
        if (!ran)
        {
            return forward_error<model::InferenceResult>(ran);
        }
        model::InferenceResult result = result_from_json(state.reply);
        result.duration_ms = elapsed_ms(started);
        result.first_token_duration_ms = first_token_ms;
        result.prompt_duration_ms = first_token_ms;
        result.generation_duration_ms = result.duration_ms - first_token_ms;
        if (result.generation_duration_ms > 0.0f)
        {
            result.tokens_per_second =
                static_cast<float>(result.completion_tokens) * 1000.0f / result.generation_duration_ms;
        }
        return foundation::Ok(std::move(result));
    }

    const inferdeck_provider* api_;
    void* instance_;
    model::ModelInfo info_;
    mutable std::mutex mutex_;
    bool loaded_{false};
    std::vector<bool> slots_;
    std::atomic<bool> cancel_all_{false};
};

std::unique_ptr<model::IBackend> create_backend(const inferdeck_provider* api,
                                                const model::ModelInfo& info)
{
    CallState state;
    const inferdeck_call call{&state, call_cancelled, call_emit, call_reply};
    void* instance = nullptr;
    int32_t status = INFERDECK_INTERNAL;
    try
    {
        status = api->create(dump(model_json(info)).c_str(), &call, &instance);
    }
    catch (...)
    {
    }
    const foundation::Result<void> created = to_result(status, state, "create");
    if (!created || !instance)
    {
        foundation::LOG_ERROR("provider_create_failed", "runtime={} model={} error={}",
                              info.runtime, info.name,
                              created ? "provider returned no instance" : created.error().message);
        return nullptr;
    }
    return std::make_unique<PluginBackend>(api, instance, info);
}

/// <summary>Opens the library and returns its entry point, or an error naming the cause.</summary>
foundation::Result<inferdeck_provider_entry> open_library(const std::filesystem::path& library)
{
    using EntryResult = foundation::Result<inferdeck_provider_entry>;
    const std::filesystem::path absolute = std::filesystem::absolute(library);
#ifdef _WIN32
    const HMODULE module = LoadLibraryExW(absolute.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module)
    {
        return foundation::Err<inferdeck_provider_entry>(
            ErrorCode::IoError, "LoadLibrary failed with error " + std::to_string(GetLastError()));
    }
    void* symbol = reinterpret_cast<void*>(GetProcAddress(module, INFERDECK_PROVIDER_ENTRY_POINT));
#else
    void* module = dlopen(absolute.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!module)
    {
        return foundation::Err<inferdeck_provider_entry>(ErrorCode::IoError, dlerror());
    }
    void* symbol = dlsym(module, INFERDECK_PROVIDER_ENTRY_POINT);
#endif
    if (!symbol)
    {
        return foundation::Err<inferdeck_provider_entry>(
            ErrorCode::NotFound,
            std::string("library does not export ") + INFERDECK_PROVIDER_ENTRY_POINT);
    }
    return EntryResult(reinterpret_cast<inferdeck_provider_entry>(symbol));
}

bool is_library(const std::filesystem::path& path)
{
    const std::string extension = path.extension().string();
#ifdef _WIN32
    return extension == ".dll";
#else
    return extension == ".so" || extension == ".dylib";
#endif
}

}

foundation::Result<model::ProviderDescriptor> load_plugin_provider(
    const std::filesystem::path& library)
{
    using DescriptorResult = foundation::Result<model::ProviderDescriptor>;
    const auto invalid = [](std::string message)
    {
        return foundation::Err<model::ProviderDescriptor>(ErrorCode::InvalidArgument, std::move(message));
    };
    const foundation::Result<inferdeck_provider_entry> entry = open_library(library);
    if (!entry)
    {
        return foundation::Err<model::ProviderDescriptor>(entry.error().code, entry.error().message);
    }
    const inferdeck_provider* api = (*entry)();
    if (!api)
    {
        return invalid("entry point returned no provider table");
    }
    if (api->abi_version != INFERDECK_PROVIDER_ABI_VERSION)
    {
        return invalid("provider ABI version " + std::to_string(api->abi_version) +
                       " is not supported; this gateway speaks version " +
                       std::to_string(INFERDECK_PROVIDER_ABI_VERSION));
    }
    if (!api->manifest_json || !api->create || !api->destroy || !api->load ||
        !api->unload || !api->vram_usage_mb || !api->invoke)
    {
        return invalid("provider table has a missing function or manifest");
    }
    const json manifest = json::parse(api->manifest_json, nullptr, false);
    if (!manifest.is_object())
    {
        return invalid("manifest is not a JSON object");
    }
    model::ProviderDescriptor descriptor;
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
            if (!capability.is_string())
            {
                return invalid("manifest capabilities must be strings");
            }
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
    descriptor.version = manifest.value("version", "");
    descriptor.origin = std::filesystem::absolute(library).string();
    descriptor.factory = [api](const model::ModelInfo& info)
    {
        return create_backend(api, info);
    };
    return DescriptorResult(std::move(descriptor));
}

PluginLoadReport load_plugin_providers(const std::filesystem::path& directory)
{
    PluginLoadReport report;
    std::error_code error;
    std::vector<std::filesystem::path> libraries;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(directory, error))
    {
        if (entry.is_regular_file() && is_library(entry.path()))
        {
            libraries.push_back(entry.path());
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

}
