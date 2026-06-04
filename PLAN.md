# InferDeck 2.0 — Multimodel Gateway Plan

**Branch:** `inferdeck-2.0-multimodel`
**Date:** 2026-06-04
**Status:** P0 in progress

## 1. Mission

Replace the current InferDeck (which underperforms raw `llama-server.exe` and contains
4 critical bugs) with a clean, layered, in-process C++23 gateway that supports
**multiple LLMs with hot-swap**, achieves **0.95 behavioural parity** with
`llama-server.exe` for each registered model, and provides **observability +
automated sampler tuning** so the best local coding experience improves over time.

### Hard constraints

- **In-process only**: single `.exe`, no subprocess, no orphan processes
- **llama.cpp backend**: Vulkan on R9700 32GB, ROCm fallback documented
- **Vision required**: mmproj stays loaded for GUI review workflow
- **Multi-model from day 1**: switchable via OAI `model:` parameter
- **Single user, local**: no multi-tenancy, no auth

### Target use case

Opencode sub-agents (parallel coding) on Qwen3-Coder-Next (specialist, fast) by
default, with Qwen3.6-27B (general, vision) available on demand for GUI reviews
and tasks requiring vision.

## 2. Locked Decisions

| Item | Value |
|---|---|
| Models | `qwen3.6-27b` (default, vision) + `qwen3-coder-next` (specialist, no vision) |
| Quant | Q4_K_M (Unsloth Dynamic GGUFs) |
| Context | 64K x 2 slots, KV cache Q8_0 |
| Concurrency | n_parallel=2 per loaded model |
| Scheduler | LCP-match first, free slot second, 30s queue |
| Backend | llama.cpp Vulkan |
| MTP | Off (conflicts with n_parallel>1) |
| Swap trigger | Dashboard click only — no auto-swap on OAI request |
| GGUF cache | None — read from disk each swap (~15-20s) |
| Optimization | Per-model Optuna TPE, 30 trials, manual confirm apply |
| Parity | 0.95 token sequence match, CI gate per registered model |
| Language | C++23, asio awaitables, std::variant messaging |

## 3. Architecture (9 Layers + Multimodel)

```
Layer 0: llama.cpp (libs/third_party/llama.cpp, Vulkan build)
Layer 1: Foundation (libs/foundation/) — asio, logging, JSON
Layer 2: Messaging (libs/messaging/) — std::variant content, role enum
Layer 3: Sampling (libs/sampling/) — common_sampler_init, family defaults
Layer 4: Model Registry + BackendCoordinator (libs/model/) — multimodel core
Layer 5: Scheduler (libs/scheduler/) — LCP-match, model-aware queue
Layer 6: Engine + Per-Model Slot Pool (libs/engine/) — 2 slots per model
Layer 7: Observability (libs/observability/) — ADLX, EMA stats, SQLite
Layer 8: HTTP routes (apps/gateway-service/) — OAI, swap, dashboard WS
Layer 9: Benchmarking (apps/benchmark-runner/) — Optuna + parity
```

## 4. Current Codebase Audit (4 Bugs to Fix)

| # | File:line | Bug | Fix in phase |
|---|---|---|---|
| 1 | `libs/llama_cpp_wrapper/src/LlamaEngine.cpp:444-554` | `reuse_cache` uses message count, not LCP | P4 |
| 2 | `libs/llama_cpp_wrapper/src/LlamaEngine.cpp:170-199` | Hardcoded sampler fights Qwen3-A3B | P2 |
| 3 | `apps/gateway-service/src/.../ChatCompletions.cpp:2175-2188` | O(n²) streaming sanitizer | P6 |
| 4 | `libs/llama_cpp_wrapper/src/LlamaEngine.cpp:126` + log | `add_bos=true` hardcoded, log shows `n_ctx_train` not `n_ctx` | P4 |

**Delete in P0:**
- `apps/gateway-service/src/LlamaServerManager.cpp` (stub, never spawns)
- `config/gateway.local.yaml`, `config/gateway.example.yaml` (dead configs)
- `llama-server.exe`, `llama.dll`, `mtmd.dll`, `ggml-*.dll` in repo root
  (already in `libs/third_party/llama.cpp/build/`)
- `hipblaslt/`, `rocblas/`, `rpc-server.exe` (ROCm leftovers, using Vulkan)

