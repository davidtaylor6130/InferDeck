# InferDeck v2 — Cleanup, Hardening & Dashboard Redesign Report

## Context

InferDeck is a Windows-only local AI gateway: one `.exe` that runs LLMs in-process via llama.cpp (Vulkan), exposes an OpenAI-compatible API on `:11434/v1/`, and serves a React dashboard at `:11434/`. The long-term goal is one GPU shared across multiple backends (LLM, TTS, STT, image gen) with InferDeck owning queuing and arbitration; the current focus is stability of the llama.cpp backend.

A v1→v2 restructuring left the repo carrying **two complete gateways**: the dead v1 (`apps/gateway-service` + `libs/core` + `libs/backends` + `libs/vector_store` + legacy wrapper files) is still compiled, installed, and tested alongside the live v2. Additionally, four v2 layers (`scheduler`, `engine`, `sampling`, `messaging`) were built speculatively and are **never called** by the live request path. ~70 MB of build artifacts, logs, and debug files are committed to git.

**Owner decisions (confirmed 2026-06-11):**
1. Delete ALL of v1, including the `libs/backends` TTS/STT/SD skeletons (git history preserves them; future backends will be designed against v2's `IModel`).
2. Delete the four unused v2 layers (`scheduler`, `engine`, `sampling`, `messaging`). Rebuild queuing later against the real `BackendCoordinator`.
3. **Full dashboard redesign** around the actual v2 feature set, with a real SSE events endpoint.

This report is written for a local Qwen3.6 implementing agent. Phases are ordered by safety: each phase ends with a build+test gate. **Do not start a phase until the previous phase's gate passes.** Severity tags: 🔴 critical, 🟠 high, 🟡 medium, ⚪ low.

---

## A. Current state map (verified)

### Live v2 path (KEEP — this is the product)

| Component | Files | Notes |
|---|---|---|
| `apps/inferdeck-gateway/src/main.cpp` | 726 lines | Entry point. Also contains ALL dashboard `/api/*` routes inline (lines ~590–754) — refactor target |
| `libs/gateway/` | `routes.cpp` (698 ln), `metrics_builder.cpp`, `streaming_sanitizer.cpp`, headers | OpenAI routes + SSE streaming. `routes.cpp` has uncommitted changes |
| `libs/model/` | `backend_coordinator.cpp` (274 ln), `model_registry`, `imodel.hpp` | Swap serialization, slot acquisition |
| `libs/llama_cpp_wrapper/src/llama_cpp_model.cpp` | 1416 lines | The live llama.cpp wrapper. Uncommitted changes |
| `libs/foundation/` | logging, Error, JSON | |
| `libs/observability/` | `gpu_telemetry.cpp`, `stats_db.cpp` (SQLite), `metrics.cpp`, `logger.cpp` | |
| `libs/optimize/` + `apps/benchmark-runner/` | ~13 KB + 7 KB src | inferdeck-bench harness (P9). Separate exe, keep |
| `apps/model-tester/` | links only `model`+`foundation` | v2 swap exerciser dev tool, keep |
| `apps/dashboard/` | React 19 + Vite + Tailwind, builds into `apps/inferdeck-gateway/static/` | Full redesign in Phase 6 |
| `apps/hardware-adlx-helper/` | optional GPU telemetry helper | Keep (referenced by observability config `adlx_helper`) |
| `libs/third_party/llama.cpp` | vendored | Keep |
| `tests/integration/`, `tests/parity/`, `tests/fixtures/` | wired via `INFERDECK_BUILD_TESTS` | Keep |
| `config/gateway.yml`, `config/sampler-profiles/`, `config/bench-search-spaces.yaml` | | Keep. `config/gateway.test-ralph.yml` is the mini-ralph harness config — keep |
| `Testing/` (untracked) | `mini-ralph.mjs`, `overflow-test.mjs` | Active local test harness — keep, never delete |
| `scripts/`, `ops/`, `service/`, `runtime/llama-b9276-vulkan/`, `MCP/` | | Keep |
| Root: `package.json`, `pnpm-workspace.yaml`, `pnpm-lock.yaml`, `tsconfig.base.json` | dashboard tsconfig extends `tsconfig.base.json` — keep it | |

### Dead code (DELETE)

Verified: **no file in the live v2 path includes anything from these.** `inferdeck-gateway` links only `gateway, model, scheduler, engine, foundation, llama_cpp_wrapper, llama` (apps/inferdeck-gateway/CMakeLists.txt:10-20), and grep confirms zero `#include` of `core/`, `llama_cpp/LlamaEngine.hpp`, `sampling/`, or `messaging/` anywhere under the v2 dirs.

| Path | What it is | Evidence dead |
|---|---|---|
| `apps/gateway-service/` (~5,700 LOC C++ + 99 MB incl. dist/) | v1 gateway | Only consumer of libs/core, libs/backends, LlamaEngine |
| `libs/core/` (~730 LOC) | v1 logger/config/jobqueue | Only included by v1 + LlamaEngine.cpp |
| `libs/backends/` (~1,370 LOC) | v1 backend registry + TTS/STT/SD skeletons (mostly commented stubs) | Only used by gateway-service |
| `libs/vector_store/` | v1 doc store | Only used by gateway-service |
| `libs/llama_cpp_wrapper/src/LlamaEngine.cpp` (28.7 KB), `LlamaServerManager.cpp`, `GGUFParser.cpp`, `VulkanDevice.cpp` + their headers in `include/llama_cpp/` | v1 engine (contains the 4 documented legacy bugs) | Only included by gateway-service + libs/tests. Note: `GGUFParser.cpp`/`VulkanDevice.cpp` aren't even in the CMake target — already orphaned |
| `libs/tests/` (~30 files) | v1 Catch2 test suite (tests LlamaEngine, backends, vector store, v1 routes) | Tests only v1 code |
| `libs/scheduler/`, `libs/engine/`, `libs/sampling/`, `libs/messaging/` | Speculative v2 layers, compiled but bypassed | `deps.scheduler` constructed in main.cpp:520 but never referenced in routes.cpp; slot acquisition goes straight to `coordinator.acquire_slot()`; sampler chain is hand-rolled in llama_cpp_model.cpp |
| `packages/backend-llama|backend-ollama|gateway-core|shared` | v1 Node packages — `git ls-files packages/` is EMPTY (removed in commit f5dde6f), only stale `dist/`+`node_modules/` remain on disk | Not in pnpm workspace |
| `cypress/` + `cypress.config.ts` | E2E tests targeting `https://localhost:8080` (v1 dashboard port) | v2 serves on :11434 |
| `libs/gateway/src/streaming_sanitizer.cpp.bak` (13.6 KB) | stale backup inside live lib | `.bak` |

### Tracked clutter (DELETE from git + disk, then .gitignore)

- Root binaries: ~25 `llama-*.exe`, ~18 `ggml-*.dll`, `llama.dll`, `llama-common.dll`, `mtmd.dll`, `rpc-server.exe` (~47 MB, duplicated in `runtime/llama-b9276-vulkan/`)
- Debug leftovers: `test_debug.cpp`, `test_fix.ps1`, `compile_debug.bat`, `test_coder.json`, `test_moe.json`, `test_prompt.json`, `test_prompt_gpt.json`, `test_request.json`, `test_request2.json`, `test_stream.json`, `query` (18-byte mystery file)
- Logs committed to git: `stderr.txt` (333 KB), `stdout.txt`, `gateway.err` (305 KB), `gateway.log`, `admin-rebuild.log`, entire `logs/` dir (~1.6 MB incl. `llama-server.*.log`), `run-logs/` dir
- Misc: `session-ses_19ef.md` (Claude transcript), `ChatGPT Image May 25, 2026 at 09_56_22 AM.png` (1.3 MB), `opencode.json.bak`, empty `rocblas/` + `hipblaslt/` dirs (ROCm leftovers; project uses Vulkan)
- `vitest.config.ts` (root): its include pattern is `apps/gateway-service/src/**/*.test.{ts,tsx}` — points at v1 paths that have no tests. Replace contents to target `apps/dashboard` (which has `OverviewPage.test.tsx`) or delete and give the dashboard its own config.
- Stale docs: `docs/opencode-inferdeck-v1.recommended.json`, `docs/opencode-inferdeck-ollama.experimental.json`, `start_service.ps1` (v1 startup)

---

## Phase 0 — Repo hygiene (no code changes, zero build risk)

1. `git rm` everything in the "Tracked clutter" table above. For untracked-on-disk leftovers (`packages/*/dist`, `packages/*/node_modules`, `rocblas/`, `hipblaslt/`), plain delete.
2. Append to `.gitignore`:
   ```
   logs/
   run-logs/
   *.log
   *.err
   stderr.txt
   stdout.txt
   session-*.md
   test_*.json
   *.bak
   data/*.sqlite
   data/logs/
   ```
   (Root `*.exe`/`*.dll` are already ignored but were force-added historically — the `git rm` in step 1 fixes that. `runtime/` binaries stay tracked deliberately: deployment bundle.)
3. Do NOT touch: `Testing/` (active mini-ralph harness, untracked by design), `config/gateway.test-ralph.yml`, `data/pricing.json`, `models/`.

**Gate:** `git status` clean of clutter; `cmake --build build --config Release -j` still succeeds (nothing in this phase is compiled).

## Phase 1 — Delete the v1 stack

Order matters: edit CMake first so each deletion is verifiable.

1. **Root `CMakeLists.txt`:**
   - Delete lines 108, 110–112: `add_subdirectory` of `libs/core`, `libs/backends`, `libs/vector_store`, `apps/gateway-service`. Keep line 109 (`libs/llama_cpp_wrapper`) and 113 (`apps/model-tester` — it is a v2 tool).
   - Replace the install block (lines 119–125): install `inferdeck-gateway` instead of `gateway-service`, and **remove `install(DIRECTORY certs/ ...)` — `certs/` does not exist**, the rule is broken today. Keep `install(DIRECTORY config/ ...)`.
   - Delete line 116's `add_subdirectory(libs/tests)` (v1 test suite).
2. **`libs/llama_cpp_wrapper/CMakeLists.txt`:**
   - Remove `src/LlamaEngine.cpp` and `src/LlamaServerManager.cpp` from the target (line 10–11), leaving only `src/llama_cpp_model.cpp`.
   - Remove `${CMAKE_SOURCE_DIR}/libs/core/include` from include dirs (line 22).
   - Try removing `psapi dxgi` (line 42) — they were for VulkanDevice/LlamaEngine; if `llama_cpp_model.cpp` or observability fails to link, restore only what's needed (observability's gpu_telemetry uses DXGI — that lib has its own CMakeLists).
