// inferdeck-provider-check: loads a provider library the same way the gateway
// does and exercises it, so a provider can be validated before it is installed.
//
//   inferdeck-provider-check <library> [--model <path>] [--artifact key=value]...
//                            [--modality <name>]... [--skip-invoke] [--no-load]
//
// For every modality in the manifest (or only those named) it runs the
// provider's init hook, creates an instance, loads it, checks the status
// queries and slots, runs one small request (streaming, cancellation and
// progress where the modality has them), and unloads it. Exit code 0 means
// every step passed. See docs/providers.md, "Checking a provider".
#include <cmath>
#include <functional>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "model/imodel.hpp"
#include "provider/plugin_provider.hpp"

using namespace inferdeck;

namespace
{

int g_failures = 0;

/// <summary>Prints one PASS/FAIL line (the detail only on failure) and counts failures.</summary>
bool report(const std::string& step, bool passed, const std::string& detail = {})
{
    const bool explain = !passed && !detail.empty();
    std::printf("%s  %s%s%s\n", passed ? "PASS" : "FAIL", step.c_str(),
                explain ? ": " : "", explain ? detail.c_str() : "");
    g_failures += passed ? 0 : 1;
    return passed;
}

template <typename T>
std::string error_of(const foundation::Result<T>& result)
{
    return result ? std::string{} : result.error().message;
}

/// <summary>When a media call reported progress, a false from the progress callback must cancel it.</summary>
template <typename Run>
void check_progress_stop(const std::string& modality, bool progressed, Run run)
{
    if (!progressed)
    {
        return;
    }
    const std::function<bool(int)> stop = [](int) { return false; };
    const auto stopped = run(stop);
    report(modality + " stops when progress returns false",
           !stopped && stopped.error().code == foundation::ErrorCode::Cancelled,
           stopped ? "expected a Cancelled error" : error_of(stopped));
}

/// <summary>Runs the smallest sensible request for a modality and checks the reply is well formed.</summary>
void invoke(const std::string& modality, model::IBackend& backend, int slot)
{
    const std::string step = modality + " invoke";
    if (modality == "text")
    {
        model::IModel* text = dynamic_cast<model::IModel*>(&backend);
        model::InferenceRequest request;
        request.messages.emplace_back(inference::MessageRole::User, "Reply with the word ok.");
        request.max_output_tokens = 8;
        const auto plain = text->predict(slot, request);
        report(step, static_cast<bool>(plain), error_of(plain));
        std::string streamed;
        std::string reasoning;
        int deltas = 0;
        const auto stream = text->predict_stream(
            slot, request, [&](const model::InferenceDelta& delta)
            {
                streamed += delta.content;
                reasoning += delta.reasoning_text;
                ++deltas;
                return true;
            });
        report(modality + " stream", stream && streamed == stream->text && reasoning == stream->reasoning_text,
               stream ? "streamed deltas must add up to the final text and reasoning" : error_of(stream));
        if (deltas > 0)
        {
            const auto stopped = text->predict_stream(
                slot, request, [](const model::InferenceDelta&) { return false; });
            report(modality + " stream stops when the client goes away",
                   !stopped && stopped.error().code == foundation::ErrorCode::Cancelled,
                   stopped ? "a false callback must end the call with Cancelled" : error_of(stopped));
        }
        const auto demand = text->estimate_request_demand(request);
        report(modality + " estimate request demand", static_cast<bool>(demand), error_of(demand));
    }
    else if (modality == "decision")
    {
        model::DecisionRequest request;
        request.input = "The parcel arrived broken.";
        request.questions.push_back({model::DecisionKind::Predicate, "The item is damaged.", {}});
        const auto result = dynamic_cast<model::IDecisionBackend*>(&backend)->decide(slot, request);
        const bool shaped = result && result->answers.size() == 1 &&
            result->answers[0].probabilities.size() == 2 &&
            std::abs(result->answers[0].probabilities[0] + result->answers[0].probabilities[1] - 1.0) < 1e-3;
        report(step, shaped, result ? "expected two probabilities that sum to 1" : error_of(result));
    }
    else if (modality == "embedding")
    {
        model::EmbeddingRequest request;
        request.inputs.push_back(model::EmbeddingTextInput{"hello"});
        const auto result = dynamic_cast<model::IEmbeddingBackend*>(&backend)->embed(slot, request);
        report(step, result && result->embeddings.size() == 1 && !result->embeddings[0].empty(),
               result ? "expected one non-empty vector" : error_of(result));
    }
    else if (modality == "image")
    {
        model::ImageGenerationRequest request;
        request.prompt = "a grey square";
        request.width = 256;
        request.height = 256;
        request.steps = 1;
        auto* images = dynamic_cast<model::IImageBackend*>(&backend);
        bool progressed = false;
        const auto result = images->generate_images(slot, request, [&](int) { progressed = true; return true; });
        report(step, result && result->png_images.size() == 1 && !result->png_images[0].empty(),
               result ? "expected one non-empty image" : error_of(result));
        check_progress_stop(modality, progressed, [&](const auto& stop) { return images->generate_images(slot, request, stop); });
    }
    else if (modality == "audio_generation")
    {
        model::AudioGenerationRequest request;
        request.prompt = "one soft piano note";
        request.duration_seconds = 1.0f;
        auto* audio = dynamic_cast<model::IAudioGenerationBackend*>(&backend);
        bool progressed = false;
        const auto result = audio->generate_audio(slot, request, [&](int) { progressed = true; return true; });
        report(step, result && !result->wav_bytes.empty(), result ? "expected audio bytes" : error_of(result));
        check_progress_stop(modality, progressed, [&](const auto& stop) { return audio->generate_audio(slot, request, stop); });
    }
    else if (modality == "video")
    {
        model::VideoGenerationRequest request;
        request.prompt = "a grey square";
        request.width = 256;
        request.height = 256;
        request.frames = 9;
        request.steps = 1;
        auto* video = dynamic_cast<model::IVideoBackend*>(&backend);
        bool progressed = false;
        const auto result = video->generate_video(slot, request, [&](int) { progressed = true; return true; });
        report(step, result && !result->video_bytes.empty(), result ? "expected video bytes" : error_of(result));
        check_progress_stop(modality, progressed, [&](const auto& stop) { return video->generate_video(slot, request, stop); });
    }
    else if (modality == "audio_speech")
    {
        model::SpeechRequest request;
        request.input = "Testing.";
        auto* speech = dynamic_cast<model::ISpeechBackend*>(&backend);
        const auto valid = speech->validate_speech_request(request);
        report(modality + " validates a plain request", static_cast<bool>(valid), error_of(valid));
        const auto result = speech->synthesize(slot, request);
        report(step, result && !result->bytes.empty(), result ? "expected audio bytes" : error_of(result));
    }
    else if (modality == "audio_transcription")
    {
        model::TranscriptionRequest request;
        request.pcm.assign(16000, 0.0f);
        auto* transcriber = dynamic_cast<model::ITranscriptionBackend*>(&backend);
        bool progressed = false;
        const auto result = transcriber->transcribe(slot, request, [&](int) { progressed = true; return true; });
        report(step, static_cast<bool>(result), error_of(result));
        check_progress_stop(modality, progressed, [&](const auto& stop) { return transcriber->transcribe(slot, request, stop); });
    }
    else
    {
        report(step, false, "unknown modality; this checker has no request for it");
    }
}

}

