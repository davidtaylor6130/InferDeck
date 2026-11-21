#include "provider/sdk.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <set>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

#include "foundation/logging.hpp"
#include "model/imodel.hpp"
#include "model/runtime_contract.hpp"
#include "provider/codec.hpp"

namespace inferdeck::provider::sdk {

namespace {

using json = nlohmann::json;
using foundation::ErrorCode;
using Clock = std::chrono::steady_clock;

Definition* g_definition = nullptr;
json g_gateway = json::object();
std::mutex g_gateway_mutex;

struct Instance
{
    std::unique_ptr<model::IBackend> backend;
};

model::IBackend& backend(void* instance)
{
    return *static_cast<Instance*>(instance)->backend;
}

int32_t reply_error(const inferdeck_call_v2* call, ErrorCode code, const std::string& message)
{
    const std::string text = json{{"message", message}}.dump(-1, ' ', false, json::error_handler_t::replace);
    call->reply(call->context, text.c_str(), nullptr, 0);
    return static_cast<int32_t>(code);
}

template <typename T>
int32_t reply_error(const inferdeck_call_v2* call, const foundation::Result<T>& failed)
{
    return reply_error(call, failed.error().code, failed.error().message);
}

int32_t reply(const inferdeck_call_v2* call, const json& value, const std::vector<inferdeck_blob>& blobs = {})
{
    const std::string text = value.dump(-1, ' ', false, json::error_handler_t::replace);
    call->reply(call->context, text.c_str(), blobs.data(), blobs.size());
    return int32_t{INFERDECK_OK};
}

template <typename T, typename Encode>
int32_t reply_result(const inferdeck_call_v2* call, const foundation::Result<T>& result, Encode encode)
{
    if (!result)
    {
        return reply_error(call, result);
    }
    std::vector<inferdeck_blob> blobs;
    return reply(call, encode(*result, blobs), blobs);
}

template <typename Body>
int32_t shielded(const inferdeck_call_v2* call, Body body)
{
    try
    {
        return body();
    }
    catch (const std::exception& error)
    {
        return call ? reply_error(call, ErrorCode::Internal, error.what()) : INFERDECK_INTERNAL;
    }
    catch (...)
    {
        return call ? reply_error(call, ErrorCode::Internal, "provider threw an unknown exception")
                    : INFERDECK_INTERNAL;
    }
}

bool call_cancelled(const inferdeck_call_v2* call)
{
    return call->cancelled(call->context) != INFERDECK_CALL_RUNNING;
}

model::LifecycleControl control_of(const inferdeck_call_v2* call)
{
    model::LifecycleControl control;
    const int64_t remaining = INFERDECK_STRUCT_HAS(call, inferdeck_call_v2, remaining_ms) && call->remaining_ms
        ? call->remaining_ms(call->context) : -1;
    if (remaining >= 0)
    {
        control.deadline = Clock::now() + std::chrono::milliseconds(remaining);
    }
    control.cancelled = [call] { return call->cancelled(call->context) == INFERDECK_CALL_CANCELLED; };
    return control;
}

int32_t status_of(const foundation::Result<void>& result, const inferdeck_call_v2* call)
{
    return result ? INFERDECK_OK : reply_error(call, result);
}

class Emitter
{
public:
    explicit Emitter(const inferdeck_call_v2* call) : call_(call) {}

    bool emit(const json& event, const inferdeck_blob* blobs = nullptr, std::size_t count = 0)
    {
        const std::string text = event.dump(-1, ' ', false, json::error_handler_t::replace);
        std::lock_guard lock(mutex_);
        return call_->emit(call_->context, text.c_str(), blobs, count) == 0;
    }