3. **Delete directories/files:** `apps/gateway-service/`, `libs/core/`, `libs/backends/`, `libs/vector_store/`, `libs/tests/`, `packages/`, `cypress/`, `cypress.config.ts`, and from `libs/llama_cpp_wrapper`: `src/LlamaEngine.cpp`, `src/LlamaServerManager.cpp`, `src/GGUFParser.cpp`, `src/VulkanDevice.cpp`, plus matching headers under `include/llama_cpp/` (`LlamaEngine.hpp`, `LlamaServerManager.hpp`, `GGUFParser.hpp`, `VulkanDevice.hpp`). Keep `include/llama_cpp_wrapper/llama_cpp_model.hpp`.
4. Delete `start_service.ps1`, `docs/opencode-inferdeck-v1.recommended.json`, `docs/opencode-inferdeck-ollama.experimental.json`. Audit `scripts/build.sh` and `service/run-gateway-service.ps1` for `gateway-service` references; update to `inferdeck-gateway` or delete the script if it's v1-only.

**Gate:** full reconfigure from scratch (`cmake -S . -B build-clean ...`) + build succeeds; `inferdeck-gateway.exe` starts, serves `/v1/models`, completes one chat completion (use `Testing/mini-ralph.mjs` or a curl from AGENTS.md §"Test a swap manually").

