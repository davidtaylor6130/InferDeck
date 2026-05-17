# Issue Tracker

> **Internal document** — move to a private tracker before going public.
> The following items were discovered through a deep code review of the InferDeck codebase. Each entry includes the file(s) and lines involved, a short description of the root cause, and suggested fixes.
>
> **Resolved by cleanup-sweep (PR #12):**
> - #1 Sidebar icons → fixed (inline SVGs)
> - #4 README Ollama refs → fixed (llama.cpp)
> - #6 Hardcoded URLs → fixed (window.location.origin)
> - #10 Version/name alignment → fixed (0.1.0, inferdeck)

## 1. Sidebar navigation items render as empty squares
- **File:** `apps/dashboard/src/components/Sidebar.tsx:62`
- **Code:** `<span className="h-5 w-5 shrink-0 rounded border border-current" />`
- **Fix:** Add real icons (inline SVG or an icon library) for each nav item.

## 2. Hardware telemetry defaults to "none" – never shows data
- **File:** `config/gateway.example.yaml:51` and `apps/gateway-service/src/services/HardwareTelemetry.ts:61-64`
- **Root cause:** Only `amd_adlx` is supported; default provider is `none`.
- **Fix:** Implement auto‑detect or support for other providers (nvidia, intel, macos) and allow `auto`.

## 3. Gaming / Maintenance modes do not pause or reject jobs
- **Files:** `apps/gateway-service/src/http/routes/modes.routes.ts`, `apps/gateway-service/src/services/WorkloadCoordinator.ts`, `apps/gateway-service/src/app.ts`
- **Root cause:** Mode changes affect `scheduler` and `lockManager`, but `WorkloadCoordinator.acquire()` never consults them.
- **Fix:** Call `ctx.scheduler.shouldRunImmediate()` or a dedicated policy engine in `acquire()` and enforce pause/resume on the queue.

~~## 4. README references a non‑existent `packages/backend-ollama`~~
- **File:** `README.md:20` and `apps/gateway-service/src/config/schema.ts:33`
- **Root cause:** Only `packages/backend-llama` exists; no Ollama support.
- **Fix:** Either add an Ollama backend or remove the reference from the README and schema.

## 5. No UI to configure context window or model settings
- **Files:** `apps/dashboard/src/pages/SettingsPage.tsx`, `apps/dashboard/src/components/ModeSwitcher.tsx`
- **Root cause:** Settings page only displays read‑only values.
- **Fix:** Add editable controls for `ctxSize`, `maxGpuLayers`, bind host/port, GGUF directory, model selector, and managed mode toggle.

~~## 6. Dashboard URLs hardcoded to `ai.homelab.com`~~
- **File:** `apps/dashboard/src/utils.ts:3-6`
- **Root cause:** Hardcoded host breaks on other machines.
- **Fix:** Dynamically compute URLs from the gateway config or use `window.location.origin`.

## 7. Hardware events are emitted but the dashboard never consumes them
- **Files:** `apps/gateway-service/src/services/HardwareTelemetry.ts:80`, `apps/gateway-service/src/http/routes/events.routes.ts`, `apps/dashboard/src/App.tsx`
- **Root cause:** No `hardwareData` field in `DashboardState`; SSE events trigger full `refreshAll()`.
- **Fix:** Add `hardwareData` to state and update it on `hardware:update` events.

## 8. `PolicyEngine` is never wired into the app
- **File:** `packages/gateway-core/src/PolicyEngine.ts`
- **Root cause:** The engine is not instantiated; `Scheduler` contains duplicate logic.
- **Fix:** Create a `PolicyEngine` instance in `AppContext` and use it in `WorkloadCoordinator.acquire()`.

## 9. `/queue/resume` does not update DB when queue is paused
- **File:** `apps/gateway-service/src/http/routes/queue.routes.ts:29`
- **Root cause:** Calls `getQueued()` which returns nothing when items are paused.
- **Fix:** Update DB rows from `ctx.queueStore.getPaused()`.

~~## 10. Inconsistent provider defaults (`"none"` vs `"null"`)~~
- **Files:** `config/gateway.example.yaml:51`, `apps/gateway-service/src/config/schema.ts:62`
- **Root cause:** Schema defaults to the string "null" while example uses "none".
- **Fix:** Use an explicit enum for provider values.
