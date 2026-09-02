# Alpha V2 native runtimes

InferDeck registers a runtime only when its native library is linked into the gateway build. Missing libraries therefore produce `runtime_available: false` in model discovery instead of a fake backend or a successful no-op response.

The adapters are validated against these upstream revisions:

- stable-diffusion.cpp `b5d812008eb7082a238fc589444544b3278187ae`
- acestep.cpp `9761469d95fc204b5468623c68a1a2203e50b1f9`
- whisper.cpp `080bbbe85230f624f0b52127f1ae1218247989f9`
- sherpa-onnx `v1.13.2` for Supertonic 3 and Parakeet TDT support

stable-diffusion.cpp and acestep.cpp are pinned top-level Git submodules.
InferDeck builds their reusable upstream sources against the same ggml headers,
library, and Vulkan backend as llama.cpp. Their nested servers, command-line
tools, and duplicate ggml copies are not built. Run
`scripts/setup-whisper-runtime.ps1` to install the pinned Whisper source at
`runtime/whisper.cpp-src` and download the default `base.en` model. Clean
InferDeck builds discover the standard source paths automatically.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DINFERDECK_BUILD_TESTS=ON `
  -DINFERDECK_REQUIRE_STABLE_DIFFUSION_CPP=ON `
  -DINFERDECK_REQUIRE_ACESTEP_CPP=ON `
  -DINFERDECK_REQUIRE_WHISPER_CPP=ON `
  -DINFERDECK_SHERPA_ONNX_ROOT=C:/src/sherpa-onnx-install
cmake --build build --target inferdeck-gateway --config Release -j
```

The sherpa-onnx prefix must contain `include/sherpa-onnx/c-api/c-api.h`,
`lib/sherpa-onnx-c-api.lib`, and the matching runtime DLLs. InferDeck copies the
DLLs beside the gateway and native test executable. The active speech models
are Whisper `base.en` and Parakeet TDT 0.6B v3 INT8 for transcription, plus
Supertonic 3 for speech. Parakeet and Supertonic run in-process on CPU with four
bounded worker threads and declare zero VRAM. Whisper also runs in-process via
whisper.cpp. This keeps the Gemma 4 31B chat model resident while voice work is
handled by native runtimes. Windows SAPI remains a compile-time fallback.

Example model registry entries:

```yaml
model_registry:
  - name: stable-diffusion-v1-5-fp16
    family: stable-diffusion-1.5
    runtime: stable_diffusion_cpp
    modality: image
    capabilities: [image_generation]
    n_slots: 1
    vram_required_mb: 4096
    artifacts:
      model: "C:/models/image/v1-5-pruned-emaonly-fp16.safetensors"
      backend: vulkan

  - name: ace-step-v1.5-turbo-q4
    family: ace-step-1.5
    runtime: ace_step_cpp
    modality: audio_generation
    capabilities: [audio_generation]
    n_slots: 1
    min_slots: 1
    vram_required_mb: 8192
    artifacts:
      text_encoder: "C:/models/audio/Qwen3-Embedding-0.6B-Q8_0.gguf"
      dit: "C:/models/audio/acestep-v15-turbo-Q4_K_M.gguf"
      vae: "C:/models/audio/vae-BF16.gguf"

  - name: whisper-base-en
    family: whisper
    runtime: whisper_cpp
    modality: audio_transcription
    capabilities: [audio_transcription]
    n_slots: 1
    vram_required_mb: 0
    artifacts:
      model: "E:/InferDeck/models/stt/whisper/ggml-base.en.bin"

  - name: supertonic-3
    family: supertonic
    runtime: sherpa_onnx
    modality: audio_speech
    capabilities: [audio_speech]
    n_slots: 1
    vram_required_mb: 0
    artifacts:
      engine: supertonic
      duration_predictor: "C:/InferDeck/models/tts/supertonic-3/duration_predictor.int8.onnx"
      text_encoder: "C:/InferDeck/models/tts/supertonic-3/text_encoder.int8.onnx"
      vector_estimator: "C:/InferDeck/models/tts/supertonic-3/vector_estimator.int8.onnx"
      vocoder: "C:/InferDeck/models/tts/supertonic-3/vocoder.int8.onnx"
      tts_json: "C:/InferDeck/models/tts/supertonic-3/tts.json"
      unicode_indexer: "C:/InferDeck/models/tts/supertonic-3/unicode_indexer.bin"
      voice_style: "C:/InferDeck/models/tts/supertonic-3/voice.bin"
      provider: cpu
      num_threads: "4"

  - name: parakeet-tdt-0.6b-v3
    family: parakeet
    runtime: sherpa_onnx
    modality: audio_transcription
    capabilities: [audio_transcription]
    n_slots: 1
    vram_required_mb: 0
    artifacts:
      encoder: "C:/InferDeck/models/stt/parakeet-tdt-0.6b-v3/encoder.int8.onnx"
      decoder: "C:/InferDeck/models/stt/parakeet-tdt-0.6b-v3/decoder.int8.onnx"
      joiner: "C:/InferDeck/models/stt/parakeet-tdt-0.6b-v3/joiner.int8.onnx"
      tokens: "C:/InferDeck/models/stt/parakeet-tdt-0.6b-v3/tokens.txt"
      model_type: nemo_transducer
      provider: cpu
      num_threads: "4"

