# InferDeck Alpha V2 architecture

InferDeck is one native gateway process that admits, loads, executes, observes, and unloads local AI models. It does not proxy another inference server and does not launch runtime subprocesses.

## System shape

```text
OpenAI clients                           React dashboard
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
2. The shared coordinator queue records model, priority, arrival time, deadline, cancellation callback, and any client-scoped voice-session reservation.
3. The head request asks the resource planner to make its model resident.
4. The planner uses configured or DXGI-reported VRAM minus the safety margin. It may keep the current residents, shrink idle calibrated slot pools, evict an idle resident, or reject the request.
5. A slot increments the per-model and global active-request counts. Inference runs without holding the coordinator mutex.
6. Client disconnect or dashboard cancellation reaches the native runtime callback.
7. The route streams or returns output, records metrics/SQLite/EventBus activity, releases the slot, and leaves model residency to policy.

This lifecycle is shared by text, embeddings, image, TTS, and STT. A swap or load does not create a second modality-specific queue.

Successful STT reserves the configured default conversation model for the same client through the STT-to-chat hand-off. The matching chat runs at media priority, and TTS releases the reservation. `gateway.voice_session_grace_ms` bounds abandoned sessions; clients behind a shared address can send `X-InferDeck-Voice-Session` to provide a distinct key.

Priority is preemptive at queue and swap boundaries. A native backend load that has already begun remains non-preemptive because model libraries do not expose a safe generic cancellation point; CPU voice can still run, and the reserved conversation model is selected at the next safe swap boundary.

## Residency and automatic expansion

`BackendCoordinator` can keep multiple models resident when their estimated footprints fit. `gateway.vram_budget_mb` overrides hardware detection; otherwise DXGI total VRAM activates multi-residency. `gateway.vram_safety_margin_mb` is always reserved.

For a resident model with calibrated `vram_fixed_mb` and `vram_per_slot_mb`, the planner may reduce slots down to `min_slots`. It never guesses slot savings. Active models are not resized or evicted. If preparation fails, the coordinator preserves or restores the previous usable residency where possible and returns a typed error.

When no VRAM budget is known, the coordinator retains the conservative single-resident swap behavior.

## Runtime registration

The registry maps a YAML runtime id to a factory. `llama_cpp` is always registered. Optional media factories are registered only when their native libraries were linked at build time. `/api/inferdeck/v1/models` reports `runtime_available`; attempting to load an unlinked runtime returns `runtime_unavailable` instead of starting a fake backend.

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

`strict_openai` is the default profile. Optional OpenAI-derivative routes live
under `/compat/openai-derivative/v1`. They are disabled by default and never
add routes or fields to strict `/v1`. Core owns no non-OpenAI protocol.

InferDeck control routes cover model load/unload, swap status/cancellation, media job cancellation, metrics, history, configuration, model aliases, and the model store. Dashboard live state uses one SSE connection; there is no WebSocket layer.

Responses is stateless. Storage/background/conversation parameters are rejected rather than silently retained.

## Model store

The model store uses Hugging Face metadata and resolver endpoints through native WinHTTP. A background job downloads to a confined `.partial` path, supports HTTP Range resume and cancellation, checks free disk space, validates exact size and SHA-256, atomically finalizes the artifact, then updates `installed.json` and the runtime registry. Sherpa ONNX repositories are staged and validated as complete multi-file bundles before a single directory rename makes them visible. A partial or corrupt artifact is never registered.

Removal is limited to store-managed paths. Loaded or active models cannot be removed. Archive moves a managed artifact into `model_store.archive_root`; permanent delete removes it after an explicit dashboard confirmation.

## Configuration

`config/gateway.yml` remains the only active configuration source. The dashboard retrieves a secret-masked document and an optimistic revision. Common controls modify the YAML document while retaining comments and unknown keys; the full editor covers all settings. The server restores unchanged secret sentinels, validates the complete document, atomically replaces the active file, and applies it through the gateway's graceful in-process reload loop.

Model entries contain runtime-neutral fields plus an optional `artifacts` map for runtime-specific files. Native examples and build pins are in `docs/alpha-v2-native-runtimes.md`.

Per-model `prompt_price_per_million`, `cached_prompt_price_per_million`, and `completion_price_per_million` values are the server-owned token-pricing source. The pricing endpoint merges those values over packaged defaults, applies target pricing to model aliases, and the dashboard reports when neither source defines a model price.
Configuration schema, optimistic revisions, atomic persistence, secret restoration, and reload recovery are defined in [configuration-schema.md](configuration-schema.md).
Packaged pricing may define `legacy_cached_prompt_ratio` and `legacy_cached_prompt_before` for model history created before cache-hit accounting was available. Recorded cache counts always take precedence, and the estimate applies only to zero-cache usage before the configured date.

Stable aliases are stored in the root `model_aliases` sequence and managed through `/api/inferdeck/v1/model-aliases`. An alias points directly to a concrete registry model and captures its minimum context and required capabilities as a compatibility contract. Retargeting is rejected when the new concrete model cannot satisfy that contract. Discovery and request metrics preserve both the requested alias and resolved concrete model.

Each model can enable `optimization.schedule` with `window_start` and `window_end` in `HH:MM`. The default window is 03:00–04:00 in the gateway host's local timezone. A scheduled benchmark starts at most once per local calendar day and only while the request queue is idle, no swap is active, and GPU utilization is at most 20 percent. The dashboard exposes the server timezone plus next and last run status.

## Dashboard model management

Model Settings owns runtime, capacity, pricing, sampler, and optimization controls. A completed optimization run is only a recommendation until the user selects **Use these values** and saves; **Discard results**, **Rerun**, closing, and cancellation never alter the active profile. Icon-only load, unload, settings, and close actions expose keyboard focus, accessible names, and tooltips.

Models owns stable aliases plus catalogue and installed-artifact operations. Catalogue filters combine name, runtime, modality, selected VRAM capacity, and Hugging Face download/like popularity. The active filter summary includes a one-step reset. Archive and permanent delete remain explicit, confirmed actions and refuse loaded or active models.

## Throughput and usage semantics

- One canonical request record feeds in-memory metrics, the SQLite ledger,
  structured completion logs, and `request` SSE events. It carries the request
  ID, principal class, endpoint/profile, modality, requested and resolved model,
  outcome, phase timings, token classes, and modality-specific usage units.
- The request ID echoed in `X-Request-Id` is the correlation key for access
  logs, completion logs, SQLite rows, and SSE events.
- **TPS** is generated tokens divided by scheduler-measured generation time; prompt prefill is excluded.
- **Prompt processing** is uncached prompt tokens divided by the scheduler-measured prefill interval.
- **Peak TPS** and peak prompt-processing speed are maxima from comparable successful requests with measured phase timings, never configured estimates.
- A missing duration produces an unavailable dashboard value rather than a fabricated zero-speed measurement.

The LLM Usage range selector drives the chart, summary totals, per-model requests and tokens, weighted throughput, peaks, and cost through the same hourly, daily, or monthly buckets. The table headers are keyboard-sortable, expose `aria-sort`, start alphabetically for model names and highest-first for numeric columns, and reverse on a second activation. Lifetime data is only shown where it is labelled lifetime.

SQLite uses WAL mode and schema version 2. Upgrading a disk ledger creates a
`stats.db.backup-v<old-version>` backup and applies the migration in one
transaction; failure rolls back without exposing a partially upgraded schema.
Prepared insert statements remain open for the database lifetime. Dashboard
lifetime totals are folded from the same all-time daily buckets used by cost
views, so lifetime and date-aware cache pricing cannot diverge. Diagnostic jobs
accept `protocol_profile` and `endpoint` filters.

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