int main(int argc, char** argv)
{
    std::string library;
    model::ModelInfo base;
    base.name = "provider-check";
    base.n_slots = 1;
    base.min_slots = 1;
    std::set<std::string> only;
    bool skip_invoke = false;
    bool no_load = false;
    for (int index = 1; index < argc; ++index)
    {
        const std::string argument = argv[index];
        const auto next = [&]() -> std::string { return index + 1 < argc ? argv[++index] : ""; };
        if (argument == "--model") base.gguf_path = next();
        else if (argument == "--modality") only.insert(next());
        else if (argument == "--skip-invoke") skip_invoke = true;
        else if (argument == "--no-load") no_load = true;
        else if (argument == "--artifact")
        {
            const std::string pair = next();
            const std::size_t equals = pair.find('=');
            if (equals != std::string::npos) base.artifacts[pair.substr(0, equals)] = pair.substr(equals + 1);
        }
        else library = argument;
    }
    if (library.empty())
    {
        std::puts("usage: inferdeck-provider-check <library> [--model <path>] [--artifact key=value]... "
                  "[--modality <name>]... [--skip-invoke] [--no-load]");
        return 2;
    }

    const auto descriptor = provider::load_plugin_provider(library);
    if (!report("load library and manifest", static_cast<bool>(descriptor), error_of(descriptor)))
    {
        return 1;
    }
    std::printf("      runtime=%s version=%s\n", descriptor->contract.name.c_str(), descriptor->version.c_str());
    base.runtime = descriptor->contract.name;
    const std::map<std::string, std::unordered_set<std::string>> modalities(
        descriptor->contract.modality_capabilities.begin(), descriptor->contract.modality_capabilities.end());
    for (const auto& [modality, capabilities] : modalities)
    {
        if (!only.empty() && !only.contains(modality))
        {
            continue;
        }
        model::ModelInfo info = base;
        info.modality = modality;
        info.capabilities.assign(capabilities.begin(), capabilities.end());
        if (descriptor->initialize)
        {
            const auto initialized = descriptor->initialize({info});
            report(modality + " init", static_cast<bool>(initialized), error_of(initialized));
        }
        const std::unique_ptr<model::IBackend> backend = descriptor->factory(info);
        if (!report(modality + " create", backend != nullptr, "create returned no instance"))
        {
            continue;
        }
        report(modality + " refuses a slot before load", !backend->acquire_slot());
        if (no_load)
        {
            continue;
        }
        const auto loaded = backend->load();
        if (!report(modality + " load", static_cast<bool>(loaded), error_of(loaded)))
        {
            continue;
        }
        const int slots = backend->n_slots();
        report(modality + " status queries", backend->is_loaded() && slots >= 1 &&
               backend->n_free_slots() == slots && backend->min_slots() <= slots &&
               backend->vram_usage_mb() >= 0 && backend->estimate_vram_mb(slots) >= 0,
               "expected loaded, n_slots >= 1, all slots free, min_slots <= n_slots, non-negative VRAM");
        const auto slot = backend->acquire_slot();
        if (report(modality + " acquire slot", static_cast<bool>(slot) && backend->slot_busy(*slot) &&
                   backend->n_free_slots() == slots - 1, slot ? "slot accounting is wrong" : error_of(slot)))
        {
            if (!skip_invoke)
            {
                invoke(modality, *backend, *slot);
            }
            const auto released = backend->release_slot(*slot);
            report(modality + " release slot", released && !backend->slot_busy(*slot) &&
                   backend->n_free_slots() == slots, error_of(released));
        }
        const auto unloaded = backend->unload();
        report(modality + " unload", unloaded && !backend->is_loaded(), error_of(unloaded));
        const auto reloaded = backend->load();
        report(modality + " load again after unload", static_cast<bool>(reloaded), error_of(reloaded));
        (void)backend->unload();
    }
    std::printf("%s: %d failure(s)\n", g_failures == 0 ? "OK" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
