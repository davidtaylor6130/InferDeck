# InferDeck

A self-hosted AI gateway for Windows: a single C++23 executable that runs
LLMs **in-process** via [llama.cpp](https://github.com/ggml-org/llama.cpp)
(Vulkan), exposes an **OpenAI-compatible HTTP API** on `:11434/v1/`, and
serves a live **React dashboard** on the same port.

Built to run unattended on a single-GPU workstation and serve coding
agents (opencode, Open WebUI, Claude-style clients) around the clock —
with hot model swapping, streaming tool calls, and full observability.

## Highlights

- **In-process inference.** Links `llama.dll` and drives the llama.cpp C
  API directly — no `llama-server.exe` subprocess, no proxying, no orphan
  processes. The only helper subprocess is an optional ADLX GPU-telemetry
  probe.
- **Multi-model with hot swap.** Models are registered in
  `config/gateway.yml`; only one is resident at a time (single-GPU VRAM
  budget). `POST /v1/swap/to/:name` is fully async: it drains active
  requests, unloads, loads the new GGUF, and streams progress to the
  dashboard over SSE — with cancellation support.
- **OpenAI- and Anthropic-compatible API.** `POST /v1/chat/completions`
  (streaming + non-streaming, tool calls, `reasoning_content`), Anthropic
  Messages API support, `/v1/models`, `/v1/health`, `/v1/metrics`. Prompt
  overflow is truncated llama-server-style instead of erroring.
- **KV-cache reuse.** Longest-common-prefix prompt matching so multi-turn
  agent sessions don't re-prefill the whole conversation each turn.
- **Live dashboard.** React 19 + Vite + Tailwind, driven by a single SSE
  stream: GPU utilisation/VRAM/temperature/power sparklines, per-request
  tokens-per-second, swap progress with cancel, token/cost tracking with
  per-model pricing, and a log viewer.
- **Observability.** Every request and swap is recorded three ways:
  in-memory metrics, SQLite history (`stats.db`), and SSE events.
  p50/p95 latency, daily/hourly usage buckets, lifetime counters.
- **Parity-tested.** A CI gate checks ≥0.95 LCS token similarity against
  raw `llama-server` per registered model, plus Catch2 unit/integration
  suites and a streaming tool-call harness.

## Architecture

```
              ┌──────────────────────── inferdeck-gateway.exe ───────────────────────┐
  HTTP :11434 │  libs/gateway        /v1 routes, /api dashboard routes, SSE,        │
  ────────────▶                      streaming sanitizer, SwapTracker, auth, CORS   │
              │  libs/model          ModelRegistry + BackendCoordinator (slots,     │
              │                      drain-on-swap, 30s acquisition queue)          │
              │  libs/llama_cpp_wrapper  LlamaCppModel: template/tokenize/decode/   │
              │                      sample, LCP prompt-cache reuse                 │
              │  libs/observability  GPU telemetry (PDH/DXGI/ADLX), Metrics,        │
              │                      SQLite StatsDb                                 │
              │  libs/foundation     logging, Result/Error, EventBus                │
              └──────────────────────────────┬───────────────────────────────────────┘
                                             │ links
                                      llama.cpp (Vulkan)
```

Request flow: route handler parses the OAI body → `BackendCoordinator`
hands out a slot → streaming inference runs on a dedicated thread pushing
deltas through a condition-variable-guarded queue into the chunked HTTP
response → metrics + SQLite + SSE event on completion. The coordinator
never holds its mutex during inference, so status endpoints and second
slots stay responsive mid-generation.

```
apps/inferdeck-gateway/   exe entry: config, dependency wiring, routes, static files
apps/dashboard/           React dashboard (built output is committed and served by the exe)
apps/benchmark-runner/    inferdeck-bench sampler-optimization harness
apps/hardware-adlx-helper/ optional GPU telemetry helper
libs/                     gateway, model, llama_cpp_wrapper, observability, optimize, foundation
config/                   gateway.yml, per-model sampler profiles
tests/                    Catch2 unit/integration, parity gate, fixtures, stress
docs/                     API reference, architecture notes, deploy guide
```

## Build

Requires Visual Studio 2022 (MSVC, C++23), CMake ≥ 3.27, vcpkg, and the
Vulkan SDK. llama.cpp is cloned into `libs/third_party/` (not a
submodule).

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DINFERDECK_BUILD_TESTS=ON
cmake --build build --target inferdeck-gateway --config Release -j

# Dashboard (output lands in apps/inferdeck-gateway/static/)
pnpm install
pnpm --filter dashboard build
```

## Run

```bash
# 1. Download GGUF model(s) and point config/gateway.yml#model_registry at them
# 2. Start the gateway
./build/bin/Release/inferdeck-gateway.exe

# 3. Verify
curl http://localhost:11434/v1/models
curl http://localhost:11434/v1/health
# Dashboard: http://localhost:11434/
```

Point any OpenAI-compatible client at `http://localhost:11434/v1` with
any API key.

## Test

```bash
# C++ unit + integration
ctest --test-dir build -C Release --output-on-failure -L "unit|integration"

# Dashboard unit tests
pnpm --filter dashboard test

# Parity gate vs raw llama-server (slow, needs real GGUFs)
bash tests/parity/run.sh
```

## API surface

| Endpoint | Notes |
| --- | --- |
| `POST /v1/chat/completions` | OpenAI-compatible; SSE streaming, tool calls, reasoning content |
| `POST /v1/messages` | Anthropic Messages API |
| `GET /v1/models` · `/v1/health` · `/v1/metrics` | discovery + ops |
| `POST /v1/swap/to/:name` | async swap, `202` + SSE progress; `/v1/swap/cancel`, `/v1/swap/status` |
| `GET /api/status` · `/api/jobs` · `/api/logs` · `/api/pricing` | dashboard data |
| `GET /api/events/stream` | SSE: `stats` (~1 Hz), `model`, `request` events |

Full reference in [`docs/api/`](docs/api/).

## Docs

- [`AGENTS.md`](AGENTS.md) — engineering guide: build/test commands,
  architecture quick reference, concurrency invariants, and design rules
  learned the hard way.
- [`docs/architecture.md`](docs/architecture.md), [`docs/DEPLOY.md`](docs/DEPLOY.md)
- [`CHANGELOG.MD`](CHANGELOG.MD)

## License

[MIT](LICENSE)
