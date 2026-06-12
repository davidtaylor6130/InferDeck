# InferDeck - Changelog

## [2.2.0] - 2026-06-12

### Added
- Anthropic Messages API support so Claude Code can run local models:
  POST /v1/messages (streaming + non-streaming) and
  POST /v1/messages/count_tokens. Translates Anthropic requests
  (system, content blocks, tool_use/tool_result, images, tools,
  tool_choice, stop_sequences) to the internal OpenAI-shaped pipeline
  and emits Anthropic SSE events (message_start, content_block_start/
  delta/stop with input_json_delta for tool calls, message_delta,
  message_stop, ping heartbeat) with Anthropic error envelopes
- Model resolution for Anthropic clients: exact registry match >
  anthropic.model_aliases config map > currently loaded model >
  default_model, so claude-* model names just work
- x-api-key header accepted as auth (Anthropic SDK convention),
  mapped onto the existing Bearer token check
- Usage: ANTHROPIC_BASE_URL=http://localhost:11434
  ANTHROPIC_AUTH_TOKEN=anything claude

## [2.1.1] - 2026-06-11

### Fixed
- 500 errors that blocked opencode auto-compaction at the context
  threshold: assistant messages with neither content nor tool_calls
  (produced by context pruning) made message parsing throw; missing
  content now defaults to "", template application is exception-guarded,
  and parse/template failures return 400 invalid_request_error instead
  of an opaque 500
- Oversized prompts are now truncated llama-server style (keep first
  1024 tokens + most recent tail, drop the middle, logged as
  llama_prompt_truncated) instead of erroring, so sessions that grew
  past n_ctx can still compact and continue; disable with
  gateway.truncate_prompt: false
- Inference failures are now logged with status, code, and full message
  (previously only `reason=inference_error`)
- Follow-up turns that strictly extend the prompt cache no longer rewind
  the KV (generated tokens are tracked); 4k-token follow-up on
  qwen2.5-coder-3b: 792ms -> 47ms

### Known limitation
- qwen3.6-35b-a3b (hybrid recurrent attention + thinking) still
  re-prefills fully each turn; needs recurrent-state checkpoints
  (see AGENTS.md "Known open items")

## [2.1.0] - 2026-06-11

### Added
- Real SSE event stream at `GET /api/events/stream`: `stats` (1Hz GPU/
  requests/lifetime counters), `model` (swap lifecycle), and `request`
  (completion) events, backed by a new foundation EventBus
- Non-blocking model swaps: `POST /v1/swap/to/:name` returns 202 and
  publishes progress events; new `POST /v1/swap/cancel`; SwapTracker
  state exposed via `GET /v1/swap/status` and `/api/status`
- `GET /api/pricing` serving `data/pricing.json` for dashboard cost defaults
- p50/p95 latency and per-model average t/s in `/api/status`
- Dashboard vitest suite (cost engine + page rendering)

### Changed
- Dashboard fully redesigned: four pages (Overview, Models, Usage & Cost,
  System), SSE-first state with connected/reconnecting/offline handling
  and a 30s polling fallback (was 4s polling), async swap progress with
  cancel, same-origin API base, cost UI preserved with localStorage
  overrides intact
- All `/api` routes moved from main.cpp into `libs/gateway/src/dashboard_routes.cpp`
- BackendCoordinator no longer holds its mutex across predict/predict_stream,
  so status queries and additional slots work during generation
- Root CMake installs `inferdeck-gateway` (was the dead `gateway-service`)
  and drops the broken `certs/` install rule

### Removed
- Entire v1 stack: `apps/gateway-service`, `libs/core`, `libs/backends`,
  `libs/vector_store`, `libs/tests`, legacy wrapper sources
  (LlamaEngine, LlamaServerManager, GGUFParser, VulkanDevice), cypress
- Never-wired v2 layers: `libs/scheduler`, `libs/engine`, `libs/sampling`,
  `libs/messaging` (and their messaging round-trip tests)
- Duplicate/no-op routes: `/api/health`, `/api/models`, `/api/models/running`,
  `/api/services`, `POST /api/(queue|modes|services)/*` (returned fake 200s),
  dashboard stub panels (Services/Queue/Settings)
- ~70 MB of committed binaries, logs, debug payloads, and stale configs

## [0.1.0] - 2025-05-10

### Added
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
- jobs.repo.ts: Fixed unclosed template literal in listStmt (now removed, using inline queries instead)
- jobs.repo.ts: Fixed `:daysDays` parameter typo in cleanupOldStmt
- jobs.repo.ts: Fixed reprioritize() and delete() endpoints to use parameterized queries
- models.routes.ts: Fixed /models/load to use `/api/show` instead of `/api/pull`
- models.routes.ts: Fixed /models/unload to use `/api/pull` with `keep_alive: "0"` instead of `/api/generate`
- PolicyEngine.ts: Fixed `linked` to `locked` in canExecuteGpuJob return value
- apiTypes.ts: Removed `"noop_test"` and `| string` from JobType union — now fully typed
- jobTypes.ts: Removed `| string` from ClientOrigin union — now fully typed
- PolicyEngine.ts: Fixed `_mode` private field with proper getter/setter for mode property

### Changed
- `.env.example`: Fixed `Ollama_HOST` to `OLLAMA_HOST` (consistent casing)
- schema.ts: Changed `publicBaseUrl` default from `http://ai.homelab.com:8721` to `http://localhost:8721`
- schema.ts: Changed `executable` default from Windows path to `ollama` (cross-platform via PATH)
- schema.ts: Changed `hardware.provider` default from `"null"` to `"none"`
- gateway.example.yaml: Applied same defaults as schema.ts
- App.tsx: Removed unused `react-router-dom` imports (Link, Routes, Route, useLocation)
- dashboard package.json: Removed unused `react-router-dom` dependency
- gateway-service package.json: Removed 9 unused dependencies (@anthropic-ai/sdk, @fastify/cookie, @fastify/rate-limit, @fastify/swagger, @fastify/swagger-ui, http-proxy-middleware, winston, winston-daily-rotate-file, dotenv)
- Deleted dead files: containers/.gitkeep, hooks/.gitkeep, pages/JobsList.tsx, pages/Dashboard.tsx
- LoadConfig: Added R9700_API_KEY_ENV environment override support
- Dashboard UI: Added footer, better error states, empty states, and cross-browser compatibility
- CHANGELOG: Updated from `[Unreleased]` to versioned `[1.0.0]`

### Removed
- Dead placeholder components from App.tsx (JobsList, ModelsCard, ServicesCard, ModesCard stubs)
- Dead dashboard pages (JobsList.tsx, Dashboard.tsx)
- Unused Fastify dependencies from gateway-service package