## Phase 2 — Delete the bypassed v2 layers

1. Root `CMakeLists.txt`: remove `add_subdirectory` for `libs/messaging`, `libs/sampling`, `libs/engine`, `libs/scheduler` (lines 94–95, 97–98).
2. `apps/inferdeck-gateway/CMakeLists.txt:10-20`: remove `scheduler` and `engine` from `target_link_libraries`.
3. `apps/inferdeck-gateway/src/main.cpp`: remove the `Scheduler` construction (~line 520) and its include; remove `scheduler` member from `GatewayDeps` (in `libs/gateway/include/gateway/routes.hpp`) and any other dangling references (compiler will find them).
4. Check `libs/gateway/CMakeLists.txt` and `libs/model/CMakeLists.txt` for links to the four layers; remove.
5. Delete `libs/scheduler/`, `libs/engine/`, `libs/sampling/`, `libs/messaging/`.
6. Record intent for the future in AGENTS.md (Phase 7): queuing/LCP slot-matching will be rebuilt against `BackendCoordinator` when multi-backend lands; the deleted layers exist at git tag/commit for reference.

**Gate:** clean rebuild + run `ctest --test-dir build -LE e2e` + one streaming chat completion with tool calls (mini-ralph).

## Phase 3 — Correctness fixes in the live path

