// inferdeck-provider-check: loads a provider library the same way the gateway
// does and exercises it, so a provider can be validated before it is installed.
//
//   inferdeck-provider-check <library> [--model <path>] [--artifact key=value]...
//                            [--modality <name>]... [--skip-invoke]
//
// For every modality in the manifest (or only those named) it creates an
// instance, loads it, runs one small request, and unloads it. Exit code 0
// means every step passed. See docs/providers.md, "Checking a plugin".
#include <cmath>
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
        const auto stream = text->predict_stream(
            slot, request, [&](const model::InferenceDelta& delta) { streamed += delta.content; return true; });
        report(modality + " stream", stream && streamed == stream->text,
               stream ? "streamed deltas must add up to the final text" : error_of(stream));
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
        const auto result = dynamic_cast<model::IImageBackend*>(&backend)->generate_images(slot, request);
        report(step, result && result->png_images.size() == 1 && !result->png_images[0].empty(),
               result ? "expected one non-empty image" : error_of(result));
    }
    else if (modality == "audio_generation")
    {
        model::AudioGenerationRequest request;
        request.prompt = "one soft piano note";
        request.duration_seconds = 1.0f;
        const auto result = dynamic_cast<model::IAudioGenerationBackend*>(&backend)->generate_audio(slot, request);
        report(step, result && !result->wav_bytes.empty(), result ? "expected audio bytes" : error_of(result));
    }
    else if (modality == "video")
    {
        model::VideoGenerationRequest request;
        request.prompt = "a grey square";
        request.width = 256;
        request.height = 256;
        request.frames = 9;
        request.steps = 1;
        const auto result = dynamic_cast<model::IVideoBackend*>(&backend)->generate_video(slot, request);
        report(step, result && !result->video_bytes.empty(), result ? "expected video bytes" : error_of(result));
    }
    else if (modality == "audio_speech")
    {
        model::SpeechRequest request;
        request.input = "Testing.";
        const auto result = dynamic_cast<model::ISpeechBackend*>(&backend)->synthesize(slot, request);
        report(step, result && !result->bytes.empty(), result ? "expected audio bytes" : error_of(result));
    }
    else if (modality == "audio_transcription")
    {
        model::TranscriptionRequest request;
        request.pcm.assign(16000, 0.0f);
        const auto result = dynamic_cast<model::ITranscriptionBackend*>(&backend)->transcribe(slot, request);
        report(step, static_cast<bool>(result), error_of(result));
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
    for (int index = 1; index < argc; ++index)
    {
        const std::string argument = argv[index];
        const auto next = [&]() -> std::string { return index + 1 < argc ? argv[++index] : ""; };
        if (argument == "--model") base.gguf_path = next();
        else if (argument == "--modality") only.insert(next());
        else if (argument == "--skip-invoke") skip_invoke = true;
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
                  "[--modality <name>]... [--skip-invoke]");
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
        const std::unique_ptr<model::IBackend> backend = descriptor->factory(info);
        if (!report(modality + " create", backend != nullptr, "create returned no instance"))
        {
            continue;
        }
        report(modality + " refuses a slot before load", !backend->acquire_slot());
        const auto loaded = backend->load();
        if (!report(modality + " load", static_cast<bool>(loaded), error_of(loaded)))
        {
            continue;
        }
        const auto slot = backend->acquire_slot();
        if (report(modality + " acquire slot", static_cast<bool>(slot), error_of(slot)) && !skip_invoke)
        {
            invoke(modality, *backend, *slot);
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
