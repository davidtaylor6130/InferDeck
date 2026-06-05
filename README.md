# InferDeck

A local AI gateway. Single Windows `.exe` that runs LLMs (Qwen3.6-27B and
Qwen3-Coder-Next) in-process via llama.cpp, exposes an OpenAI-compatible
HTTP API on `:11434/v1/`, and serves a React dashboard on `:11434/`.

> **Branch:** `inferdeck-2.0-multimodel`. v2 replaces the v1 Node/Fastify
> stack with a clean, layered, in-process C++23 multimodel gateway. See
> `PLAN.md` for the full plan and commit log, and `AGENTS.md` for build/test
> commands.

## What it does

- **Multimodel, hot-swap.** `qwen3.6-27b` (default, vision) and
  `qwen3-coder-next` (specialist, no vision) registered at startup. Switch
  via the OAI `model:` field or the dashboard. Only one model is loaded at
  a time (32 GB VRAM ceiling); swap drains active requests, unloads, reads
  the new GGUF, initialises `n_parallel=2` contexts.
- **OpenAI-compatible.** `POST /v1/chat/completions` (streaming + non-
  streaming), `GET /v1/models`, `POST /v1/swap/to/{model}`,
  `GET /v1/swap/status`, `GET /v1/health`, `GET /v1/metrics`,
  `GET /v1/stats/history`, `WebSocket /v1/stats`.
- **Parity CI gate.** 0.95 LCS token similarity vs raw `llama-server.exe`
  per registered model. See `tests/parity/README.md`.
- **Automated sampler tuning.** `inferdeck-bench run --trials 30` produces
  `~/.inferdeck/optimization.json`. Apply a trial's params to
  `config/sampler-profiles/{model}.yaml` after human review.
- **Live observability.** GPU util / temp / VRAM / power via ADLX
  (subprocess `inferdeck-adlx-helper.exe --json`); per-request t/s, prompt
  + completion tokens, slot id, status; swap history. Persisted to
  `~/.inferdeck/stats.db` (SQLite).

## Hard constraints

- **In-process.** No `llama-server.exe` subprocess, no `popen`, no orphan
  processes. The only subprocess is `inferdeck-adlx-helper.exe` for GPU
  telemetry (per `apps/hardware-adlx-helper/` reuse rule in `AGENTS.md`).
- **llama.cpp library directly.** No proxy design, no CLI parsing of
  `llama-server.exe` output. We link `llama.dll` and call the C API.
- **No auto-swap on OAI request.** Swap is a dashboard click. The
  Scheduler returns 503 with a hint instead.
- **No MTP for v1.** Conflicts with `n_parallel>1`.
- **mmproj stays loaded** permanently for the vision model.

## Build

```bash
# Configure
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH="C:/Users/david/Documents/GitHub/InferDeck/vcpkg_installed/x64-windows" \
      -DINFERDECK_BUILD_TESTS=ON

# Build
cmake --build build --config Release -j
```

Output: `build/Release/inferdeck-gateway.exe` and
`build/Release/inferdeck-bench.exe`.

> The vcpkg toolchain shim `cmake/vcpkg-msvc.cmake` is currently broken
> (recursive `CMAKE_TOOLCHAIN_FILE`); use the `CMAKE_PREFIX_PATH` form
> above until it is fixed.

## Run

```bash
# 1. Drop GGUFs into place (see config/gateway.yml for expected paths)
# 2. Optionally edit config/gateway.yml to register new models
# 3. Start the gateway
./build/Release/inferdeck-gateway.exe

# 4. Verify
curl http://localhost:11434/v1/models
curl http://localhost:11434/v1/health
```

The gateway auto-preloads the model named in `~/.inferdeck/state.json`
(falling back to `config/gateway.yml#default_model`).

## Test

```bash
# Unit + integration (every commit, fast)
ctest --test-dir build --output-on-failure -LE e2e

# Parity (CI gate)
bash tests/parity/run.sh

# Real-model end-to-end (slow, needs a real GGUF)
INFERDECK_TEST_MODEL="C:/Inferdeck/models/Qwen/Qwen2.5-Coder-3B-Instruct-GGUF/qwen2.5-coder-3b-instruct-q4_k_m.gguf" \
  bash tests/integration/run.sh e2e

# Swap stress + 4h session (pre-release)
bash tests/stress/swap_cycle.sh 20
bash tests/stress/four_hour_session.sh
```

Current passing count: **84/87 active tests** (3 placeholder cases for
`test_metrics.cpp` are excluded from the build because Windows Defender
holds a real-time lock on that file; coverage is provided transitively by
the 14 other observability tests).

## Manual smoke tests

```bash
# List models
curl http://localhost:11434/v1/models | jq

# Trigger a swap (background, returns 202)
curl -X POST http://localhost:11434/v1/swap/to/qwen3-coder-next

# Poll status
curl http://localhost:11434/v1/swap/status | jq

# Once ready, run a chat completion
curl -X POST http://localhost:11434/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{"model":"qwen3-coder-next","messages":[{"role":"user","content":"hello"}]}'
```

## Run an optimization

```bash
# CLI: dry-run, synthetic scoring
./build/Release/inferdeck-bench run \
  --model qwen3.6-27b \
  --profile coding \
  --suite humaneval,mbpp \
  --trials 30 \
  --dry-run \
  --output build/optimization.json

# Dashboard: click "Run Optimization" -> select model + profile
# Results in ~/.inferdeck/optimization.json, view in dashboard
```

## Add a new model

1. Download GGUF to `C:/Users/david/Documents/00_Models/{name}-GGUF/`.
2. Add entry to `config/gateway.yml#model_registry`.
3. (Optional) Add sampler profile to `config/sampler-profiles/{name}.yaml`.
4. Restart `inferdeck-gateway.exe`; the new model appears in `/v1/models`.
5. Run parity: `bash tests/parity/run.sh {name}`.

## File layout

```
apps/inferdeck-gateway/         HTTP routes + .exe entry (Layer 8)
apps/benchmark-runner/          inferdeck-bench (Layer 9)
apps/dashboard/                 React dashboard source
apps/hardware-adlx-helper/      ADLX subprocess for GPU telemetry
libs/foundation/                Layer 1: asio, logging, JSON
libs/messaging/                 Layer 2: std::variant content, role enum
libs/sampling/                  Layer 3: common_sampler_init wrapper
libs/model/                     Layer 4: ModelRegistry + BackendCoordinator
libs/engine/                    Layer 5: per-model slot pool (n_parallel=2)
libs/scheduler/                 Layer 6: LCP-match + 30s queue
libs/observability/             Layer 7: ADLX + EMA stats + SQLite
libs/llama_cpp_wrapper/         Real LlamaCppModel (P10)
libs/optimize/                  In-house search (P9)
libs/third_party/llama.cpp      Layer 0: Vulkan build
config/gateway.yml              Active config
config/sampler-profiles/        Per-model sampler YAMLs
config/bench-search-spaces.yaml Default search space
tests/parity/                   CI gate: parity with raw llama-server
tests/stress/                   4h session, swap cycles
tests/integration/              HTTP end-to-end
```

## License

Internal.
