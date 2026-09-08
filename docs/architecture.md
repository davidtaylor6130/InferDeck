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
- `IAudioGenerationBackend` for music generation;
- `ISpeechBackend` for text-to-speech;
- `ITranscriptionBackend` for speech-to-text.

Routes dispatch through the coordinator and a typed modality interface. They never cast directly to a concrete runtime.

## Request lifecycle

1. The route validates and bounds the complete request before admission.
2. The shared coordinator queue records model, priority, arrival time, deadline, cancellation callback, and any client-scoped voice-session reservation.
3. The head request asks the resource planner to make its model resident.
4. The planner uses fresh observed GPU usage and total VRAM, capped by the configured budget and safety margin. It falls back to declared footprints when telemetry is stale or unavailable. It may keep the current residents, shrink idle calibrated slot pools, evict an idle resident, or reject the request.
5. A slot increments the per-model and global active-request counts. Inference runs without holding the coordinator mutex.
6. Client disconnect or dashboard cancellation reaches the native runtime callback.
7. The route streams or returns output, records metrics/SQLite/EventBus activity, releases the slot, and leaves model residency to policy.

This lifecycle is shared by text, embeddings, image, music generation, TTS,
and STT. A swap or load does not create a second modality-specific queue.
Resident models with independent admission pools may execute at the same time.

Successful STT reserves the configured default conversation model for the same client through the STT-to-chat hand-off. The matching chat runs at media priority, and TTS releases the reservation. `gateway.voice_session_grace_ms` bounds abandoned sessions; clients behind a shared address can send `X-InferDeck-Voice-Session` to provide a distinct key.

Priority is preemptive at queue and swap boundaries. A native backend load that has already begun remains non-preemptive because model libraries do not expose a safe generic cancellation point; CPU voice can still run, and the reserved conversation model is selected at the next safe swap boundary.

## Residency and automatic expansion

`BackendCoordinator` can keep multiple models resident when their actual
headroom fits. The gateway refreshes used and total VRAM from telemetry; a
sample expires after three seconds and every load, unload, or slot resize
invalidates it. Live headroom is used only when every resident GPU runtime can
account for its future peak. Otherwise the planner uses the more conservative
of declared availability and observed pressure with the incomplete runtime's
full declared footprint reserved.
`gateway.vram_budget_mb` caps hardware detection and
`gateway.vram_safety_margin_mb` defaults to 1024 MB and may be set to 0. It reserves GPU headroom only; host/system memory reserve is separate.

Lazy native runtimes reserve their declared peak beyond memory already retained
by the runtime. This prevents an unloaded phase from being double-spent while
allowing warmed modules already present in the observed usage to count once.
Execution uses independent per-model slots and does not hold the coordinator
mutex. Lifecycle loads, resizes, and evictions remain serialized because
overlapping native model initialization is unsafe.

For a resident model with calibrated `vram_fixed_mb` and `vram_per_slot_mb`, the planner may reduce slots down to `min_slots`. It never guesses slot savings. Active models are not resized or evicted. If preparation fails, the coordinator preserves or restores the previous usable residency where possible and returns a typed error.

Automatic unified context pooling is measured and bounded. It preserves each request context limit, shares capacity within a model, and may reclaim idle slot cache. Recreating an idle context keeps model weights and a vision projector resident but clears that slot cache. Active slots are protected.

Enable automatic fit on an individual model entry with `kv_unified: true`,
`context_pool_auto: true`, and `context_pool_size: 0`. Keep `context_size` as
that model's per-request limit and `n_slots` as its maximum concurrent requests.
For a manually bounded pool, disable `context_pool_auto` and set a positive
`context_pool_size`. Automatic mode and a fixed pool size are mutually exclusive.

The pool is shared storage for independent sequences within one model. Different
models retain separate weights and KV tensors. Physical fit accounts for target,
draft and execution buffers; it does not promise every slot its maximum context
simultaneously. Requests wait when their reserved prompt/output capacity cannot
fit. Idle context reclamation can save a model reload but discards its cached
conversation state; later requests may need prefill again.

