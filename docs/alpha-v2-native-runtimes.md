# Alpha V2 native runtimes

InferDeck registers a runtime only when its native library is linked into the gateway build. Missing libraries therefore produce `runtime_available: false` in model discovery instead of a fake backend or a successful no-op response.

The adapters were compile-checked on 2026-07-13 against these upstream revisions:

- stable-diffusion.cpp `b5d812008eb7082a238fc589444544b3278187ae`
- whisper.cpp `080bbbe85230f624f0b52127f1ae1218247989f9`
- sherpa-onnx `40b75e98a0cd5b3f961e73ae158305b3447b5ebb`

Pin those revisions on the Windows validation machine. stable-diffusion.cpp and whisper.cpp are added as source trees so all three ggml users share InferDeck's already-linked ggml/Vulkan implementation rather than exporting duplicate ggml symbols.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DINFERDECK_BUILD_TESTS=ON `
  -DINFERDECK_STABLE_DIFFUSION_ROOT=C:/src/stable-diffusion.cpp `
  -DINFERDECK_WHISPER_ROOT=C:/src/whisper.cpp `
  -DINFERDECK_SHERPA_ONNX_ROOT=C:/src/sherpa-onnx-install
cmake --build build --target inferdeck-gateway --config Release -j
```

The sherpa-onnx prefix must contain `include/sherpa-onnx/c-api/c-api.h` and its C API library. Build sherpa-onnx with its C API and TTS enabled. Use provider `cuda` only with a compatible NVIDIA build; provider `cpu` is the portable Windows fallback. sherpa-onnx does not currently expose a Windows Vulkan provider, so AMD TTS GPU execution remains a documented native-runtime limitation even though admission, residency, cancellation, and cleanup still use InferDeck's shared resource coordinator.

Example model registry entries:

```yaml
model_registry:
  - name: flux-image
    runtime: stable_diffusion_cpp
    modality: image
    capabilities: [image_generation]
    n_slots: 1
    vram_required_mb: 16000
    artifacts:
      model: "C:/InferDeck/models/flux/model.gguf"
      vae: "C:/InferDeck/models/flux/ae.safetensors"
      clip_l: "C:/InferDeck/models/flux/clip_l.safetensors"
      t5xxl: "C:/InferDeck/models/flux/t5xxl.gguf"
      backend: "vulkan"

  - name: whisper-large-v3
    runtime: whisper_cpp
    modality: audio_transcription
    capabilities: [audio_transcription]
    n_slots: 1
    vram_required_mb: 3500
    artifacts:
      model: "C:/InferDeck/models/whisper/ggml-large-v3.bin"

  - name: lessac-voice
    runtime: sherpa_onnx
    modality: audio_speech
    capabilities: [audio_speech]
    n_slots: 1
    vram_required_mb: 512
    artifacts:
      model: "C:/InferDeck/models/tts/en_US-lessac-medium.onnx"
      tokens: "C:/InferDeck/models/tts/tokens.txt"
      data_dir: "C:/InferDeck/models/tts/espeak-ng-data"
      provider: "cpu"
```

The image endpoint returns PNG bytes through `b64_json` and retains no output. The speech endpoint streams runtime chunks and retains no audio. The transcription endpoint accepts request-scoped PCM16 or float32 RIFF/WAVE input, returns JSON or text, and retains neither input nor transcript. Image and transcription callbacks publish progress; active media jobs can be cancelled through the dashboard or `POST /api/media/jobs/:id/cancel`.

vLLM is not an eligible in-process runtime: it requires a Python/CUDA service and would violate InferDeck's no-subprocess, no-proxy constraint. The runtime registry can host additional native C/C++ providers without changing API routes or scheduling.
