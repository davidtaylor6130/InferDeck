# InferDeck Gateway Architecture (v2 — Multimodel)

> **v2 addendum.** The original document described a single-model
> OpenAI-compatible gateway. v2 (branch `inferdeck-2.0-multimodel`) replaces it
> with a layered, in-process, multimodel gateway with hot-swap, observability,
> parity CI, and automated sampler tuning. This file documents the v2
> multimodel story; the original v1 narrative is preserved below for reference.

## v2 — Multimodel in-process gateway (current)

### Goals

- Run Qwen3.6-27B (default, vision) and Qwen3-Coder-Next (specialist, no
  vision) in a single process, hot-swap between them.
- Match `llama-server.exe` output for each registered model to **0.95 LCS
  similarity** (CI gate).
- No subprocess. Single `.exe`. No orphan `llama-server.exe` on Windows.
- Per-model sampler search (30 trials) via `inferdeck-bench`; human-confirm
  apply.
- Live observability: GPU telemetry (ADLX), per-request stats, swap history,
  SQLite persistence.

### 9-layer architecture

```
Layer 0  libs/third_party/llama.cpp      — Vulkan build, library only
Layer 1  libs/foundation/                 — asio, logging, JSON
Layer 2  libs/messaging/                  — std::variant content, role enum
Layer 3  libs/sampling/                   — common_sampler_init wrapper
Layer 4  libs/model/                      — ModelRegistry + BackendCoordinator
Layer 5  libs/engine/                     — Per-model slot pool (n_parallel=2)
Layer 6  libs/scheduler/                  — LCP-match + 30s queue
Layer 7  libs/observability/              — ADLX + EMA stats + SQLite
Layer 8  apps/inferdeck-gateway/          — HTTP routes, .exe entry
Layer 9  apps/benchmark-runner/           — inferdeck-bench optimization
```

Each layer is a CMake subdir with `include/`, `src/`, and `tests/`. Layers
depend downward only.

### Request lifecycle

When a request hits `POST /v1/chat/completions`:

1. **Layer 8** parses OAI body, extracts `model:`, `messages:`, `tools:`.
2. **Layer 2** converts to internal `Message` representation
   (`std::variant<TextContent, ImageContent, ToolCallContent, ...>`).
3. **Layer 5/6** Scheduler calls `BackendCoordinator::AcquireSlot(model)`.
4. **Layer 4** Coordinator ensures target model is loaded (swap if necessary),
   returns a free slot from the per-model pool.
5. **Layer 5** Engine tokenizes prompt, checks LCP against the slot's KV cache
   (`llama_kv_cache_seq_rm` to trim divergent tail).
6. **Layer 3** Sampling builds the sampler chain from the model's profile
   (`llama_sampler_chain_init` + `top_k` + `top_p` + `min_p` + `temp` + `dist`).
7. **llama.cpp** runs the inference loop (`llama_decode` + sampler).
8. **Layer 7** Observability records t/s, tokens, slot id to SQLite
   (`~/.inferdeck/stats.db`).
9. **Layer 8** streams SSE chunks back to the client.

### Swap lifecycle

When a swap is requested (dashboard click or `POST /v1/swap/to/{model}`):

1. **Layer 4** Coordinator sets `swap_in_progress_ = true` and clears the
   cancel flag.
2. Drains active requests on the current model (30s timeout).
3. Polls `swap_cancel_` after each step; returns
   `Error{Cancelled, ...}` and rolls back if cancellation was requested.
4. Unloads current model: destroys contexts, frees VRAM.
5. Reads new GGUF from disk (`llama_model_load_from_file`).
6. Initialises `n_parallel` contexts (`llama_init_from_model`).
7. Resets `swap_in_progress_ = false` and clears the cancel flag.
8. **Layer 7** logs the swap event to SQLite.
9. WebSocket broadcasts `ready` to the dashboard.

### Configuration

