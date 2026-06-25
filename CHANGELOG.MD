# InferDeck - Changelog

## [0.3.0] - 2026-06-25

### Added
- **Server-side configurable sampling with per-model overrides.** A
  `gateway.sampling` block sets global sampler defaults (temperature, top_p,
  top_k, min_p, repeat/DRY penalties) mirroring stock llama-server, with
  per-entry overrides under `model_registry`. Explicit per-request values still
  take precedence. Closes [#42](https://github.com/davidtaylor6130/InferDeck/issues/42)

### Fixed
- **Scheduler KV reuse clamped to cached tokens; slot KV preserved across turns**,
  preventing context loss between requests. Closes [#43](https://github.com/davidtaylor6130/InferDeck/issues/43)
- **History-aware prompt truncation and raised context sizes**, so long
  conversations are trimmed correctly instead of overflowing the context window.
  Closes [#38](https://github.com/davidtaylor6130/InferDeck/issues/38),
  [#40](https://github.com/davidtaylor6130/InferDeck/issues/40)

---

## [0.2.1] - 2026-06-21

### Versioning note
Prior to this release the project used an inconsistent `2.x.x` scheme that
didn't reflect actual maturity. All version strings are now standardised to
`0.x.x` semver. The old `2.x.x` entries have been renumbered into `0.2.0`
below where they belong chronologically.

### Added
- **Continuous batching — true parallel token generation across slots.**
  Replaced per-slot `llama_context` + `decode_mtx_` serialization with a single
  shared context (`n_ctx = context_size × n_slots`, `n_seq_max = n_slots`) and a
  `ContinuousBatchScheduler` background thread. Each decode step collects one
  token from every active slot, packs them into a single `llama_batch`, and calls
  `llama_decode` once — the GPU processes all slots in parallel rather than
  sequentially. Verified on Qwen3.6-27B (n_slots=3): two concurrent 10,000-word
  story requests completed at 23.6 and 26.3 tps with no throughput degradation
  vs single-slot baseline (~22 tps). KV-cache reuse preserved per-slot via
  `llama_memory_seq_rm`. Closes [#36](https://github.com/davidtaylor6130/InferDeck/issues/36)

---

## [0.2.0] - 2026-06-11/12

Full rewrite of the inference stack and gateway in C++23. Replaced the
original TypeScript/Node gateway-service with a single in-process C++
executable linking llama.cpp directly.

### Added
- **Anthropic Messages API** so Claude Code and other Anthropic-API clients
  can run local models: `POST /v1/messages` (streaming + non-streaming) and
  `POST /v1/messages/count_tokens`. Translates Anthropic requests (system,
  content blocks, tool_use/tool_result, images, tools, tool_choice,
  stop_sequences) to the internal OpenAI-shaped pipeline and emits Anthropic
  SSE events with Anthropic error envelopes
- Model resolution for Anthropic clients: exact registry match →
  `anthropic.model_aliases` config map → currently loaded model →
  `default_model`, so `claude-*` model names just work
- `x-api-key` header accepted as auth (Anthropic SDK convention)
- Real SSE event stream at `GET /api/events/stream`: `stats` (1 Hz GPU /
  requests / lifetime counters), `model` (swap lifecycle), and `request`
  (completion) events, backed by a new foundation EventBus
- Non-blocking model swaps: `POST /v1/swap/to/:name` returns 202 and
  publishes progress events; `POST /v1/swap/cancel`; SwapTracker state
  via `GET /v1/swap/status` and `/api/status`
- `GET /api/pricing` serving `data/pricing.json` for dashboard cost defaults
- p50/p95 latency and per-model average t/s in `/api/status`
- Dashboard vitest suite (cost engine + page rendering)
- Read Vulkan SDK path from environment variable instead of hardcoding. Closes [#23](https://github.com/davidtaylor6130/InferDeck/issues/23)

### Fixed
- 500 errors that blocked opencode auto-compaction at the context threshold:
  assistant messages with neither content nor tool_calls (produced by context
  pruning) made message parsing throw; missing content now defaults to `""`,
  template application is exception-guarded, and parse/template failures return
  `400 invalid_request_error` instead of an opaque 500. Closes [#28](https://github.com/davidtaylor6130/InferDeck/issues/28)
- Oversized prompts are now truncated llama-server style (keep first 1024
  tokens + most recent tail, drop the middle) instead of erroring, so sessions
  that grew past `n_ctx` can still compact and continue; disable with
  `gateway.truncate_prompt: false`
- Inference failures logged with full status, code, and message
- Follow-up turns that strictly extend the prompt cache no longer rewind the KV
  (generated tokens are tracked); 4k-token follow-up on qwen2.5-coder-3b:
  792 ms → 47 ms. Closes [#18](https://github.com/davidtaylor6130/InferDeck/issues/18)
- VRAM leak when model context init fails mid-slot (OOM on slot N left slots 0..N-1 allocated). Closes [#31](https://github.com/davidtaylor6130/InferDeck/issues/31)
- Auto-swap behaviour resolved: enabled by default, returns 503 on mid-swap requests. Closes [#19](https://github.com/davidtaylor6130/InferDeck/issues/19)

### Changed
- Dashboard fully redesigned: four pages (Overview, Models, Usage & Cost,
  System), SSE-first state with connected/reconnecting/offline handling and a
  30s polling fallback, async swap progress with cancel
- All `/api` routes moved from `main.cpp` into `libs/gateway/src/dashboard_routes.cpp`
- `BackendCoordinator` no longer holds its mutex across `predict/predict_stream`,
  so status queries and additional slots stay responsive during generation
- Root CMake installs `inferdeck-gateway` (was the dead `gateway-service`)

### Removed
- Entire v1 TypeScript stack: `apps/gateway-service`, `libs/core`,
  `libs/backends`, `libs/vector_store`, `libs/tests`, legacy wrapper sources
  (LlamaEngine, LlamaServerManager, GGUFParser, VulkanDevice), cypress
- Never-wired layers: `libs/scheduler`, `libs/engine`, `libs/sampling`,
  `libs/messaging`
- Duplicate/no-op routes that returned fake 200s
- ~70 MB of committed binaries, logs, debug payloads, and stale configs

### Known limitation
- qwen3.6-35b-a3b (hybrid recurrent attention) still re-prefills fully each
  turn; needs recurrent-state checkpoints (see AGENTS.md "Known open items")

---

## [0.1.0] - 2025-05-10

### Added — Closes [#1](https://github.com/davidtaylor6130/InferDeck/issues/1) [#2](https://github.com/davidtaylor6130/InferDeck/issues/2) [#3](https://github.com/davidtaylor6130/InferDeck/issues/3) [#4](https://github.com/davidtaylor6130/InferDeck/issues/4) [#5](https://github.com/davidtaylor6130/InferDeck/issues/5) [#6](https://github.com/davidtaylor6130/InferDeck/issues/6) [#7](https://github.com/davidtaylor6130/InferDeck/issues/7) [#8](https://github.com/davidtaylor6130/InferDeck/issues/8) [#9](https://github.com/davidtaylor6130/InferDeck/issues/9) [#10](https://github.com/davidtaylor6130/InferDeck/issues/10) [#11](https://github.com/davidtaylor6130/InferDeck/issues/11)
- Full React + Vite + Tailwind dashboard with live data
- Dashboard tabs: Dashboard, Jobs, Models, Services, Modes
- Complete job management UI with table, status badges, and cancel action
- Model management UI with pull, load, and delete operations
- Service health cards with real-time status indicators
- Mode switcher with three modes: AI, Gaming, Maintenance
- SSE events route for real-time queue updates
- API documentation at docs/api/overview.md
- Gateway-core build configuration (tsc output to dist/)
- Proper TypeScript builds for all packages
- .gitkeep files for data/logs and data/job-artifacts directories
- Hardware metrics type definitions in shared package

### Fixed
- events.routes.ts: Fixed undefined `ctx` — now retrieves queueStore from app context
- jobs.repo.ts: Fixed broken SQL — replaced all string interpolation with positional `?` parameters, eliminating SQL injection vulnerability
- jobs.repo.ts: Fixed unclosed template literal in listStmt
- jobs.repo.ts: Fixed `:daysDays` parameter typo in cleanupOldStmt
- jobs.repo.ts: Fixed reprioritize() and delete() endpoints to use parameterized queries
- models.routes.ts: Fixed /models/load to use `/api/show` instead of `/api/pull`
- models.routes.ts: Fixed /models/unload to use `/api/pull` with `keep_alive: "0"`
- PolicyEngine.ts: Fixed `linked` to `locked` in canExecuteGpuJob return value
- apiTypes.ts: Removed `"noop_test"` and `| string` from JobType union — now fully typed
- jobTypes.ts: Removed `| string` from ClientOrigin union — now fully typed
- PolicyEngine.ts: Fixed `_mode` private field with proper getter/setter

### Changed
- `.env.example`: Fixed `Ollama_HOST` to `OLLAMA_HOST`
- schema.ts: Changed `publicBaseUrl` default to `http://localhost:8721`
- schema.ts: Changed `executable` default to `ollama`
- schema.ts: Changed `hardware.provider` default from `"null"` to `"none"`
- App.tsx: Removed unused `react-router-dom` imports
- dashboard package.json: Removed unused `react-router-dom` dependency
- gateway-service package.json: Removed 9 unused dependencies
- Dashboard UI: Added footer, better error states, empty states, and cross-browser compatibility

### Removed
- Dead placeholder components from App.tsx
- Dead dashboard pages (JobsList.tsx, Dashboard.tsx)
- Unused Fastify dependencies from gateway-service package