## 5. 12-Phase Implementation Plan

### P0 — Foundation (Day 1-2)
- `libs/foundation/`: asio awaitables, structured logging, JSON
- CMake presets: `debug`, `release`, `asan`
- Tooling: clang-format, clang-tidy, include-what-you-use
- **Test:** `hello_future` async test passes
- **Deliverable:** empty `inferdeck-gateway.exe` skeleton that builds + runs

### P1 — Messaging Layer (Day 3-4)
- `libs/messaging/`: `std::variant<TextContent, ImageContent, AudioContent, ToolCallContent, ToolResultContent, ReasoningContent, DeveloperContent>`
- Role enum: `system | developer | user | assistant | tool`
- OAI <-> Anthropic <-> internal conversions
- **Test:** round-trip through all content types, JSON serializer
- **Deliverable:** `libs/messaging/tests/test_messaging.cpp` passes

### P2 — Sampling Layer (Day 5-6)
- `libs/sampling/`: wrap `common_sampler_init` from
  `libs/third_party/llama.cpp/common/sampling.cpp:187`
- `family_defaults.hpp`: Qwen3.6 dense + Qwen3-Coder profiles
- `SamplerProfile` struct matches all `common_sampler` parameters
- **REPLACE** `LlamaEngine.cpp:170-199` `CreateSampler` with `common_sampler_init`
- **Test:** `libs/sampling/tests/test_sampling.cpp` — first 200 tokens
  from InferDeck == raw `llama-server.exe` defaults within 1% on HumanEval
- **Deliverable:** parity test passes for Qwen3.6-27B at temp=0.6/top_p=0.95

### P3 — Model Registry + BackendCoordinator (Day 7-9) **CRITICAL**
- `libs/model/`: `IModel` interface, `ModelEntry` struct, `ModelRegistry`
- `ModelEntry { name, gguf_path, mmproj_path, family, n_slots, vram_required }`
- `BackendCoordinator`:
  - `LoadModel(name)` — reads GGUF, init llama_model, alloc contexts
  - `UnloadModel(name)` — free VRAM
  - `EnsureLoaded(name)` — blocks until ready, triggers swap if needed
  - `AcquireSlot(name)` — returns free slot or queues (30s timeout)
  - `IsLoaded(name)`, `GetLoadedModel()`, `GetVramUsage()`
- Single-threaded swap: mutex during VRAM move, requests queue
- Startup: read `~/.inferdeck/state.json` for last-used, load that model
- **Test:** programmatic swap 27B -> Coder-Next -> 27B in <30s end-to-end
- ADLX telemetry confirms correct VRAM at each state
- **Deliverable:** `apps/model-tester/main.cpp` exercises swap + reports VRAM

### P4 — Engine + Per-Model Slot Pool (Day 10-13)
- `libs/engine/`: per-model 2-slot pool
- Each slot owns: `llama_context`, KV cache, prev_tokens ring buffer, sampler chain
- Implement `server_prompt_cache::load` algorithm at `slot_prompt_similarity`
- KV cache trim via `llama_kv_cache_seq_rm` on prefix match
- **Fix Bug 1** (LCP cache) and **Bug 4** (add_bos, log)
- `add_bos` from `llama_vocab_get_add_bos`, log allocated `n_ctx` not `n_ctx_train`
- **Test:** 2 concurrent opencode requests, 2nd with shared system prompt
  hits slot 0 cache, 2nd divergent prompt lands in slot 1
- **Deliverable:** `libs/engine/tests/test_engine.cpp` — 2 concurrent,
  LCP hit verified via debug log

### P5 — Scheduler (Day 14-15)
- `libs/scheduler/`: LCP-match first, free slot second, queue 30s
- Model-aware: `AcquireSlot(model_name)` delegates to BackendCoordinator
- Streaming multiplexer: per-slot token stream -> SSE wire format
- **Test:** opencode sub-agent run, 2 slots both active, total throughput ~2x single
- **Deliverable:** `libs/scheduler/tests/test_scheduler.cpp` + load test

