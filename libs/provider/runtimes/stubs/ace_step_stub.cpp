// Test double for the ACE-Step pipeline: one second of silent stereo audio.
#include "model-store.h"
#include "pipeline-synth.h"
#include "request.h"

namespace {
int g_store{};
int g_synth{};
int g_job{};
}

ModelStore* store_create(EvictPolicy) { return reinterpret_cast<ModelStore*>(&g_store); }
void store_free(ModelStore*) {}
size_t store_vram_bytes(const ModelStore*) { return 0; }
void ace_synth_default_params(AceSynthParams* params) { *params = {}; }
AceSynth* ace_synth_load(ModelStore*, const AceSynthParams*) { return reinterpret_cast<AceSynth*>(&g_synth); }
void ace_synth_free(AceSynth*) {}
void request_init(AceRequest* request) { *request = AceRequest{}; }
void request_resolve_seed(AceRequest* request) { if (request->seed < 0) request->seed = 42; }
AceSynthJob* ace_synth_job_run_dit(AceSynth*, const AceRequest*, const float*, int, const float*, int,
                                   const float*, int, const float*, int, int,
                                   bool (*cancel)(void*), void* cancel_data) {
    return cancel && cancel(cancel_data) ? nullptr : reinterpret_cast<AceSynthJob*>(&g_job);
}
int ace_synth_job_run_vae(AceSynth*, AceSynthJob*, AceAudio* out, bool (*cancel)(void*), void* cancel_data) {
    if (cancel && cancel(cancel_data)) return -1;
    out->sample_rate = 48000;
    out->n_samples = 48000;
    out->samples = new float[96000]{};
    return 0;
}
void ace_synth_job_free(AceSynthJob*) {}
void ace_audio_free(AceAudio* audio) { delete[] audio->samples; audio->samples = nullptr; }
