# InferDeck Alpha V2 architecture

InferDeck is one native gateway process that admits, loads, executes, observes, and unloads local AI models. It does not proxy another inference server and does not launch runtime subprocesses.

## System shape

```text
OpenAI/Anthropic clients                 React dashboard
          │                                    │
          └──────── HTTP + SSE ────────────────┘
                               │
                    apps/inferdeck-gateway
                  validation · auth · streaming
                               │
               ┌───────────────┴───────────────┐
               │ shared priority/aging queue   │
               │ cancellation · 30s admission  │
               └───────────────┬───────────────┘
                               │
                     BackendCoordinator
          residency · slot capacity · VRAM fit · eviction
                               │
                         ModelRegistry
                  runtime-keyed native factories
                               │
       ┌───────────────┬───────┴────────┬──────────────┐
       │ llama.cpp     │ stable-        │ whisper.cpp  │ sherpa-onnx
       │ text/embed    │ diffusion.cpp  │ STT          │ TTS
       │ Vulkan        │ Vulkan         │ GPU          │ CPU/CUDA
       └───────────────┴────────────────┴──────────────┘
```

All backends implement `IBackend`, which owns lifecycle and capacity:

- immutable model/runtime/modality/capability metadata;
- load and unload;
- loaded state and estimated VRAM;
- slot acquire/release and optional slot resizing.

Modality interfaces add only their execution contract:

- `IModel` for chat and Responses;
- `IEmbeddingBackend` for embeddings;
- `IImageBackend` for image generation;
- `ISpeechBackend` for text-to-speech;
- `ITranscriptionBackend` for speech-to-text.

Routes dispatch through the coordinator and a typed modality interface. They never cast directly to a concrete runtime.

## Request lifecycle

1. The route validates and bounds the complete request before admission.
2. The shared coordinator queue records model, priority, arrival time, deadline, and cancellation callback.
3. The head request asks the resource planner to make its model resident.
4. The planner uses configured or DXGI-reported VRAM minus the safety margin. It may keep the current residents, shrink idle calibrated slot pools, evict an idle resident, or reject the request.
5. A slot increments the per-model and global active-request counts. Inference runs without holding the coordinator mutex.
6. Client disconnect or dashboard cancellation reaches the native runtime callback.
7. The route streams or returns output, records metrics/SQLite/EventBus activity, releases the slot, and leaves model residency to policy.

This lifecycle is shared by text, embeddings, image, TTS, and STT. A swap or load does not create a second modality-specific queue.

## Residency and automatic expansion

`BackendCoordinator` can keep multiple models resident when their estimated footprints fit. `gateway.vram_budget_mb` overrides hardware detection; otherwise DXGI total VRAM activates multi-residency. `gateway.vram_safety_margin_mb` is always reserved.

For a resident model with calibrated `vram_fixed_mb` and `vram_per_slot_mb`, the planner may reduce slots down to `min_slots`. It never guesses slot savings. Active models are not resized or evicted. If preparation fails, the coordinator preserves or restores the previous usable residency where possible and returns a typed error.

When no VRAM budget is known, the coordinator retains the conservative single-resident swap behavior.

## Runtime registration

The registry maps a YAML runtime id to a factory. `llama_cpp` is always registered. Optional media factories are registered only when their native libraries were linked at build time. `/v1/models` reports `runtime_available`; attempting to load an unlinked runtime returns `runtime_unavailable` instead of starting a fake backend.

This boundary supports additional in-process providers. vLLM is intentionally excluded because it requires a Python/CUDA service and conflicts with the no-subprocess/no-proxy requirement. A future provider must expose a native C/C++ library, implement `IBackend` plus the relevant modality interface, and use the same coordinator.

## API surface

OpenAI-compatible routes:

- `POST /v1/chat/completions`
- `POST /v1/responses`
- `POST /v1/embeddings`
- `POST /v1/images/generations`
- `POST /v1/audio/speech`
- `POST /v1/audio/transcriptions`
- `GET /v1/models`

Anthropic compatibility remains at `POST /v1/messages` and `/v1/messages/count_tokens`.

InferDeck control routes cover model load/unload, swap status/cancellation, media job cancellation, metrics, history, configuration, and the model store. Dashboard live state uses one SSE connection; there is no WebSocket layer.

Responses is stateless. Storage/background/conversation parameters are rejected rather than silently retained.

## Model store

The model store uses Hugging Face metadata and resolver endpoints through native WinHTTP. A background job downloads to a confined `.partial` path, supports HTTP Range resume and cancellation, checks free disk space, validates exact size and SHA-256, atomically finalizes the artifact, then updates `installed.json` and the runtime registry. A partial or corrupt artifact is never registered.

Removal is limited to store-managed paths. Loaded or active models cannot be removed.

## Configuration

`config/gateway.yml` remains the only active configuration source. The dashboard retrieves a secret-masked document and an optimistic revision. Common controls modify the YAML document while retaining comments and unknown keys; the full editor covers all settings. The server restores unchanged secret sentinels, validates the complete document, and atomically replaces the file. Changes require an operator restart and never restart the production process automatically.

Model entries contain runtime-neutral fields plus an optional `artifacts` map for runtime-specific files. Native examples and build pins are in `docs/alpha-v2-native-runtimes.md`.

## Persistence boundary

InferDeck persists operational data only:

- configured and store-installed model artifacts;
- model-store manifest and partial downloads;
- YAML configuration;
- request/swap metrics and logs.

Generated images, synthesized audio, uploaded audio, transcripts, chat output, and Responses state are request-scoped and are not retained.

## Concurrency invariants

- Inference never runs while holding the coordinator mutex.
- Unload drains active requests before destroying a backend.
- Slot release is idempotently owned by the route or stream state, never both.
- Streaming state outlives both its inference thread and HTTP provider.
- Native cancellation callbacks must terminate work and release GPU capacity.
- stable-diffusion.cpp generation is serialized while its upstream progress callback remains process-global.
- Runtime absence is visible; no unavailable path returns synthetic success.

## Source layout

```text
apps/inferdeck-gateway/       executable wiring and static dashboard serving
apps/dashboard/               React dashboard
libs/model/                   contracts, registry, shared queue/coordinator
libs/llama_cpp_wrapper/       in-process llama.cpp implementation
libs/native_runtimes/         optional image, TTS, and STT adapters
libs/gateway/                 protocol routes, streaming, model store, SSE
libs/observability/           GPU telemetry, metrics, SQLite
libs/foundation/              Result/Error, logging, EventBus
```