### P6 — HTTP Routes + Model Selection (Day 16-18)
- `apps/gateway-service/`:
  - OAI `/v1/chat/completions` (POST) — `model:` param honored
  - OAI `/v1/embeddings` (POST) — stub returns 501
  - OAI `/v1/models` (GET) — list with status
  - `POST /v1/swap/to/{model}` — kick off background swap, 202
  - `GET /v1/swap/status` — current state
  - `WebSocket /v1/swap/status` — progress events
  - `GET /v1/health` — liveness
  - `GET /v1/stats` — per-model stats JSON
  - `WebSocket /v1/stats` — live stream to dashboard
- Tool-call extraction via `llama_sampler_init_grammar_lazy_patterns`
  for Qwen (defensive regex fallback stays in `ChatCompletions.cpp`)
- **Fix Bug 3** (O(n²) streaming sanitizer -> cursor-based, scan only new tokens)
- 503 response: `{"error":"model_unloaded","model":"...","swap_url":"...","retry_after":30}`
- **Test:** opencode end-to-end, switch model via OAI `model:`, swap completes
  transparently after user clicks dashboard "Switch"
- **Deliverable:** `apps/gateway-service/tests/integration_test.cpp` —
  real HTTP requests, model switch, 503 handling, tool calls parsed

### P7 — Observability (Day 19-21)
- `libs/observability/`:
  - `InferenceStats` — EMA trackers per model, lifetime counters
  - `GpuTelemetry` interface + `AdlxGpuTelemetry` reusing
    `apps/hardware-adlx-helper/inferdeck_adlx_helper.cpp`
  - `StatsPersister` — SQLite at `~/.inferdeck/stats.db`
  - Swap event log (timestamp, from, to, duration, success/failure)
- WebSocket broadcasts every 100ms
- **Test:** load test, 1000 requests, stats match ground truth, SQLite queryable
- **Deliverable:** `libs/observability/tests/test_observability.cpp`

### P8 — Parity Test Harness (Day 22-23)
- `tests/parity/`:
  - For each registered model, run 50 prompts through raw
    `libs/third_party/llama.cpp/build/bin/llama-server.exe`
  - Same prompts through InferDeck
  - Diff first 200 tokens via `simhash` + Levenshtein on critical tokens
    (function signatures, code blocks, numbers)
  - Pass if similarity >= 0.95
- CI gate: every PR must pass parity for all registered models
- **Test:** parity tests pass for `qwen3.6-27b` and `qwen3-coder-next`
- **Deliverable:** `tests/parity/run.sh` + `tests/parity/expected_outputs/`

### P9 — Optimization Harness (Day 24-26)
- `apps/benchmark-runner/`:
  - Optuna TPE search, 30 trials
  - **Per-model runs**: search space may differ for 27B vs Coder-Next
- Coding suite: HumanEval+ (164), MBPP+ (500), BFCL subset (100)
- Chat suite: MT-Bench subset (80), MMLU subset (500), BBH subset (200)
- LLM-judge: use Qwen3-9B (local, no extra cost) for MT-Bench
- SQLite partitioned by `model_id`
- Dashboard panel: per-model optimization history, "Run Optimization" button
- Apply policy: diff shown, click to apply, never auto-apply
- **Test:** 30 trials x coding suite completes, results persisted, dashboard
  shows history
- **Deliverable:** `apps/benchmark-runner/config/coding_search.yaml` etc.

### P10 — Hardening + Real-Hardware Validation (Day 27-29)
- Long-context: 64K x 2 slots x 50 sequential requests, no LCP breaks
- Swap stress: 20 rapid swap cycles (27B->Coder->27Bx10), no leaks
- Multi-hour opencode session: 4h, both models used, swap mid-session
- Memory leak check: VRAM returns to baseline after swap
- Crash recovery: kill mid-swap, coordinator recovers, no orphan VRAM
- **Test:** all pass
- **Deliverable:** `tests/stress/` + postmortem report