The dashboard's Settings > Configuration & recovery page exposes the VRAM
reserve. Saving it reloads configuration. Zero disables the extra GPU reserve;
it does not disable allocation checks or the separate host-memory budget.


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

InferDeck data-plane routes:

- `POST /api/inferdeck/v1/audio/generations`

`strict_openai` is the only Core profile. It owns no non-OpenAI protocol.
OpenAI-derivative routes use a separate disabled-by-default compatibility
prefix, while InferDeck-specific contracts remain under `/api/inferdeck/v1`.

InferDeck control routes cover model load/unload, swap status/cancellation,
dashboard image/music generation, media job cancellation and output retrieval,
metrics, history, configuration, model aliases, and the model store. Dashboard
live state uses one SSE connection; there is no WebSocket layer.

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

Each AI section owns a route-scoped Model Store with Discover, Downloads, and Installed views. Discover queries Hugging Face for locally compatible artifacts, defaults to trending results, and supports search, popularity or recency sorting, gated-model opt-in, and relevant runtime or VRAM filters. Repository inspection revalidates the exact standalone artifact or complete native bundle before installation. Archive and permanent delete remain explicit, confirmed actions and refuse loaded or active models.

## Post-training boundary

InferDeck currently implements GGUF quantisation, not fine-tuning. A
control-plane request starts one background call to llama.cpp's public
`llama_model_quantize` API. The source must be an unloaded, managed, regular
GGUF file inside the model-store root. The destination is server-derived inside
that root, written as a new partial artifact, hashed, finalized without
overwrite, added to the manifest, and registered as a separate model. Q4_K_M,
Q5_K_M, Q6_K, and Q8_0 are supported. Requantisation is disabled.

The upstream call has no progress or cancellation callback, so the API reports
the job as non-cancellable and shutdown waits for it to finish. Full-model FP32
training in the vendored example remains experimental, and the public backend
does not expose a compatible LoRA training and save path. The capability route
reports fine-tuning as unavailable rather than presenting an unsafe workflow.

Quantisation atomically owns the shared CPU maintenance resource from admission
until either installation or failure. This blocks competing CPU-backed model
work, configuration mutation, and new background leases while leaving
GPU-backed inference eligible. Background availability reports `maintenance`
with a suggested report-back time. The measured benchmark and quantisation
workers release only reservations they own, so one maintenance subsystem cannot
clear the other's resource state.

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

SQLite uses WAL mode and schema version 4. Schema 4 adds swap identity fields. Before migration, SQLite creates a `stats.db.backup-v<old-version>` backup with `.backup` and applies the migration in one transaction. Older binaries reject newer schemas. Rollback must restore the matching executable, configuration, and database; post-backup history is lost, so preserve the newer database separately.
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

Generated images and music are retained in a 100-job, 2 GB bounded media
history beside the configured stats database. Synthesized speech, uploaded
audio, transcripts, chat output, and Responses state remain request-scoped and
are not retained.

## Concurrency invariants

- Inference never runs while holding the coordinator mutex.
- Unload drains active requests before destroying a backend.
- Slot release is idempotently owned by the route or stream state, never both.
- Streaming state outlives both its inference thread and HTTP provider.
- Native cancellation callbacks must terminate work and release GPU capacity.
- Non-cancellable llama.cpp quantisation is limited to one job and joined on
  shutdown so a partial model is never presented as installed.
- stable-diffusion.cpp generation is serialized while its upstream progress callback remains process-global.
- acestep.cpp music generation uses one slot and strict module eviction; jobs
  are serialized across ACE-Step models.
- Runtime absence is visible; no unavailable path returns synthetic success.

## Source layout

The source layout includes benchmark implementation modules and stream serialization and control YAML modules.

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