Each item: file:line, problem, required fix. These were found by line-by-line review including the uncommitted diffs on `routes.cpp`, `llama_cpp_model.cpp`, `backend_coordinator.cpp`. **Commit the current uncommitted work first** (it's mostly sound: SSE heartbeat, friendlier errors, fallback tool-call streaming, debug-log removal) so these fixes are separate commits.

### 3.1 🔴 SSE error handling sets HTTP status after headers are committed
`libs/gateway/src/routes.cpp:700-711`. In the streaming path, errors discovered after streaming starts attempt `finish_once(false, 400/500, ...)` — but httplib has already sent `200 OK` with the first chunk; the status can't change. Fix:
- Validate everything possible (model exists/loaded, body parse, max_tokens sanity, context length precheck) **before** calling `set_chunked_content_provider`, returning proper JSON errors with real status codes.
- For genuinely mid-stream failures, standardize on: emit one `data: {"error": {...}}` SSE event, then `data: [DONE]`, then close. Document this in `docs/api/chat_completions.md` (it's also what OpenAI-compatible clients expect).

### 3.2 🟠 `max_tokens` default is inconsistent in three places
`routes.cpp:213` now defaults to `-1` (= use full context budget, per `llama_cpp_model.cpp:1193`), but `libs/model/include/model/imodel.hpp:59` still has struct default `512`, and the old behavior was `256`. Fix: make `-1` the single default — change `imodel.hpp:59` to `-1`, keep `routes.cpp:213` as-is, and add a comment-free named constant `k_max_tokens_use_context_budget = -1` in `imodel.hpp` used by both sites.

### 3.3 🟠 Error classification by substring matching
`routes.cpp:445-446` and `routes.cpp:696-697` decide `context_length_exceeded` vs `invalid_request_error` by `message.find("maximum context length")`. This breaks the moment the message text in `llama_cpp_model.cpp:1171` changes (it already changed in the uncommitted diff). Fix:
- Add an `enum class ErrorCode { internal, invalid_request, context_length_exceeded, model_not_loaded, ... }` field to the Error type in `libs/foundation` (find it via `foundation::Error` / `std::expected` usage).
- Set it at the throw/return site in `llama_cpp_model.cpp` (:1168-1173, :1349-1351).
- Map enum→(HTTP status, OpenAI `type` string, `code` string) in ONE helper in `routes.cpp`; call it from both the stream and non-stream paths (currently duplicated at :441-448 and :694-699).

### 3.4 🟠 Non-OpenAI error response shape
`routes.cpp:244-249` and `main.cpp:566-567` return `{"error":{"code","message"}}`. OpenAI clients expect `{"error":{"message","type","param","code"}}`. The uncommitted diff adds `type` in some paths but not all. Fix: one `make_error_json(ErrorCode, message)` helper (natural companion to 3.3), used by every error return in `routes.cpp` AND the `/v1/*` handlers in `main.cpp`.

### 3.5 🟡 Fallback tool calls can be emitted twice
`llama_cpp_model.cpp:1487-1511` (uncommitted diff): tool calls are appended to `out.tool_calls` at :1487, then re-sent as deltas in the `fallback_tool_calls_used` loop at :1498-1510. If the parser already streamed some tool-call deltas before falling back, the client receives duplicates. Fix: only emit fallback deltas for tool calls not already streamed (track which indices the parser emitted), or suppress the fallback entirely when the parser emitted ≥1 tool-call delta.

### 3.6 🟡 Invalid UTF-8 can crash/garble SSE chunks
llama.cpp tokens can split multi-byte UTF-8 sequences; a piece ending mid-sequence makes `nlohmann::json::dump()` throw `type_error.316` (default error handler), which in the streaming thread context kills the stream. Audit every `json{...}.dump()` on token text in `routes.cpp` (delta construction, ~:660-690) and `llama_cpp_model.cpp`. Fix (both):
- Buffer incomplete trailing UTF-8 bytes in the streaming pipeline (hold back 1–3 bytes until the continuation arrives — `streaming_sanitizer` is the right home), and
- Defensively use `dump(-1, ' ', false, nlohmann::json::error_handler_t::replace)` for delta serialization so a bug degrades to U+FFFD instead of a dead stream.

### 3.7 🟡 Inference thread lifecycle is fragile
`routes.cpp:533-577` (`finish_once`) has a self-join check at :541 that can never be true from the content-provider thread, and detaches the inference thread (:542) as a fallback. A detached thread that touches `StreamState` after the provider returns is a use-after-free risk if the shared_ptr discipline ever slips. Fix: ensure `StreamState` is held by `shared_ptr` captured by BOTH the inference thread and the provider (verify this is already true — if so, replace the dead self-join branch with a plain `if (t.joinable()) t.detach()` and document ownership in the header); prefer `std::jthread` + `stop_token` for the abort path.

### 3.8 🟡 Heartbeat loop: verify write-failure path
`routes.cpp:637-648` (uncommitted): the `cv.wait_for(2s, pred)` heartbeat pattern is correct (predicate re-checked under lock — no lost wakeup), but when `sink.write(": \n\n")` fails the code must set `aborted` and signal the inference thread so generation stops promptly; confirm this happens (and add it if the failure branch only breaks the loop).

### 3.9 🟡 Swap/coordinator: confirm slot-drain vs request-record ordering
`backend_coordinator.cpp` (274 ln) serializes swaps (commit c635e9d). Verify: (a) `acquire_slot` failure during an in-progress swap returns a typed error mapped to 503 + `model_swapping` hint per AGENTS.md's no-auto-swap policy — note `config/gateway.yml` has `auto_swap: true`, contradicting AGENTS.md's "NO auto-swap" rule; **ask the owner which is intended, then make code, config, and AGENTS.md agree**; (b) unload waits for `active_request_count == 0` and a slot released mid-swap can't be re-acquired.

### 3.10 ⚪ Hardcoded sampler magic numbers
`llama_cpp_model.cpp:1097-1110`: `dry_multiplier=0.8f`, `dry_penalty_last_n=1024`, `min_p=0.05f` etc. hardcoded. `config/sampler-profiles/` exists for exactly this. Fix: read from the model's sampler profile with these values as named-constant defaults (`k_default_dry_multiplier`...).

### 3.11 ⚪ Hardcoded Vulkan SDK path
Root `CMakeLists.txt:50-66` hardcodes `C:/VulkanSDK/1.4.309.0`. Fix: `if(DEFINED ENV{VULKAN_SDK})` use it, else fall back to the current literal; keeps the build working on this machine, unbreaks others.

## Phase 4 — Refactor for readability/modularity (no behavior change)

1. **Split `handle_chat_completions` (routes.cpp:340-748, ~408 lines)** into: `parse_chat_request(body) -> expected<InferenceRequest, ErrorCode>`, `resolve_model_or_503(...)`, `run_non_streaming(...)`, `run_streaming(...)` (owns StreamState + thread), plus the shared `make_error_json` from 3.4. Target: no function over ~80 lines.
2. **Split `predict_stream` (llama_cpp_model.cpp:1294-1516, ~222 lines)** into: prompt build/tokenize, sampler setup (RAII wrapper for `common_sampler*` — fixes the manual-free confusion at :1410-1421), decode loop, and tool-call finalization. Share prompt/sampler setup with the non-streaming `predict` (the two are near-duplicates today).
3. **Move dashboard `/api/*` routes out of `main.cpp`** (lines ~590-754) into `libs/gateway/src/dashboard_routes.cpp` with its own `register_dashboard_routes(server, deps)`. `main.cpp` should be: config load, deps construction, route registration, signal handling, run. Target ≤250 lines.
4. **Observability facade:** routes currently call `record_request()` juggling raw `Metrics*` + `StatsDb*` pointers (routes.cpp:37-74). Introduce one `RequestRecorder` (in `libs/observability`) with `record_request(...)` / `record_swap(...)`; pass that in `GatewayDeps` instead of two raw pointers.
5. Delete `libs/gateway/src/streaming_sanitizer.cpp.bak`.

**Gate:** clean build, `ctest -LE e2e`, mini-ralph streaming+tool-call run, diff of `/v1/models`, one non-stream and one stream completion response against pre-refactor captures (byte-identical apart from ids/timestamps).

## Phase 5 — Backend API consolidation + real SSE events

Prereq for the dashboard redesign. Current dashboard API issues (all in `main.cpp`):

1. **Implement `GET /api/events/stream`** (currently a 204 stub at main.cpp:732-740) as a real SSE endpoint emitting named events:
   - `stats` every ~1s: `{gpu: {util, vram_used_mb, vram_total_mb, temp_c, power_w}, tps_current, active_requests, queued}` (source: `GpuTelemetry::latest()` + `Metrics`)
   - `model`: on load/unload/swap-start/swap-progress/swap-complete/swap-failed `{state, model, from, to, duration_ms, error}`
   - `request`: on completion `{model, prompt_tokens, completion_tokens, tps, status, duration_ms}`
   Implementation: a small `EventBus` (mutex + per-subscriber queue + cv) in `libs/observability` or `libs/gateway`; `BackendCoordinator` and `record_request` publish; the SSE handler subscribes via httplib chunked provider, reusing the heartbeat pattern from routes.cpp:637. Drop slow consumers (bounded queue) rather than blocking publishers.
2. **Make swap non-blocking with progress.** `/v1/swap/to/:name` (routes.cpp:594 registration) currently blocks until done. Return `202 {"status":"swapping"}` immediately, run swap on the coordinator's existing serialized path, publish `model` events for progress. Wire `request_swap_cancel()` (exists per AGENTS.md §When Stuck #7) to `POST /v1/swap/cancel`.
3. **Delete the no-op stubs**: `POST /api/(queue|modes|services)/...` (main.cpp:707-711) returns fake `{"ok":true}` — remove entirely; a UI button that does nothing is worse than no button.
4. **Trim `/api/services`** (main.cpp:697): return only real services (gateway, llama-cpp in-process, telemetry). Remove fabricated Whisper/Training/TTS/Image entries.
5. **Unify the duplicate surfaces:** `/api/health` vs `/v1/health`, `/api/models` vs `/v1/models` overlap. Keep `/v1/*` as the OpenAI-ish surface, keep `/api/*` only for dashboard-specific shapes (`/api/status`, `/api/jobs`, `/api/logs`, `/api/events/stream`), and have dashboard fetch models from `/v1/models`. Delete `/api/health`, `/api/models`, `/api/models/running` after the dashboard migrates; `/api/models/load|unload` become thin wrappers over the swap path (or move to `/v1/models/load`— pick one, document in docs/api/).
6. Add `p50/p95` latency and per-model tps to `/api/status` summary (computable from `StatsDb::recent_requests`).

**Gate:** `curl -N http://localhost:11434/api/events/stream` shows live `stats` events; swap via curl returns 202 and events show progress; integration tests updated and green.

## Phase 6 — Dashboard full redesign

Rebuild `apps/dashboard/src` around what v2 actually does. Keep the stack (React 19, Vite, Tailwind, TanStack Query — all current) and the build pipeline (`pnpm build:dashboard` → `apps/inferdeck-gateway/static/`, copied post-build per apps/inferdeck-gateway/CMakeLists.txt:43-49; built assets stay committed).

**Information architecture — 4 pages replace the current 7** (drop ServicesPage, QueuePage-as-is, empty SettingsPage):

1. **Overview** (default): hero card = current model + state (loaded/swapping w/ progress bar + cancel button/error); live charts (60s window, fed by SSE): tps, GPU util, VRAM, temp; counters: lifetime tokens in/out, requests, uptime; recent activity feed from `request` events.
2. **Models**: list from `/v1/models` (name, family, ctx size, vram_required_mb, vision badge, loaded state); per-row "Load" → swap with inline progress; swap history table (from `/v1/stats/history` swap rows).
3. **Usage & Cost**: the existing cost UI is the best part of the current dashboard — port it (per-model token usage from `/api/status` tokenUsage/monthlyTokenUsage, localStorage price overrides, break-even tracking) but: prices keyed by model name with the `MODEL_COST_DEFAULTS` table (OverviewPage.tsx:43-107) moved to a fetched `data/pricing.json` (file already exists in repo) so defaults aren't baked into the bundle; add p50/p95 latency + per-model tps once Phase 5.6 lands.
4. **System**: hardware telemetry detail (current HardwarePage content), log viewer (current LogsPage, `/api/logs`), gateway config display (read-only dump of active gateway.yml subset — add tiny `/api/config` endpoint if desired, optional).

**Cross-cutting requirements:**
- **SSE-first state**: one `EventSource('/api/events/stream')` in a context provider; TanStack Query for on-demand fetches; keep a slow 30s polling fallback ONLY when EventSource errors (replaces the 4s `setInterval` at App.tsx:127).
- **Connection state machine**: connected / reconnecting (EventSource auto-retry, show banner + stale-data timestamp) / offline. Fixes the current "stale numbers shown forever" bug.
- Remove hardcoded `originWithPort('11434')` (utils.ts:16) — use `window.location.origin` since the dashboard is served by the gateway itself; keep a `VITE_API_BASE` env override for `pnpm dev` (the dev proxy in vite.config.ts already handles this).
- Remove hardcoded GPU name "Radeon AI PRO R9700" (ModelTable.tsx:44) — take from telemetry payload.
- Keep `OverviewPage.test.tsx`-style vitest tests for the new pages; fix root `vitest.config.ts` to point at `apps/dashboard` (Phase 0 note).
- After redesign: rebuild, commit new `apps/inferdeck-gateway/static/`, delete now-unused components (Sidebar entries, QueueTable, ServicesPage, etc.).

**Gate:** `pnpm --filter dashboard test` green; manual run: load dashboard, watch live stats tick without polling (Network tab shows one EventSource), trigger swap from Models page and watch progress, kill gateway → offline banner → restart → auto-recover.

## Phase 7 — Documentation truth pass

AGENTS.md is the onboarding doc for the user's agents and is now materially wrong. Update:
- Project structure section (lines 18-47): remove scheduler/engine/sampling/messaging/core layers, gateway-service, packages; reflect actual layout.
- Architecture quick reference (139-159): rewrite request flow as it actually is (routes → coordinator → llama_cpp_model); remove WebSocket claims (lines 159, 266-267, 325-326 — there has never been a WebSocket; it's SSE + polling).
- Critical-bugs table (161-173): rewrite — the referenced legacy files no longer exist; keep the lessons ("LCP not message count", "use common_sampler_init", "cursor-based sanitizer", "llama_vocab_get_add_bos") as design rules instead of file references.
- Resolve the `auto_swap` contradiction per the Phase 3.9 decision.
- `docs/architecture.md`, `docs/README.md`, `README.md`: same truth pass. Delete or mark-legacy any doc describing v1 (`docs/jobs.md` etc. — verify each).
- Update CHANGELOG.MD with the v1 removal.

---

## Verification matrix (run after every phase)

| Check | Command |
|---|---|
| Clean configure+build | `cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ...` then `cmake --build build --config Release -j` |
| Unit+integration | `ctest --test-dir build --output-on-failure -LE e2e` (configure with `-DINFERDECK_BUILD_TESTS=ON`) |
| Smoke: serve+complete | start exe; `curl :11434/v1/models`; one non-stream + one `stream:true` chat completion |
| Tool-call streaming | `node Testing/mini-ralph.mjs` (uses `config/gateway.test-ralph.yml`) |
| Swap | `curl -X POST :11434/v1/swap/to/<other-model>` + status poll |
| Dashboard | build, open `:11434/`, exercise per Phase 6 gate |

## Guardrails for the implementing agent

- One phase per branch/PR; never mix deletion phases with behavior-change phases.
- Hard rules from AGENTS.md remain: **no subprocess/llama-server.exe, everything in-process; no orphan processes; C++23; `std::expected` over exceptions; no comments unless asked.**
- When deleting, if the build breaks on a reference this report missed: do NOT restore the whole file — extract only the needed symbol into the live lib and note it.
- The uncommitted changes on `routes.cpp` / `llama_cpp_model.cpp` / `backend_coordinator.cpp` are wanted work — commit them as-is before Phase 3, do not revert.
- Never delete `Testing/`, `config/gateway.test-ralph.yml`, `data/`, `models/`, or anything under `runtime/`.
