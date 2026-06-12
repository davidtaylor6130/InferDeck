# AGENTS.md — InferDeck 2.0

**For:** Future AI agents and developers working on this codebase.
**Read this first.** It saves you from re-deriving what the previous team learned.

## What is InferDeck?

A local AI gateway. Single Windows `.exe` that runs LLMs in-process via
llama.cpp (Vulkan), exposes an OpenAI-compatible HTTP API on
`:11434/v1/`, and serves a React dashboard on `:11434/`.

**Hard constraint:** Everything in-process. NO subprocess. NO
`llama-server.exe` proxy. NO orphan processes. This is non-negotiable
because the user is on Windows and orphan `llama-server.exe` processes
have been a pain point.

## Project Structure

```
apps/inferdeck-gateway/       .exe entry: config load, deps wiring, /v1 routes,
                              static file serving, stats publisher thread
apps/dashboard/               React dashboard source (pnpm build:dashboard
                              outputs into apps/inferdeck-gateway/static/,
                              which is committed)
apps/benchmark-runner/        inferdeck-bench optimization harness
apps/model-tester/            Swap exerciser dev tool
apps/hardware-adlx-helper/    Optional ADLX helper for GPU telemetry

libs/foundation/              logging (spdlog), Result/Error, EventBus
libs/model/                   ModelRegistry + BackendCoordinator + IModel
libs/llama_cpp_wrapper/       LlamaCppModel — the live llama.cpp wrapper
libs/gateway/                 HTTP routes (routes.cpp), dashboard /api routes
                              (dashboard_routes.cpp), SSE, streaming sanitizer,
                              SwapTracker, auth, CORS, metrics builder
libs/observability/           GPU telemetry (PDH/DXGI), Metrics, SQLite StatsDb
libs/optimize/                In-house search (random + greedy) for inferdeck-bench
libs/third_party/llama.cpp    Vendored, Vulkan build (gitignored — cloned, not committed)

config/gateway.yml            Active config (only this one is read)
config/gateway.test-ralph.yml Small-model config for the mini-ralph harness (port 11435)
config/sampler-profiles/      Per-model sampler configs
data/pricing.json             Dashboard cost defaults, served via GET /api/pricing

Testing/                      mini-ralph.mjs streaming tool-call harness (untracked,
                              do not delete)
tests/integration/            Catch2 integration tests (mocked coordinator)
tests/parity/                 Parity with raw llama-server
tests/fixtures/               Realistic request payloads (opencode/openwebui/Anthropic)
```

Deleted in the 2026-06 cleanup (see `docs/v2-cleanup-report.md`, history
has the code): the entire v1 stack (`apps/gateway-service`, `libs/core`,
`libs/backends`, `libs/vector_store`, `libs/tests`, `LlamaEngine.cpp`)
and the never-wired layers `libs/{scheduler,engine,sampling,messaging}`.
Queuing/LCP slot matching will be rebuilt against `BackendCoordinator`
when multi-backend support lands — do not resurrect the old layers.

## Build Commands

```bash
# Configure (one-time; add -DINFERDECK_BUILD_TESTS=ON for tests)
cmake -S . -B build -G "Visual Studio 17 2022" -A x64

# Build the gateway
cmake --build build --target inferdeck-gateway --config Release -j

# Dashboard (output goes to apps/inferdeck-gateway/static/, commit it)
pnpm --filter dashboard build
```

## Test Commands

```bash
# C++ unit + integration (use -L, NOT -LE: vendored brotli registers
# hundreds of its own ctest entries that fail and are not ours)
ctest --test-dir build -C Release --output-on-failure -L "unit|integration"

# Dashboard unit tests
pnpm --filter dashboard test

# Streaming tool-call harness (starts nothing; needs a running gateway)
build/bin/Release/inferdeck-gateway.exe -c config/gateway.test-ralph.yml
GATEWAY_URL=http://127.0.0.1:11435 GATEWAY_MODEL=qwen2.5-coder-3b node Testing/mini-ralph.mjs
# Note: the 3B model is nondeterministic; a single FAIL on one iteration
# can be flake — rerun before assuming a regression.

# Parity (slow)
bash tests/parity/run.sh
```

## Deployment