### P11 — Final Polish + Documentation (Day 30-32)
- `config/gateway.yml` final form
- `AGENTS.md` (this file's sibling) — for future devs/agents
- Dashboard: model switcher, swap controls, optimization history, parity badge
- README: model selection guide, swap docs
- Both models benchmarked, optimized profiles applied
- **Test:** manual opencode run, both models, swap, optimization applied
- **Deliverable:** release notes

## 6. VRAM Budget (R9700 32GB)

| Active model | mmproj | slots | KV @ 64K Q8 | Compute | Total |
|---|---|---|---|---|---|
| Qwen3.6-27B | 4.5 GB | x2 | 5.0 GB | 0.8 GB | **28.3 GB** OK |
| Qwen3-Coder-Next | 0 GB | x2 | 5.0 GB | 0.8 GB | **29.8 GB** OK |
| Qwen3-Coder-Next | 4.5 GB | x1 | 2.5 GB | 0.8 GB | **31.8 GB** OK tight |

## 7. Testing Strategy

| Layer | Test type | Tool | Gate |
|---|---|---|---|
| Foundation | Unit | GoogleTest | All pass |
| Messaging | Unit + JSON round-trip | GoogleTest | All pass |
| Sampling | Parity vs raw llama-server | simhash + Levenshtein | >= 0.95 |
| Model | Swap cycle | custom | < 30s, no leaks |
| Engine | LCP hit verification | custom | Hit rate > 80% on shared prompts |
| Scheduler | Concurrency | custom | 2 slots used, queue < 30s |
| HTTP | Integration | custom | opencode end-to-end |
| Observability | Stats accuracy | GoogleTest | Error < 0.1% |
| Parity | CI gate | bash script | All registered models pass |
| Optimization | Per-model benchmark | Optuna | Results persisted |
| Hardening | Stress + 4h session | bash script | Zero OOM, zero crash |

## 8. File Structure

```
InferDeck/
├── PLAN.md                          (this file)
├── AGENTS.md                        (agent instructions)
├── README.md
├── CMakeLists.txt                   (root, top-level)
├── cmake/                           (presets, toolchain)
├── apps/
│   ├── gateway-service/             (Layer 8 — HTTP routes)
│   │   ├── CMakeLists.txt
│   │   ├── src/
│   │   └── tests/
│   ├── benchmark-runner/            (Layer 9 — Optuna)
│   │   ├── CMakeLists.txt
│   │   ├── src/
│   │   └── config/
│   ├── model-tester/                (P3 — swap exerciser)
│   └── hardware-adlx-helper/        (existing, reused)
├── libs/
│   ├── foundation/                  (Layer 1)
│   ├── messaging/                   (Layer 2)
│   ├── sampling/                    (Layer 3)
│   ├── model/                       (Layer 4 — registry + coordinator)
│   ├── engine/                      (Layer 5 — slot pool)
│   ├── scheduler/                   (Layer 6)
│   ├── observability/               (Layer 7)
│   ├── llama_cpp_wrapper/           (legacy, will shrink)
│   ├── core/                        (legacy, will shrink)
│   └── third_party/
│       └── llama.cpp/               (Layer 0)
├── config/
│   ├── gateway.yml                  (active config)
│   ├── sampler-profiles/            (per-model sampler configs)
│   │   ├── qwen3.6-27b-coding.yaml
│   │   ├── qwen3.6-27b-chat.yaml
│   │   └── qwen3-coder-next-coding.yaml
│   └── bench-search-spaces.yaml     (per-model search spaces)
└── tests/
    ├── parity/                      (P8 — CI gate)
    ├── stress/                      (P10)
    └── integration/                 (P6)
```

## 9. GGUF Files Required

| Model | Path | Size | Source |
|---|---|---|---|
| Qwen3.6-27B Q4_K_M | `C:/Users/david/Documents/00_Models/Qwen3.6-27B-GGUF/Qwen3.6-27B-Q4_K_M.gguf` | 18 GB | https://huggingface.co/unsloth/Qwen3.6-27B-GGUF |
| Qwen3.6-VL BF16 | `C:/Users/david/Documents/00_Models/Qwen3.6-VL/Qwen3.6-VL-BF16.gguf` | 4.5 GB | https://huggingface.co/Qwen/Qwen3.6-VL |
| Qwen3-Coder-Next Q4_K_M | `C:/Users/david/Documents/00_Models/Qwen3-Coder-Next-GGUF/Qwen3-Coder-Next-Q4_K_M.gguf` | 24 GB | https://huggingface.co/unsloth/Qwen3-Coder-Next-GGUF |

**Total: 46.5 GB.** Download in P0 prep or defer to P3.

## 10. Open Items (resolved)

All decisions locked. Implementation begins on plan mode exit.