`config/gateway.yml` is the only config file the gateway reads at startup.
Highlights:

- `server`: bind host/port, request timeouts.
- `logging`: structured-log sinks, rotation.
- `auth`: optional bearer token (default off for local use).
- `cors`: allowed origins.
- `model_registry[]`: per-model entry with name, gguf path, optional
  mmproj path, family, n_slots, vram_required_mb.
- `observability`: stats_db path, ADLX helper path, telemetry poll interval.
- `sampler_profiles_dir`: directory of per-model sampler YAMLs.
- `default_model`: which model to preload on startup
  (overridden by `~/.inferdeck/state.json` last-loaded).

### File layout

```
apps/inferdeck-gateway/         HTTP routes + .exe entry (Layer 8)
apps/benchmark-runner/          inferdeck-bench (Layer 9)
apps/dashboard/                 React dashboard source
apps/hardware-adlx-helper/      ADLX subprocess for GPU telemetry
libs/foundation/                Layer 1
libs/messaging/                 Layer 2
libs/sampling/                  Layer 3
libs/model/                     Layer 4
libs/engine/                    Layer 5
libs/scheduler/                 Layer 6
libs/observability/             Layer 7
libs/llama_cpp_wrapper/         Real LlamaCppModel (replaces legacy stub)
libs/optimize/                  In-house search (random + greedy)
libs/third_party/llama.cpp      Layer 0, Vulkan build
config/gateway.yml              Active config
config/sampler-profiles/        Per-model sampler YAMLs
config/bench-search-spaces.yaml Default search space
tests/parity/                   CI gate: parity with raw llama-server
tests/stress/                   4h session, swap cycles
tests/integration/              HTTP end-to-end
```

---

## v1 — Single-model OpenAI gateway (preserved for reference)

### Overview

InferDeck Gateway is a production-grade C++ 23 application that provides a
strict OpenAI-compatible API for local LLM inference. It bridges the gap
between `llama.cpp` and applications expecting the OpenAI API format.

### Component Diagram

```
┌─────────────────────────────────────────────────┐
│                React Dashboard                   │
│   (apps/dashboard – unchanged, updated API      │
│    layer to /v1/... and /inferdeck/...)         │
└──────────────────────┬──────────────────────────┘
                       │ HTTPS + SSE / WebSocket
                       ▼
┌─────────────────────────────────────────────────┐
│              Gateway Service (.exe)              │
│                                                 │
│  ┌──────────┐  ┌──────────┐  ┌───────────────┐  │
│  │  HTTP/S   │  │  SSE     │  │  Config       │  │
│  │ Server    │  │ Stream   │  │  (YAML)       │  │
│  │(cpp-httpl│  │ Handler  │  │  (spdlog)     │  │
│  └──────────┘  └──────────┘  └───────────────┘  │
│                                                 │
│  ┌───────────────────────────────────────────┐  │
│  │           Job Queue & Worker Pool          │  │
│  │    (priority queue → LlamaEngine)         │  │
│  └───────────────────────────────────────────┘  │
│                                                 │
│  ┌───────────────────────────────────────────┐  │
│  │              LlamaEngine                   │  │
│  │  (libs/llama_cpp_wrapper — legacy)         │  │
│  └───────────────────────────────────────────┘  │
│                                                 │
└─────────────────────────────────────────────────┘
```

All errors follow OpenAI schema:

```json
{
  "error": {
    "message": "Model not found",
    "type": "invalid_request_error",
    "param": "model",
    "code": "model_not_found"
  }
}
```

### Security

- TLS 1.2+ with self-signed certs (generated during build)
- CORS configured via `api.cors_origins` in gateway.yml
- Request validation before inference to prevent malformed input
- Graceful shutdown via signal handlers (SIGINT, SIGTERM)

### Build System

- CMake 3.27+ with C++23 standard
- vcpkg for dependency management
- Self-signed TLS certs generated during build
- Coverage with gcov/lcov (CI future)