The live server runs from `C:\InferDeck\bin\`, launched by
`C:\InferDeck\Start-InferDeck.ps1` (scheduled tasks at startup/logon
plus a 15-min watchdog). A deploy requires copying **both** artifacts —
updating only one leaves a mismatched install:

- `build/bin/Release/inferdeck-gateway.exe` → `C:\InferDeck\bin\gateway-service.exe`
- `apps/inferdeck-gateway/static/` (index.html + assets) → `C:\InferDeck\bin\static/`

The gateway serves the dashboard from `executable_dir()/static`, so the
exe and its static dir must come from the same build. Replacing just the
exe makes the new gateway serve the old dashboard. The exe can't be
overwritten while running — stop the process (or rename the file aside)
first. Clear stale hashed bundles in `bin/static/assets/` so only the
current `index-*.js`/`index-*.css` remain.

## HTTP API

OpenAI-compatible (`/v1`):
- `POST /v1/chat/completions` — streaming + non-streaming, tool calls,
  reasoning_content. SSE chunks end with `data: [DONE]`.
- `GET /v1/models`, `GET /v1/health`, `GET /v1/metrics`, `GET /v1/stats/history`

Swap control:
- `POST /v1/swap/to/:name` — **async**: returns `202 {"status":"swapping"}`
  immediately (200 if already loaded, 404 unknown, 409 if a swap is running).
  Progress arrives as `model` events on the SSE stream.
- `POST /v1/swap/cancel` — requests cancellation of the in-flight swap.
- `GET /v1/swap/status` — loaded model, vram, active requests, SwapTracker state.

Dashboard (`/api`, registered in `libs/gateway/src/dashboard_routes.cpp`):
- `GET /api/status` — queue/swap/hardware/summary (incl. p50/p95 latency),
  tokenUsage, monthlyTokenUsage
- `GET /api/jobs`, `GET /api/logs`, `GET /api/pricing` (serves data/pricing.json)
- `POST /api/models/load` (async, same path as /v1/swap/to), `POST /api/models/unload`
- `GET /api/events/stream` — **SSE** (there is no WebSocket anywhere).
  Named events, each ~1Hz or on occurrence:
  - `stats`: gpu{utilizationPct,vramUsedMb,temperatureC,powerW}, loadedModel,
    activeRequests, swapping, lifetime counters, uptime
  - `model`: state=swapping|ready|failed|cancelled|unloaded, from, to, durationMs, error
  - `request`: model, tokens, durationMs, tokensPerSecond, status

## Architecture Quick Reference

Request flow for `/v1/chat/completions`:

1. `routes.cpp::handle_chat_completions` parses the OAI body
2. (optional) auto-swap via the tracked swap path
3. `BackendCoordinator::acquire_slot(model)` (blocks up to 30s)
4. non-stream: `coordinator.predict(...)`; stream: inference runs on a
   dedicated thread pushing `InferenceDelta`s through a cv-guarded queue
   consumed by httplib's chunked content provider (2s heartbeat `: \n\n`)
5. `LlamaCppModel` (libs/llama_cpp_wrapper) does template/tokenize/decode/sample
6. `record_request` writes Metrics + SQLite StatsDb and publishes a
   `request` event on the EventBus

Swap flow: `start_swap_async` (routes.cpp) → SwapTracker.begin →
detached thread → `coordinator.swap_to_cancellable` (drains active
requests, unloads, loads) → record + publish `model` event →
SwapTracker.end. main.cpp waits up to 120s for an in-flight swap at
shutdown.

Concurrency invariants:
- `BackendCoordinator::predict/predict_stream` must NOT hold `mutex_`
  during inference (fixed 2026-06; holding it froze /api/status and
  second-slot acquisition for the whole generation).
- Slot acquisition increments `active_requests_`; `unload` drains it
  with a 30s timeout, so an IModel can't be destroyed mid-predict.
- `StreamState` is shared_ptr-owned by both the inference thread and
  the chunked provider; `finish_once` is idempotent via CAS.

## Design rules learned the hard way (do not regress)

- KV-cache reuse must use longest-common-prefix matching, never message
  count. Reference `server_prompt_cache` in vendored llama.cpp.
- Use `common_sampler_init` (`common/sampling.cpp`) rather than
  hand-rolling sampler chains that fight model defaults.
- Streaming sanitizers must be cursor-based (scan only new bytes) —
  the v1 O(n²) rescan was a real perf bug.
- Never hardcode `add_bos`; use `llama_vocab_get_add_bos(vocab)`.
- Tool-call JSON is parsed from sampler output, not regexed.

## Known open items

- **qwen3.6-35b-a3b re-prefills the whole prompt every turn** (~60s at
  80k ctx). It is a hybrid recurrent/linear-attention model: the
  recurrent state cannot be rewound to an arbitrary position
  (`llama_memory_seq_rm` mid-sequence fails, `pos_min==pos_max` in the
  `llama_prompt_cache_fallback` log line), and thinking-model history
  re-rendering always diverges just before the generation boundary, so
  a rewind is always needed. Fix = llama-server-style recurrent-state
  checkpoints (snapshot before each generation, restore on rewind).
  Full-attention models reuse the cache fine, and follow-up turns that
  strictly extend the cache skip the rewind entirely
  (`llama_prompt_cache_extend`). `swa_full` does not help; keep false.

- `config/gateway.yml` has `gateway.auto_swap: true` while the original
  design said "no auto-swap, return 503". Both paths exist in
  `handle_chat_completions`; the config flag decides. Owner decision on
  the default is still pending (see docs/v2-cleanup-report.md §3.9).
- Remaining report items not yet implemented: error-code enum on
  foundation::Error instead of message substring matching (§3.3/3.4),
  UTF-8 hold-back in the streaming path (§3.6), god-function splits
  (§4.1/4.2), sampler magic numbers → sampler-profiles (§3.10),
  Vulkan SDK path from env (§3.11).

## Don'ts

- **NO subprocess.** Don't call `llama-server.exe`. Don't `system()` or `popen()`.
- **NO proxy design.** Use the llama.cpp library directly.
- **NO MMProj load on demand.** mmproj stays loaded for vision models.
- **NO MTP for now.** Conflicts with n_parallel>1.
- **NO comments unless asked.**

## Dashboard

React 19 + Vite + Tailwind in `apps/dashboard/`. Four pages:
**Overview** (model card + swap progress/cancel, live SSE sparklines,
lifetime counters, activity feed), **Models** (registry table with
async load/cancel/unload, swap history), **Usage & Cost** (token/cost
graph, per-model table; price defaults come from `GET /api/pricing`,
user overrides persist in localStorage under
`inferdeck:model-token-costs`), **System** (hardware meters, log
viewer).

State comes from one `EventSource('/api/events/stream')` in
`src/gateway.tsx` with a connected/reconnecting/offline state machine
and a 30s `/api/status` polling fallback. API base is same-origin
(`VITE_API_BASE` override for `pnpm dev`, which proxies /api and /v1
to :11434).

Build with `pnpm --filter dashboard build`; the output in
`apps/inferdeck-gateway/static/` is committed and copied next to the
exe post-build by CMake.

## Observability

- Requests: timestamp, model, prompt/completion tokens, duration, t/s,
  status, slot — in-memory `Metrics` + SQLite `StatsDb`
  (`observability.stats_db` path in gateway.yml) + `request` SSE event.
- Swaps: from, to, duration, success, error — same three sinks.
- GPU: PDH/DXGI polling (`telemetry_poll_ms`), optional ADLX helper for
  temperature/power (`observability.adlx_helper`).

## When Stuck

1. **Tool calls failing?** Check which process owns :11434 first
   (`Get-NetTCPConnection -LocalPort 11434`); a stale process serving the
   old code has burned hours before.
2. **Slow first token?** LCP miss — check prompt-cache behavior in
   llama_cpp_model.cpp.
3. **Swap stuck?** `GET /v1/swap/status`, then `POST /v1/swap/cancel`.
   Partial state after a cancel is acceptable; the next swap re-unloads.
4. **Dashboard frozen during generation?** That bug was the coordinator
   holding `mutex_` across predict — do not reintroduce it.
5. **ctest reports hundreds of failures?** You ran `-LE e2e` and hit
   vendored brotli's tests. Use `-L "unit|integration"`.