    std::function<bool(int)> progress()
    {
        return [this](int percent) { return emit({{"type", "progress"}, {"percent", percent}}); };
    }

private:
    const inferdeck_call_v2* call_;
    std::mutex mutex_;
};

int32_t chat(model::IModel& text, const json& body, codec::Blobs blobs, const inferdeck_call_v2* call)
{
    foundation::Result<model::InferenceRequest> request = codec::decode_inference_request(body, blobs);
    if (!request)
    {
        return reply_error(call, request);
    }
    const int slot = body.value("slot", 0);
    std::atomic<bool> cancel{false};
    Emitter emitter(call);
    const std::shared_ptr<inference::RequestProgress> progress = request->progress;
    std::jthread watcher([&](std::stop_token stop)
    {
        json last;
        std::mutex mutex;
        std::condition_variable_any wake;
        while (!stop.stop_requested())
        {
            if (call_cancelled(call))
            {
                cancel.store(true);
            }
            try
            {
                if (progress)
                {
                    json snapshot = codec::encode(*progress);
                    if (snapshot != last && !emitter.emit(snapshot))
                    {
                        cancel.store(true);
                    }
                    last = std::move(snapshot);
                }
            }
            catch (...)
            {
            }
            std::unique_lock lock(mutex);
            wake.wait_for(lock, stop, std::chrono::milliseconds(50), [] { return false; });
        }
    });
    foundation::Result<model::InferenceResult> result =
        body.value("stream", false)
            ? text.predict_stream(slot, *request, [&](const model::InferenceDelta& delta)
              {
                  if (!emitter.emit(codec::encode(delta)))
                  {
                      cancel.store(true);
                  }
                  return !cancel.load();
              }, &cancel)
            : text.predict_cancellable(slot, *request, &cancel);
    watcher.request_stop();
    watcher.join();
    if (progress)
    {
        (void)emitter.emit(codec::encode(*progress));
    }
    return reply_result(call, result, [](const model::InferenceResult& value, std::vector<inferdeck_blob>&)
    {
        return codec::encode(value);
    });
}

int32_t text_operation(model::IBackend& base, const std::string& operation, const json& body,
                       codec::Blobs blobs, const inferdeck_call_v2* call)
{
    model::IModel* text = dynamic_cast<model::IModel*>(&base);
    if (!text)
    {
        return INFERDECK_NOT_IMPLEMENTED;
    }
    if (operation == "chat")
    {
        return chat(*text, body, blobs, call);
    }
    if (operation == "estimate_request_demand")
    {
        foundation::Result<model::InferenceRequest> request = codec::decode_inference_request(body, blobs);
        if (!request)
        {
            return reply_error(call, request);
        }
        return reply_result(call, text->estimate_request_demand(*request),
                            [](const model::RequestDemand& value, auto&) { return codec::encode(value); });
    }
    if (operation == "ensure_request_capacity")
    {
        foundation::Result<model::RequestDemand> demand = codec::decode_demand(body);
        if (!demand)
        {
            return reply_error(call, demand);
        }
        return status_of(text->ensure_request_capacity(*demand, control_of(call)), call);
    }
    return reply(call, codec::encode(text->chat_template_meta()));
}

template <typename Interface>
Interface* as(model::IBackend& base)
{
    return dynamic_cast<Interface*>(&base);
}

int32_t media_operation(model::IBackend& base, const std::string& operation, const json& body,
                        codec::Blobs blobs, const inferdeck_call_v2* call)
{
    const int slot = body.value("slot", 0);
    Emitter emitter(call);
    const auto cancelled = [call] { return call_cancelled(call); };
    const auto encoder = [](const auto& value, std::vector<inferdeck_blob>& out) { return codec::encode(value, out); };
    if (operation == "embed")
    {
        model::IEmbeddingBackend* embedder = as<model::IEmbeddingBackend>(base);
        if (!embedder) return INFERDECK_NOT_IMPLEMENTED;
        const auto request = codec::decode_embedding_request(body);
        if (!request) return reply_error(call, request);
        return reply_result(call, embedder->embed(slot, *request, cancelled),
                            [](const model::EmbeddingResult& value, auto&) { return codec::encode(value); });
    }
    if (operation == "decide")
    {
        model::IDecisionBackend* decider = as<model::IDecisionBackend>(base);
        if (!decider) return INFERDECK_NOT_IMPLEMENTED;
        const auto request = codec::decode_decision_request(body);
        if (!request) return reply_error(call, request);
        return reply_result(call, decider->decide(slot, *request, cancelled),
                            [](const model::DecisionResult& value, auto&) { return codec::encode(value); });
    }
    if (operation == "image")
    {
        model::IImageBackend* images = as<model::IImageBackend>(base);
        if (!images) return INFERDECK_NOT_IMPLEMENTED;
        const auto request = codec::decode_image_request(body, blobs);
        if (!request) return reply_error(call, request);
        return reply_result(call, images->generate_images(slot, *request, emitter.progress()), encoder);
    }
    if (operation == "audio_generation")
    {
        model::IAudioGenerationBackend* audio = as<model::IAudioGenerationBackend>(base);
        if (!audio) return INFERDECK_NOT_IMPLEMENTED;
        const auto request = codec::decode_audio_generation_request(body);
        if (!request) return reply_error(call, request);
        return reply_result(call, audio->generate_audio(slot, *request, emitter.progress()), encoder);
    }
    if (operation == "video")
    {
        model::IVideoBackend* video = as<model::IVideoBackend>(base);
        if (!video) return INFERDECK_NOT_IMPLEMENTED;
        const auto request = codec::decode_video_request(body);
        if (!request) return reply_error(call, request);
        return reply_result(call, video->generate_video(slot, *request, emitter.progress()), encoder);
    }
    if (operation == "transcribe")
    {
        model::ITranscriptionBackend* transcriber = as<model::ITranscriptionBackend>(base);
        if (!transcriber) return INFERDECK_NOT_IMPLEMENTED;
        const auto request = codec::decode_transcription_request(body, blobs);
        if (!request) return reply_error(call, request);
        return reply_result(call, transcriber->transcribe(slot, *request, emitter.progress()),
                            [](const model::TranscriptionResult& value, auto&) { return codec::encode(value); });
    }
    model::ISpeechBackend* speech = as<model::ISpeechBackend>(base);
    if (!speech || (operation != "speech" && operation != "speech_validate"))
    {
        return INFERDECK_NOT_IMPLEMENTED;
    }
    const auto request = codec::decode_speech_request(body);
    if (!request) return reply_error(call, request);
    if (operation == "speech_validate")
    {
        return status_of(speech->validate_speech_request(*request), call);
    }
    std::function<bool(const std::byte*, std::size_t)> stream;
    if (body.value("stream", false))
    {
        stream = [&emitter](const std::byte* data, std::size_t size)
        {
            const inferdeck_blob chunk{reinterpret_cast<const uint8_t*>(data), size};
            return emitter.emit({{"type", "audio"}}, &chunk, 1);
        };
    }
    return reply_result(call, speech->synthesize(slot, *request, stream), encoder);
}

int32_t sdk_init(const inferdeck_host_v2* host, const inferdeck_call_v2* call)
{
    return shielded(call, [&]
    {
        const foundation::Result<json> parsed = codec::parse(host->host_json);
        if (!parsed)
        {
            return reply_error(call, parsed);
        }
        {
            std::lock_guard lock(g_gateway_mutex);
            g_gateway = parsed->value("gateway", json::object());
        }
        if (INFERDECK_STRUCT_HAS(host, inferdeck_host_v2, log) && host->log)
        {
            foundation::Logger::instance().set_forwarder(
                [log = host->log, context = host->context](foundation::LogLevel level, std::string_view event,
                                                           std::string_view message)
                {
                    log(context, static_cast<int32_t>(level), std::string(event).c_str(),
                        std::string(message).c_str());
                });
        }
        return g_definition->init ? status_of(g_definition->init(*parsed), call) : INFERDECK_OK;
    });
}

void sdk_shutdown()
{
    (void)shielded(nullptr, []
    {
        if (g_definition->shutdown)
        {
            g_definition->shutdown();
        }
        foundation::Logger::instance().set_forwarder({});
        return int32_t{INFERDECK_OK};
    });
}

int32_t sdk_create(const char* model_json, const inferdeck_call_v2* call, void** instance)
{
    return shielded(call, [&]
    {
        const foundation::Result<json> parsed = codec::parse(model_json);
        if (!parsed)
        {
            return reply_error(call, parsed);
        }
        const foundation::Result<model::ModelInfo> info = codec::decode_model_info(*parsed);
        if (!info)
        {
            return reply_error(call, info);
        }
        json gateway;
        {
            std::lock_guard lock(g_gateway_mutex);
            gateway = g_gateway;
        }
        std::unique_ptr<model::IBackend> created = g_definition->factory(*info, gateway);
        if (!created)
        {
            return reply_error(call, ErrorCode::InvalidArgument, "runtime factory returned no backend for " + info->name);
        }
        *instance = new Instance{std::move(created)};
        return int32_t{INFERDECK_OK};
    });
}

void sdk_destroy(void* instance)
{
    (void)shielded(nullptr, [&] { delete static_cast<Instance*>(instance); return int32_t{INFERDECK_OK}; });
}

int32_t sdk_load(void* instance, const inferdeck_call_v2* call)
{
    return shielded(call, [&] { return status_of(backend(instance).load(control_of(call)), call); });
}

int32_t sdk_unload(void* instance, const inferdeck_call_v2* call)
{
    return shielded(call, [&] { return status_of(backend(instance).unload(control_of(call)), call); });
}

int64_t sdk_query(void* instance, int32_t query, int64_t arg)
{
    try
    {
        const model::IBackend& target = backend(instance);
        switch (query)
        {
        case INFERDECK_QUERY_LOADED: return target.is_loaded() ? 1 : 0;
        case INFERDECK_QUERY_HEALTHY: return target.execution_healthy() ? 1 : 0;
        case INFERDECK_QUERY_VRAM_USAGE_MB: return target.vram_usage_mb();
        case INFERDECK_QUERY_N_SLOTS: return target.n_slots();
        case INFERDECK_QUERY_N_FREE_SLOTS: return target.n_free_slots();
        case INFERDECK_QUERY_SLOT_BUSY: return target.slot_busy(static_cast<int>(arg)) ? 1 : 0;
        case INFERDECK_QUERY_CONTEXT_POOL_CAPACITY: return target.context_pool_capacity();
        case INFERDECK_QUERY_MIN_SLOTS: return target.min_slots();
        case INFERDECK_QUERY_CAN_RESIZE_SLOTS: return target.can_resize_slots() ? 1 : 0;
        case INFERDECK_QUERY_CAN_RECLAIM_IDLE_CONTEXT: return target.can_reclaim_idle_context() ? 1 : 0;
        case INFERDECK_QUERY_ESTIMATE_VRAM_MB: return target.estimate_vram_mb(static_cast<int>(arg));
        case INFERDECK_QUERY_ADDITIONAL_VRAM_RESERVE_MB: return target.additional_vram_reserve_mb();
        case INFERDECK_QUERY_LIVE_VRAM_ACCOUNTING_COMPLETE: return target.live_vram_accounting_complete() ? 1 : 0;
        case INFERDECK_QUERY_RECLAIMABLE_IDLE_CONTEXT_MB: return target.reclaimable_idle_context_mb();
        default: return INFERDECK_QUERY_DEFAULT;
        }
    }
    catch (...)
    {
        return INFERDECK_QUERY_DEFAULT;
    }
}

int32_t sdk_acquire_slot(void* instance, int32_t* slot)
{
    return shielded(nullptr, [&]
    {
        const foundation::Result<int> acquired = backend(instance).acquire_slot();
        if (!acquired)
        {
            return static_cast<int32_t>(acquired.error().code);
        }
        *slot = *acquired;
        return int32_t{INFERDECK_OK};
    });
}

int32_t sdk_release_slot(void* instance, int32_t slot)
{
    return shielded(nullptr, [&]
    {
        const foundation::Result<void> released = backend(instance).release_slot(slot);
        return released ? INFERDECK_OK : static_cast<int32_t>(released.error().code);
    });
}

void sdk_request_cancel(void* instance)
{
    (void)shielded(nullptr, [&] { backend(instance).request_cancel(); return int32_t{INFERDECK_OK}; });
}

int32_t sdk_reset_all_slots(void* instance, const inferdeck_call_v2* call)
{
    return shielded(call, [&] { return status_of(backend(instance).reset_all_slots(), call); });
}

int32_t sdk_resize_slots(void* instance, int32_t slots, const inferdeck_call_v2* call)
{
    return shielded(call, [&] { return status_of(backend(instance).resize_slots(slots, control_of(call)), call); });
}

int32_t sdk_reclaim_idle_context(void* instance, int32_t reserve_mb, const inferdeck_call_v2* call)
{
    return shielded(call, [&]
    {
        const foundation::Result<bool> reclaimed = backend(instance).reclaim_idle_context(reserve_mb, control_of(call));
        return reclaimed ? reply(call, {{"reclaimed", *reclaimed}}) : reply_error(call, reclaimed);
    });
}

int32_t sdk_invoke(void* instance, const char* operation, const char* request_json,
                   const inferdeck_blob* blobs, size_t blob_count, const inferdeck_call_v2* call)
{
    return shielded(call, [&]
    {
        const foundation::Result<json> body = codec::parse(request_json);
        if (!body)
        {
            return reply_error(call, body);
        }
        const std::string name = operation ? operation : "";
        const codec::Blobs payload(blobs, blobs ? blob_count : 0);
        if (name == "chat" || name == "estimate_request_demand" || name == "ensure_request_capacity" ||
            name == "chat_template_meta")
        {
            return text_operation(backend(instance), name, *body, payload, call);
        }
        return media_operation(backend(instance), name, *body, payload, call);
    });
}

int32_t sdk_provider_invoke(const char* operation, const char* request_json, const inferdeck_blob*, size_t,
                            const inferdeck_call_v2* call)
{
    return shielded(call, [&]
    {
        if (!g_definition->operation)
        {
            return int32_t{INFERDECK_NOT_IMPLEMENTED};
        }
        const foundation::Result<json> request = codec::parse(request_json);
        if (!request)
        {
            return reply_error(call, request);
        }
        const foundation::Result<json> result = g_definition->operation(operation ? operation : "", *request);
        return result ? reply(call, *result) : reply_error(call, result);
    });
}

}

std::string contract_manifest(const std::string& runtime, const std::string& version)
{
    const model::RuntimeContractRegistry contracts = model::standard_runtime_contracts();
    const model::RuntimeContract* contract = contracts.find(runtime);
    if (!contract)
    {
        return "{}";
    }
    json modalities = json::object();
    for (const auto& [modality, capabilities] : contract->modality_capabilities)
    {
        modalities[modality] = std::vector<std::string>(capabilities.begin(), capabilities.end());
    }
    static constexpr const char* sources[] = {"gguf_path", "artifacts", "none"};
    return json{{"runtime", runtime}, {"version", version}, {"modalities", std::move(modalities)},
                {"model_source", sources[static_cast<int>(contract->artifact_policy)]},
                {"vision", contract->vision}, {"reasoning", contract->reasoning},
                {"speculative", contract->speculative},
                {"speculative_types", std::set<std::string>(contract->speculative_types.begin(),
                                                            contract->speculative_types.end())}}.dump();
}

const inferdeck_provider_v2* provider_table(Definition (*define)())
{
    try
    {
        static Definition definition = define();
        static const inferdeck_provider_v2 table = []
        {
            g_definition = &definition;
            return inferdeck_provider_v2{
                sizeof(inferdeck_provider_v2), INFERDECK_PROVIDER_ABI_VERSION_2, definition.manifest_json.c_str(),
                sdk_init, sdk_shutdown, sdk_create, sdk_destroy, sdk_load, sdk_unload, sdk_query,
                sdk_acquire_slot, sdk_release_slot, sdk_request_cancel, sdk_reset_all_slots, sdk_resize_slots,
                sdk_reclaim_idle_context, sdk_invoke, sdk_provider_invoke};
        }();
        return &table;
    }
    catch (...)
    {
        return nullptr;
    }
}

}
