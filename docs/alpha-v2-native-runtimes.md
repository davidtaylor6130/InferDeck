# Alpha V2 native runtimes

InferDeck registers a runtime only when its native library is linked into the gateway build. Missing libraries therefore produce `runtime_available: false` in model discovery instead of a fake backend or a successful no-op response.

The adapters are validated against these upstream revisions:

- stable-diffusion.cpp `b5d812008eb7082a238fc589444544b3278187ae`
- whisper.cpp `080bbbe85230f624f0b52127f1ae1218247989f9`
- sherpa-onnx `v1.13.2` for Supertonic 3 and Parakeet TDT support

Pin those revisions on the Windows validation machine. stable-diffusion.cpp and whisper.cpp are added as source trees so all three ggml users share InferDeck's already-linked ggml/Vulkan implementation rather than exporting duplicate ggml symbols. Run `scripts/setup-whisper-runtime.ps1` to install the pinned Whisper source at `runtime/whisper.cpp-src` and download the default `base.en` model. Clean InferDeck builds discover that standard source path automatically.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DINFERDECK_BUILD_TESTS=ON `
  -DINFERDECK_STABLE_DIFFUSION_ROOT=C:/src/stable-diffusion.cpp `
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

The image endpoint returns PNG bytes through `b64_json` and retains no output.
The speech endpoint streams runtime chunks and retains no audio. The
transcription endpoint accepts request-scoped PCM16 or float32 RIFF/WAVE input,
including WAVE_FORMAT_EXTENSIBLE, and returns `json`, `text`, `verbose_json`,
`srt`, or `vtt`. Input and transcripts are not retained. Whisper `base.en` is
the Open WebUI default. Parakeet remains available with automatic language
detection across 25 European languages, punctuation, and capitalization. Image
and transcription callbacks publish progress; active media jobs can be
cancelled through the dashboard or `POST /api/inferdeck/v1/media/jobs/:id/cancel`.

The dashboard is an administration surface only. It does not capture a
microphone, transcribe recordings, synthesize speech, or play audio. Voice
clients use the OpenAI-compatible endpoints directly.

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
```

vLLM is not an eligible in-process runtime: it requires a Python/CUDA service and would violate InferDeck's no-subprocess, no-proxy constraint. The runtime registry can host additional native C/C++ providers without changing API routes or scheduling.