```

Image model weights are not bundled. The Windows/Vulkan compatibility check uses
the 2.13 GB [Stable Diffusion 1.5 FP16 checkpoint](https://huggingface.co/Comfy-Org/stable-diffusion-v1-5-archive/blob/main/v1-5-pruned-emaonly-fp16.safetensors),
SHA-256 `e9476a13728cd75d8279f6ec8bad753a66a1957ca375a1464dc63b37db6e3916`,
under the CreativeML OpenRAIL-M model licence. Other models supported by the
pinned stable-diffusion.cpp revision can use the same registry contract.

ACE-Step model weights are not bundled. The direct synthesis path needs one
text encoder, one DiT, and one VAE GGUF from the MIT-licensed
[ACE-Step 1.5 GGUF repository](https://huggingface.co/Serveurperso/ACE-Step-1.5-GGUF/tree/main).
The language-model stage is optional upstream and is deliberately omitted from
InferDeck's initial text-to-music path. `ace_step_cpp` uses strict model-store
eviction and one slot, so one ACE module is resident at a time and music jobs
are serialized.

The Windows/Vulkan compatibility check pins repository revision
`9b3707625776cc4cf775e9b12ab82f9fe48335ff` and these files:

- `Qwen3-Embedding-0.6B-Q8_0.gguf`, 784,144,960 bytes, SHA-256
  `972f23255e46adfe744a0eb9a0039f3c63988f65753b0968d776e8b27168c321`
- `acestep-v15-turbo-Q4_K_M.gguf`, 1,445,710,272 bytes, SHA-256
  `55b4d8514850f3d0f82536f37e99673aaf48df802b5ae5b153eea32a2e2daa5e`
- `vae-BF16.gguf`, 337,420,928 bytes, SHA-256
  `0599862ac5d15cd308e1d2e368373aea6c02e25ebd1737ad4a4562a0901b0ef8`

A fixed-seed 10-second request produced exactly 480,000 stereo frames at
48 kHz in 5.81 seconds. The 1,920,044-byte PCM16 WAVE had SHA-256
`8660d7777276a7a3a4fa824c0e9b510121533a8d7005fcc3b23d61ecb8411645`.
The media job reached 100%, the request row recorded HTTP 200, all modules were
evicted after use, and the queue returned to zero.

The OpenAI image endpoint returns PNG bytes through `b64_json`.
`POST /api/inferdeck/v1/audio/generations` accepts `model`, `prompt`, optional
`lyrics`, `duration`, `seed`, `steps`, and `guidance_scale`, then returns one
48 kHz stereo PCM16 WAVE body. It is an InferDeck data-plane endpoint because
OpenAI has no general music-generation API. Managed API keys and the legacy
OpenAI bearer token can call it, but neither gains control authority. The
resolved seed, media job ID, and encoded duration are response headers.
The dashboard uses control-session media routes for both generators. Generation
attempts and their PNG/WAV outputs are saved under `generated-media` beside the
configured stats database. History is bounded to 100 jobs and 2 GB; InferDeck
removes the oldest completed entries first. Running jobs are never pruned.
The speech endpoint streams runtime chunks and retains no audio. The
transcription endpoint accepts request-scoped PCM16 or float32 RIFF/WAVE input,
including WAVE_FORMAT_EXTENSIBLE, and returns `json`, `text`, `verbose_json`,
`srt`, or `vtt`. Input and transcripts are not retained. Whisper `base.en` is
the Open WebUI default. Parakeet remains available with automatic language
detection across 25 European languages, punctuation, and capitalization. Image
and transcription callbacks publish progress; active media jobs can be
cancelled through the dashboard or `POST /api/inferdeck/v1/media/jobs/:id/cancel`.

The dashboard includes Image and Music generation, output preview, playback,
download, cancellation, and attempt history. It does not capture a microphone,
transcribe recordings, or synthesize speech. Voice clients use the
OpenAI-compatible endpoints directly.

For Open WebUI, set both engines to `openai` and point them at InferDeck:

```text
ENABLE_OPENAI_API=True
OPENAI_API_BASE_URL=http://host.docker.internal:11434/v1
OPENAI_API_KEY=inferdeck-local
AUDIO_STT_ENGINE=openai
AUDIO_STT_OPENAI_API_BASE_URL=http://host.docker.internal:11434/v1
AUDIO_STT_OPENAI_API_KEY=inferdeck-local
AUDIO_STT_MODEL=whisper-base-en
AUDIO_STT_OPENAI_API_REQUEST_FORMAT=multipart
AUDIO_TTS_ENGINE=openai
AUDIO_TTS_OPENAI_API_BASE_URL=http://host.docker.internal:11434/v1
AUDIO_TTS_OPENAI_API_KEY=inferdeck-local
AUDIO_TTS_MODEL=supertonic-3
AUDIO_TTS_VOICE=alloy
AUDIO_TTS_OPENAI_PARAMS={"response_format":"wav"}
DEFAULT_MODELS=gemma-4-31b
```

Use the InferDeck LAN address instead of `host.docker.internal` when Open WebUI
runs on another machine. Supertonic accepts the standard OpenAI voice names,
`M1` through `M5`, `F1` through `F5`, and numeric speaker IDs when the loaded
model has the corresponding speakers. The configured one-style Supertonic model
maps every standard OpenAI voice name to that single local style. Leave
Open WebUI audio preprocessing enabled so browser WebM or Ogg recordings are
converted to MP3 before InferDeck receives them. InferDeck accepts that MP3
upload as well as direct RIFF/WAVE input. Select `gemma-4-31b` as the
conversation model; it is also the repository default.
Its configured context is Gemma 4 31B's full 262,144-token window.
Open WebUI 0.11.0 requests MP3 from OpenAI-compatible TTS providers. InferDeck's
native speech runtimes reject explicit MP3 output with
`unsupported_response_format`; configure `response_format` as `wav` until a
native in-process MP3 encoder is available.
On an existing Open WebUI installation, these settings may already be persisted
in its database; update them in the Admin settings if changed environment
variables do not take effect after a restart.

MTP remains disabled for Gemma 4 31B. InferDeck's current MTP implementation is
the embedded-head path used by Qwen 3.6, while Gemma 4 uses a separate assistant
drafter and current upstream llama.cpp Vulkan support is not safe enough for the
live AMD configuration. Revisit this only after upstream support is stable and a
real acceptance-rate benchmark passes on the target GPU.

Real-model validation can be included in the native runtime test executable:

```powershell
$env:INFERDECK_SHERPA_ASR_TEST_MODEL_DIR = "C:/InferDeck/models/stt/parakeet-tdt-0.6b-v3"
$env:INFERDECK_SHERPA_ASR_TEST_AUDIO = "runtime/whisper.cpp-src/samples/jfk.wav"
$env:INFERDECK_SUPERTONIC_TEST_MODEL_DIR = "C:/InferDeck/models/tts/supertonic-3"
$env:INFERDECK_WHISPER_TEST_MODEL = "E:/InferDeck/models/stt/whisper/ggml-base.en.bin"
$env:INFERDECK_WHISPER_TEST_AUDIO = "runtime/whisper.cpp-src/samples/jfk.wav"
ctest --test-dir build -C Release -R native_runtime_tests --output-on-failure
powershell -File Testing/Test-ImageGeneration.ps1 `
  -Model stable-diffusion-v1-5-fp16 `
  -Output image-validation.png
powershell -File Testing/Test-AudioGeneration.ps1 `
  -Model ace-step-v1.5-turbo-q4 `
  -DurationSeconds 10 `
  -Output audio-generation-validation.wav
```

vLLM is not an eligible in-process runtime: it requires a Python/CUDA service and would violate InferDeck's no-subprocess, no-proxy constraint. The runtime registry can host additional native C/C++ providers without changing API routes or scheduling.
