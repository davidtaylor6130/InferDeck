<div align="center">

<img src="docs/assets/banner.svg" alt="InferDeck, self-hosted AI gateway for Windows" width="100%"/>

<br/>

[![C++23](https://img.shields.io/badge/C%2B%2B-23-00599C?logo=cplusplus&logoColor=white)](https://en.cppreference.com/w/cpp/23)
[![Platform](https://img.shields.io/badge/platform-Windows%20x64-0078D6?logo=windows&logoColor=white)](#build-from-source)
[![Backend](https://img.shields.io/badge/backend-llama.cpp%20%C2%B7%20Vulkan-A41E22)](https://github.com/ggml-org/llama.cpp)
[![API](https://img.shields.io/badge/API-OpenAI%20compatible-412991?logo=openai&logoColor=white)](#api-surface)
[![Dashboard](https://img.shields.io/badge/dashboard-React%2019%20%2B%20SSE-61DAFB?logo=react&logoColor=black)](#live-dashboard)
[![License: MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

**A single C++23 executable that runs LLMs in-process via [llama.cpp](https://github.com/ggml-org/llama.cpp),
exposes an OpenAI-compatible API on `:11434`, and serves a live React dashboard on the same port.**

[Features](#features) · [Architecture](#architecture) · [Quick start](#quick-start) · [API](#api-surface) · [Roadmap](#roadmap) · [Docs](#documentation)

</div>

---

## Why InferDeck

My "AI server" is also my gaming and dev PC, a Windows machine upgraded with
a Radeon AI PRO R9700. I wasn't willing to switch it to Linux or maintain a
dual boot just to serve models, and I was already building
[Universal Agent Manager](https://github.com/davidtaylor6130/Universal-Agent-Manager),
which needed a local inference backend it could control over the network and
trust to run unattended.

None of the existing options fit that setup. **LM Studio**'s server ate too
much system RAM. **Ollama** was slow and a faff to control programmatically.
Raw **llama-server.exe** generates well but is hard to manage over the
network. **vLLM** is built for a different scale than a single-GPU Windows
box. So I built my own gateway that aims to retain `llama-server.exe`'s
response quality, with a manually provisioned parity harness for comparison,
while adding the control layer the others lacked. It was also a welcome excuse
to get back into a serious modern-C++ project.

The guiding idea is simple: **one GPU, fully under your control, with
overlapping work queued.** My first attempt was a bodged-together stack of a
server binary, proxy, separate UI, and a script that restarted whatever fell
over. It proved the idea, but it was awkward to operate reliably. InferDeck is
the deliberate replacement: **one process** where every model is managed from
the dashboard and overlapping requests are **queued and scheduled, not
rejected**. It is built to run unattended on a single-GPU workstation and
serve coding agents (opencode, Open WebUI, Claude-style clients) around the
clock.

It links `llama.dll` and drives the llama.cpp C API directly, with no
`llama-server.exe` subprocess, proxying, or orphan processes. It wraps this
with the operational layer that raw llama.cpp doesn't have: hot model swapping,
KV-cache reuse across agent turns, request history, cost tracking, and a
real-time dashboard.

The long-term goal is **your own AI datacentre at home, on one machine**:
text, speech, images, music and eventually video, available through one local
service, API and dashboard. One GPU shares the work within its memory limits;
model quality, context size and waiting time are practical tradeoffs. Supported
workloads run locally without a cloud inference dependency.

**The current priority is unattended server use on Windows with AMD GPUs.**
Performance, stability, remote operation and clear dashboard controls come
first. The longer-term installation goal spans Windows, macOS and Linux/Docker,
with model recommendations and setup suited to the actual hardware. See the
[build-out plan](#build-out-plan) for the order and completion criteria.

> [!NOTE]
> InferDeck is a working daily-driver, but it's also deliberately a
> **learning project**. Part of the goal is to explore the problem space, so
> some subsystems take the experimental route where a boring, conventional one
> would do. That is a feature, not an accident. The parity harness and test
> suites are there to keep the experiments honest.

## Features

### Inference engine

- **In-process llama.cpp (Vulkan).** Direct C-API integration with no backend
  subprocess, proxy, or orphan process.
- **Multi-model residency with concurrent execution.** Models register in
  `config/gateway.yml`; the coordinator uses fresh GPU headroom when available
  and declared footprints as a safe fallback. Fitting LLM, Image, and Music
  runtimes stay resident and their independent admission pools can run
  together. Lifecycle loads remain serialized.
  `POST /api/inferdeck/v1/swap/to/:name` loads the selected model, evicts only
  the idle capacity it needs, and streams progress to the dashboard over SSE.
- **KV-cache reuse.** Longest-common-prefix prompt matching, so multi-turn
  agent sessions reuse full-attention KV state and hybrid recurrent
  checkpoints instead of re-prefilling the whole conversation each turn.
- **Honest modality discovery.** Text models advertise text input only until
  the in-process multimodal projector path is implemented.

### API

- **OpenAI-compatible** `POST /v1/chat/completions`: SSE streaming,
  tool calls, and llama-server-style prompt truncation on
  context overflow instead of a hard error.
- **OpenAI Responses and embeddings APIs** at `POST /v1/responses` and
  `POST /v1/embeddings`. Responses is stateless; unsupported storage,
  background, and conversation fields are rejected explicitly.
- **Strict OpenAI Core.** `/v1` exposes only the pinned OpenAI surface.
  OpenAI-derivative routes use their disabled-by-default compatibility prefix;
  InferDeck-specific APIs stay under `/api/inferdeck/v1`.
- **Native audio APIs.** CPU-only Parakeet
  TDT 0.6B v3 transcription at `POST /v1/audio/transcriptions` and in-process
  Supertonic 3 speech synthesis at `POST /v1/audio/speech` are release-built
  and live-verified end to end.
- **Native music generation.** The compile-gated `ace_step_cpp` runtime accepts
  text and optional lyrics at `POST /api/inferdeck/v1/audio/generations` and
  returns one 48 kHz stereo PCM16 WAVE file. The direct synthesis path passes
  real-model Windows/Vulkan validation.
- **Native image generation.** The compile-gated stable-diffusion.cpp backend
  at `POST /v1/images/generations` passes real-model Windows/Vulkan validation.
- **Image and Music workspaces.** The dashboard can submit local generations,
  cancel active jobs, show failures, preview PNG/WAV outputs, and download
  persisted results.
- **Native GGUF quantisation.** A control-plane job at
  `POST /api/inferdeck/v1/post-training/quantizations` calls llama.cpp
  in-process, accepts only unloaded managed GGUF sources, disables
  requantisation, and registers the finished model without replacing its
  source.
- Discovery and operations endpoints: `GET /v1/models`, `GET /api/inferdeck/v1/health`,
  `GET /api/inferdeck/v1/metrics`, and `GET /api/inferdeck/v1/stats/history`.

### Live dashboard

React 19 + Vite + Tailwind, driven by one SSE connection with a bounded
30-second status fallback. The task views separate Model Settings from model
catalogue, installed-artifact management, server-owned usage pricing, and
diagnostics. Dedicated Image and Music pages use the authenticated dashboard
session and retain bounded output history. Voice capture, transcription, and
speech playback belong to API clients such as Open WebUI.

Loopback dashboard access is passwordless. LAN and encrypted-overlay access
requires remote control to be enabled, an exact `control.origins` entry, and
the separate control token. The browser exchanges that token for an HTTP-only,
same-site session cookie so native SSE and administrative actions remain
authenticated.

### Observability & quality

- Every request and swap is recorded three ways: in-memory metrics, SQLite
  history (`stats.db`), and SSE events. p50/p95 latency, daily/hourly usage
  buckets, generation TPS, prompt TPS, measured peak TPS, and lifetime counters.
- Catch2 unit/integration suites and a streaming tool-call harness cover API
  shape and runtime behaviour. Real-model parity remains a manually provisioned
  hardware test.
- `build/bin/Release/inferdeck-bench.exe --dry-run` validates search-space
  parsing and optimiser mechanics. The dashboard can run measured, fixed-seed
  quality and throughput benchmarks before staging a model profile for
  validation and hot application.

## Architecture

```
              ┌──────────────────────── inferdeck-gateway.exe ───────────────────────┐
  HTTP :11434 │  libs/gateway        /v1 routes, /api dashboard routes, SSE,        │
  ────────────▶                      streaming sanitizer, SwapTracker, auth, CORS   │
              │  libs/model          ModelRegistry + BackendCoordinator (slots,     │
              │                      drain-on-swap, priority/ageing queue)           │
              │  libs/llama_cpp_wrapper  LlamaCppModel: template/tokenize/decode/   │
              │                      sample, LCP prompt-cache reuse                 │
              │  libs/observability  GPU telemetry (PDH/DXGI), Metrics,             │
              │                      SQLite StatsDb                                 │
              │  libs/foundation     logging, Result/Error, EventBus                │
              └──────────────────────────────┬───────────────────────────────────────┘
                                             │ links
                                      llama.cpp (Vulkan)
```

**Request flow:** route handler parses the OpenAI body → `BackendCoordinator`
hands out a slot → streaming inference runs on a dedicated thread, pushing
deltas through a condition-variable-guarded queue into the chunked HTTP
response → metrics + SQLite + SSE event on completion. The coordinator never
holds its mutex during inference, so status endpoints and second slots stay
responsive mid-generation.

<details>
<summary><b>Repository layout</b></summary>

```
apps/inferdeck-gateway/    exe entry: config, dependency wiring, routes, static files
apps/dashboard/            React dashboard (built output is committed and served by the exe)
apps/benchmark-runner/     inferdeck-bench sampler-optimisation harness
apps/hardware-adlx-helper/ standalone ADLX probe experiment; not launched by the gateway
libs/                      gateway, model, llama_cpp_wrapper, observability, optimize, foundation
config/                    gateway.yml, per-model sampler profiles
tests/                     Catch2 integration and parity suites, plus request fixtures
Testing/                   manual streaming, overflow, compaction, and cache-reuse harnesses
docs/                      API reference, architecture notes, deploy guide
```

</details>

## Quick start

### Clone

`llama.cpp`, `Vulkan-Headers`, `stable-diffusion.cpp`, and `acestep.cpp` are
pinned Git submodules:

```bash
git clone --recurse-submodules https://github.com/davidtaylor6130/InferDeck.git
cd InferDeck
```

If the repository was cloned without submodules, initialise them before
configuring the build:

```bash
git submodule update --init --recursive
```

### Prerequisites

- Windows 10/11 x64, a Vulkan-capable GPU
- Visual Studio 2022 (MSVC, C++23), CMake ≥ 3.27, Vulkan SDK, and vcpkg
  with `VCPKG_ROOT` set
- Node.js 22 + pnpm 9 (dashboard only)

### Build from source

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DINFERDECK_BUILD_TESTS=ON
cmake --build build --config Release --parallel

# Dashboard (output lands in apps/inferdeck-gateway/static/)
pnpm install
pnpm --filter dashboard build
```

### Optional speech setup

The active speech models are Parakeet TDT 0.6B v3 and Supertonic 3 through
sherpa-onnx. There is no complete automated setup script for this configuration.
The integration is still experimental and has not yet been thoroughly tested
end to end.
Supply a sherpa-onnx installation prefix containing
`include/sherpa-onnx/c-api/c-api.h`, `lib/sherpa-onnx-c-api.lib`, and the
matching runtime DLLs, then configure with its path:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DINFERDECK_BUILD_TESTS=ON `
  -DINFERDECK_SHERPA_ONNX_ROOT=C:/path/to/sherpa-onnx-install
```

Download the Parakeet and Supertonic model artefacts separately, then update
their `artifacts` paths in `config/gateway.yml`. The existing
`scripts/setup-whisper-runtime.ps1` installs the optional whisper.cpp fallback;
it does not set up the active Parakeet or Supertonic models.

> [!WARNING]
> The default homelab configuration binds to `0.0.0.0`, disables
> authentication, and allows all CORS origins. Do not expose it directly to
> the public internet. Use it only on a trusted LAN or through a VPN, firewall,
> or properly configured reverse proxy. Enable authentication and restrict
> CORS origins where appropriate.

### Run

```bash
# 1. Download GGUF model(s) and point config/gateway.yml#model_registry at them
# 2. Start the gateway
./build/bin/Release/inferdeck-gateway.exe

# 3. Verify
curl http://localhost:11434/v1/models
curl http://localhost:11434/api/inferdeck/v1/health
# Dashboard: http://localhost:11434/
```

Point an OpenAI-compatible client at `http://localhost:11434/v1`. With the
default authentication setting, clients that require a key may use a
placeholder. When authentication is enabled, send the configured Bearer token.
For example:

```bash
curl http://localhost:11434/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{"model": "qwen3-coder-next", "messages": [{"role": "user", "content": "Hello!"}], "stream": true}'
```

### Test

```powershell
# C++ unit + integration
ctest --test-dir build -C Release --output-on-failure -L "unit|integration"

# Dashboard unit tests
pnpm --filter dashboard test

# Pinned OpenAI JavaScript SDK 7.5.0 contract
pnpm test:openai-contract

# Pinned OpenAI Python SDK 3.3.1 contract
python -m pip install -r Testing/requirements-openai-contract.txt
python -m unittest -v Testing.openai_sdk_contract_test

# Real image generation through a running gateway with a registered image model
powershell -File Testing/Test-ImageGeneration.ps1 `
  -Model stable-diffusion-v1-5-fp16 `
  -Output image-validation.png

# Real music generation through a running gateway with a registered ACE-Step model
powershell -File Testing/Test-AudioGeneration.ps1 `
  -Model ace-step-v1.5-turbo-q4 `
  -DurationSeconds 10 `
  -Output audio-generation-validation.wav

# Prepare the pinned quantisation fixture without invoking llama.cpp
powershell -File Testing/Test-PostTrainingQuantization.ps1 `
  -RouteTests build/bin/Release/route_tests.exe `
  -PrepareOnly

# Real Q8_0 conversion; stop any existing InferDeck instance first
powershell -File Testing/Test-PostTrainingQuantization.ps1 `
  -RouteTests build/bin/Release/route_tests.exe `
  -Output C:\tmp\inferdeck-quantization-validation.gguf

# Real-model parity needs raw llama-server and InferDeck running with the same model
pwsh -File tests/parity/record_baseline.ps1 -Model qwen3.6-27b
pwsh -File tests/parity/run.ps1 `
  -BaselinePath tests/parity/baselines/qwen3.6-27b.jsonl `
  -Model qwen3.6-27b
```

## API surface

| Endpoint | Notes |
| --- | --- |
| `POST /v1/chat/completions` | OpenAI-compatible; SSE streaming and tool calls |
| `POST /v1/responses` | Stateless OpenAI Responses compatibility; streaming, tools, reasoning, and structured output translation |
| `POST /v1/embeddings` | OpenAI-compatible float or base64 embeddings for registered embedding models |
| `POST /v1/audio/transcriptions` | Request-scoped WAV-to-text via native Parakeet TDT or whisper.cpp models |
| `POST /v1/audio/speech` | Request-scoped WAV or PCM output via native Supertonic 3 |
| `POST /v1/images/generations` | OpenAI-compatible base64 PNG generation through native stable-diffusion.cpp; requires a registered image model |
| `POST /api/inferdeck/v1/audio/generations` | InferDeck text-to-music API through native acestep.cpp; returns one 48 kHz stereo PCM16 WAVE file |
| `POST /api/inferdeck/v1/media/images/generations` · `POST /api/inferdeck/v1/media/audio/generations` | Dashboard-session generation routes used by the Image and Music workspaces |
| `GET /api/inferdeck/v1/media/jobs` · `GET /api/inferdeck/v1/media/jobs/:id/outputs/:index` | Bounded generation attempt history and authenticated PNG/WAV output retrieval |
| `GET /v1/models` · `GET /api/inferdeck/v1/health` · `GET /api/inferdeck/v1/metrics` · `GET /api/inferdeck/v1/stats/history` | model discovery, health, live metrics, and usage history |
| `POST /api/inferdeck/v1/swap/to/:name` | async swap, `202` + SSE progress; `POST /api/inferdeck/v1/swap/cancel`; `GET /api/inferdeck/v1/swap/status` |
| `GET /api/inferdeck/v1/status` · `GET /api/inferdeck/v1/jobs` · `GET /api/inferdeck/v1/logs` · `GET /api/inferdeck/v1/pricing` | dashboard data |
| `GET /api/inferdeck/v1/events/stream` | SSE: `stats` (~1 Hz), `model`, `request` events |
| `GET /api/inferdeck/v1/model-store/search` · `GET /api/inferdeck/v1/model-store/inspect` | dashboard model discovery and artefact inspection |
| `GET /api/inferdeck/v1/model-store/downloads` · `POST /api/inferdeck/v1/model-store/downloads` | list or start downloads |
| `POST /api/inferdeck/v1/model-store/downloads/:id/cancel` · `POST /api/inferdeck/v1/model-store/downloads/:id/resume` | cancel or resume a download |
| `POST /api/inferdeck/v1/model-store/remove` | remove an inactive model-store entry and its managed artefact |
| `GET /api/inferdeck/v1/post-training/capabilities` | report the exact quantisation and fine-tuning capability boundary |
| `GET` · `POST /api/inferdeck/v1/post-training/quantizations` | list or start one in-process managed GGUF quantisation job |
| `GET` · `POST /api/inferdeck/v1/api-keys` · `PATCH` · `DELETE /api/inferdeck/v1/api-keys/:id` | create and manage hash-only client keys with server-owned queue priorities; plaintext is returned once |
| `GET /api/inferdeck/v1/background/availability` · `POST /api/inferdeck/v1/background/lease` · `PATCH` · `DELETE /api/inferdeck/v1/background/lease/:id` | coordinate one persisted background-work lease after the configured quiet period; conflicts include a suggested report-back time |

## Roadmap

### Direction

The original roadmap already aimed for one GPU sharing text, speech, image,
video and post-training workloads. That destination remains. This plan makes
the Windows/AMD server the first delivery target, makes music explicit in the
product goal, and puts wider platform support behind a reliable server and
usable controls. The [OpenAI-Core overhaul plan](overhall_plan.md) remains the
record of architecture decisions and verification history.

Core remains one native process with shared admission, residency, cancellation
and observability. Native runtimes do the inference. Strict OpenAI endpoints
stay under `/v1`; capabilities such as music generation and administration use
InferDeck's own API. Cloud fallback is outside this local-first Core plan.

### Immediate target: sustained Qwen3.8-27B concurrency

The first priority is stable parallel serving of **Qwen3.8-27B on Windows/AMD**.
Establish a repeatable baseline before adding more workloads or selecting a
second engine. The current checkout profile uses four slots, 100k context per
slot and adaptive MTP for one active request; these settings are a starting
point to measure, not a performance target.

Test one, two, three and four active requests, then eight submitted requests to exercise
queueing. Cover cold and cached prompts, short requests alongside long prefills,
multi-turn tool histories, disconnects/cancellation and transitions into and out
of MTP. Include at least a 60-minute steady-load run and a burst/recovery run.
Record model/quantisation, context budget, engine revision, driver and hardware.

Acceptance requires no crashes, stuck requests or unexplained sustained memory
growth. Under a fixed arrival rate within measured capacity, queue length and
latency must settle rather than increase indefinitely. Report aggregate output
TPS, per-request TPS, p50/p95 first-token and completion latency, cache reuse,
RAM/VRAM and errors. The generation target is **at least 20 output tokens per
second for each of four active requests**. A measured active-request generation
rate below 20 TPS fails the responsiveness target and needs diagnosis;
combined throughput cannot hide a slow request. Report per-request rates and
token-delivery stalls separately from queue and prefill time.

Prompt processing and cache rebuilding are the first optimisation priority for
total task time. Measure uncached prompt throughput, cached tokens and cache
restore/rebuild time separately from model loading and generation. Compare
complete multi-turn tasks, including swaps, against the baseline. Set numerical
prefill and end-to-end latency budgets from those measurements before tuning.
These are acceptance targets, not achieved performance claims. Evidence from
another Qwen model or CPU-only tests does not establish Qwen3.8-27B GPU
stability or performance.

Include timeout-sensitive n8n workflows in the latency workload: measure complete
non-streaming responses, cancellation and retries against the configured client
and proxy timeouts. Keep host-memory use bounded for a 32 GB machine. Preserve
existing simultaneous-model support when models fit; dedicated dual-model tuning
is a V1.0.1 follow-up, not a requirement to remove that capability from V1.0.0.

### Build-out plan

These phases are planned work, in priority order. Each extends the existing
capabilities listed below. No delivery dates or new platform support are implied.

| Phase | Work | Completion criteria |
| --- | --- | --- |
| 0. Stabilise sustained load | Baseline and fix Qwen3.8-27B parallel serving on Windows/AMD using the test matrix above. Investigate context/slot budgets, cache rebuilds, prefill fairness and MTP transitions. | Pass the steady-load and burst/recovery checks with recorded capacity and agreed latency budgets before expanding workload intake. |
| 1. Explain performance | Add a request inspector with queue, load/swap, prompt-processing and generation timings. Show cached tokens, truncation and MTP fallback reasons. Improve the UX around the recent scheduling, caching and statistics fixes. | A slow request can be traced from the dashboard to recorded timings and a concrete cause. Unavailable measurements are labelled; request content is excluded from diagnostics by default. |
| 2. Connect clients and agents | Extend aliases, keys and priorities with guided setup for OpenCode, Open WebUI and Universal Agent Manager. Add an optional MCP generation adapter as described below. | Verify authentication, streaming and tool calls from a separate client machine. Agents can submit permitted media jobs, follow progress, cancel and retrieve their outputs without administrative credentials. |
| 3. Tune real workloads | Extend measured optimisation with representative coding, conversation and tool-use workloads. Compare first-token latency, p95 response time, prompt/generation throughput, quality and RAM/VRAM. | Compare baseline and candidate on identical workloads on Windows/AMD. Show the evidence before applying a profile, retain the previous configuration and reject regressions against the selected workload's limits. |
| 4. Share one GPU predictably | Add Interactive, Throughput and Background policies using the existing queue, priorities and background leases. Add bounded session-cache retention with visible memory budgets and expiry. | A mixed coding, voice and media run follows the selected policy without starvation or unbounded memory growth. Explain waits and eviction; cancel only at supported runtime boundaries. |
| 5. Plan model capacity | Extend the model store with a hardware fit planner for combinations of models, context sizes and slots. Include verification/finalisation stages in download progress and explain shared-artifact removal restrictions. | Distinguish estimated from measured fit, validate a proposed configuration with an isolated load check and provide a usable alternative when it exceeds available capacity. |
| 6. Operate and update safely | Show running, installed and available versions. Add a guided Windows update and rollback flow for matching executable/dashboard artifacts and compatible configuration/state. | Verify the selected service after restart and reboot. A failed activation offers a tested rollback; the dashboard reports the build actually serving requests. |
| 7. Develop media workflows | Extend existing image/music generation and history with reusable presets, duplicate-and-edit, comparisons and model-version provenance. Then add a native video runtime using the same job lifecycle. | Existing media jobs remain browsable and reproducible where the runtime permits. Video requires real Windows/AMD generation, bounded memory, progress, cancellation and playable output before being advertised. |
| 8. Broaden installation | Build supported macOS and Linux/Docker packages, hardware-aware onboarding and a first-result flow for each supported capability. | Each platform passes clean-install, restart, API and real-model checks on its own supported hardware/backend combinations. The UI clearly identifies unavailable capabilities and required model downloads. |

Sustained-load stability comes first. Request diagnostics and workload
benchmarks support that work; client setup and MCP then make the verified
capacity easier to use. Video, general fine-tuning and additional platforms
remain longer-term work; managed GGUF quantisation already has its own native
path.

### MCP generation tools

Plan an optional, separately named **InferDeck MCP adapter** for agents to
create media through the existing native runtimes. It consumes supported
InferDeck APIs and remains outside Core, following the
[protocol boundary](docs/adr/0001-openai-core-boundary.md). Core keeps ownership
of jobs, scheduling, model residency, cancellation and output storage.

Start with capability discovery, image/music job submission, job status,
cancellation and output retrieval. Advertise video generation only after a
working video runtime is available. Use Streamable HTTP for remote clients;
[Open WebUI supports this transport](https://docs.openwebui.com/features/extensibility/mcp/).
Verify Universal Agent Manager's actual client integration separately.

Long jobs should return a job identifier promptly and offer status/result tools.
Use optional [MCP Tasks](https://modelcontextprotocol.io/extensions/tasks/overview)
only when negotiated with a client that supports them. Define reconnect,
retention and interrupted-job behaviour explicitly. Add caller-scoped job APIs
where needed: generation clients can access only their authorised jobs and
artifacts, with bounded requests and outputs. They must not receive the
administrative token, arbitrary filesystem access or untrusted queue priorities.
The UX should identify which agent started a job and explain waits and failures.

### Alternative-engine research

Evaluate vLLM as a separate throughput experiment after establishing the
Qwen3.8-27B baseline. Automatic switching between llama.cpp for one request and
vLLM for multiple requests is an unproven design: engines need their own runtime
state and caches, and loading or rebuilding that state can outweigh throughput
gains. Compare one selected engine per model/deployment before attempting
switching under load. Include transition costs and memory residency in results.

[vLLM's official installation guide](https://docs.vllm.ai/en/stable/getting_started/installation/gpu/)
requires Linux and does not offer native Windows support. AMD lists the R9700 in
its [ROCm WSL compatibility matrix](https://rocm.docs.amd.com/projects/radeon-ryzen/en/docs-7.2/docs/compatibility/compatibilityrad/wsl/wsl_compatibility.html),
but that does not establish support or performance for this exact model,
quantisation and feature combination. Its
[GGUF path is experimental](https://docs.vllm.ai/en/stable/features/quantization/gguf/),
so the existing Q4_K_M artifact is not an assumed production configuration.

Any comparison must match workload and quality requirements, document differing
quantisation, and test tools, reasoning, required vision/MTP behaviour,
latency, throughput and sustained stability on the target GPU. Adopting a
separate Python/WSL engine would require an explicit architecture decision about
Core's native single-process boundary. No dual-engine routing is committed by
this roadmap.

### UX work alongside the backend

| Existing area | Planned UX improvement |
| --- | --- |
| Scheduling and cache reuse | Explain what a request is waiting for, whether its prompt cache was reused, and the memory cost of retaining it. |
| Optimisation | Compare current and proposed settings with their measured results. Explain a completed run with no useful improvement, and make apply/revert outcomes clear. |
| Usage and history | Use consistent totals and time windows, show data freshness, and explain when history recording needs attention. |
| Model downloads and removal | Distinguish downloading, checksum verification and finalisation. Make cancellation availability, recovery and shared-file restrictions visible. |
| Image and music jobs | Keep progress, cancellation, previews and errors coherent during reconnects. Build presets and comparisons on the existing saved parameters and outputs. |

Keep the dashboard compact, with a true black background, white primary text
and advanced controls available when needed. Avoid continuous decorative
animations that consume GPU time. Review distinct static mockups before
implementing substantial UI changes. A UX change is complete only after its
success, empty, loading, cancellation, failure and reconnect states work.

Each phase needs a reviewed implementation, passing required tests with their
expected counts, and relevant real Windows/AMD evidence. CPU tests establish
CPU behaviour; GPU performance claims require GPU measurements. Release and
live activation are separate steps, verified against the running artifacts.

### Capability milestones

**Hardening the core**
- [x] **Shared request queue** across text, embeddings, image, speech,
  transcription, and music generation. It supports priorities with ageing,
  cancellation, queue position reporting, and preparation across model swaps.
  It is in memory, not durable across gateway restarts.
- [x] **Recurrent-state checkpoints** for hybrid linear-attention models
  (e.g. Qwen3.6-A3B), so they get the same KV-cache reuse as full-attention
  models instead of re-prefilling every turn.
- [x] **Structured error codes and UTF-8 hold-back** in the streaming paths for
  clean multi-byte output and consistent API errors.
- [x] **Required CI on every push and pull request.** Architecture policy,
  dashboard/SDK contracts, and clean native build/security gates run separately
  from release packaging.

**Beyond text: the multimodal gateway**
- [x] **Speech-to-text** (`/v1/audio/transcriptions`, Parakeet TDT and
  whisper.cpp). Both native runtimes pass real-audio and pinned OpenAI SDK
  verification.
- [x] **Text-to-speech** (`/v1/audio/speech`, Supertonic 3). The native runtime
  passes real-model and pinned OpenAI SDK verification.
- [x] **Image generation API and adapter** (`/v1/images/generations`). The
  pinned stable-diffusion.cpp backend builds in-process with shared ggml/Vulkan,
  and the OpenAI endpoint passes real-model Windows/Vulkan validation. The
  dashboard includes generation, cancellation, preview, download, and
  persisted attempt history. Model weights remain a separate download under
  their own licences.
- [x] **Music generation API and adapter**
  (`/api/inferdeck/v1/audio/generations`). The pinned acestep.cpp backend,
  shared queue, cancellation, PCM16 WAVE output, and public verifier pass
  real-model Windows/Vulkan validation. The dashboard includes prompt, lyrics,
  duration, seed, advanced controls, playback, download, and persisted attempt
  history. Model weights remain a separate MIT licensed download.
- [ ] **Video generation** as local open-model pipelines mature, using
  long-running jobs with progress streamed over the existing SSE channel.
- [x] **Managed GGUF quantisation API.** The in-process llama.cpp path supports
  Q4_K_M, Q5_K_M, Q6_K, and Q8_0 output, refuses requantisation and loaded
  sources, stages output without overwrite, and records one background job.
- [ ] **Fine-tuning and dashboard controls.** The vendored experimental trainer
  can only perform full-model FP32 work and exposes no supported LoRA save API.
  This remains unavailable until an upstream-compatible adapter workflow can
  be implemented and verified without a subprocess.

**Expanding the engine**
- [x] **True parallel slots (continuous batching).** Decode multiple concurrent
  requests against one resident model in a single batched `llama_decode` loop
  (one shared context, `n_seq_max` sequences) instead of serialising them behind
  a per-model lock, turning the slot queue into real concurrency.
- [x] **OpenAI Responses API** (`/v1/responses`) for stateless text input,
  tools, reasoning, structured outputs, and typed streaming events. Vision is
  still rejected because no model can currently advertise vision support.
- [x] **Embeddings endpoint** (`/v1/embeddings`) for local RAG pipelines.
- [x] **Adaptive MTP decoding** for configured Qwen3.6 models at low
  concurrency, with ordinary continuous batching used outside the MTP window.
- [ ] **Draft-model speculative decoding.** Only MTP is implemented.
- [x] **Multi-model residency and execution** when fresh observed GPU headroom
  or declared footprints can fit more than one model. Independent resident
  runtimes can accept work together; model load and eviction remain serialized
  to protect the native backends.
- [x] **Measured profile optimisation** using the in-house search and dashboard
  benchmark flow. It measures quality, throughput, load time, and peak VRAM
  before staging the recommended configuration.

**Expanding the platform**
- [x] **Integrated model store** for Hugging Face discovery, verified downloads,
  cancellation/resume, registration, and safe removal on Windows.
- [ ] **Linux support.** The inference core is portable; the GPU telemetry
  layer (PDH/DXGI/ADLX) needs a sysfs/NVML equivalent.
- [x] **Managed client keys** with individual revocation and server-owned
  queue priorities, alongside the legacy shared token.
- [ ] **Multi-user controls.** Extend client identity with usage attribution,
  per-client limits and model-access policies.
- [ ] **macOS and Linux/Docker installation packages**, with platform-specific
  runtime, hardware and clean-install verification as described above.

Suggestions and issues are welcome. See [CONTRIBUTING.md](CONTRIBUTING.md).

## Documentation

| Doc | Contents |
| --- | --- |
| [`AGENTS.md`](AGENTS.md) | Engineering guide: build/test commands, architecture quick reference, concurrency invariants, design rules learned the hard way |
| [`docs/architecture.md`](docs/architecture.md) | Layer-by-layer architecture notes |
| [`docs/post-training.md`](docs/post-training.md) | Managed GGUF quantisation API, lifecycle, limits, and real-model verification |
| [`docs/DEPLOY.md`](docs/DEPLOY.md) | Windows service deployment, build identity, activation and rollback |
| [`docs/opencode-setup-guide.md`](docs/opencode-setup-guide.md) | Pointing opencode at InferDeck |
| [`CHANGELOG.MD`](CHANGELOG.MD) | Release history |

## Acknowledgements

InferDeck stands on [llama.cpp](https://github.com/ggml-org/llama.cpp) by
Georgi Gerganov and contributors, and
[stable-diffusion.cpp](https://github.com/leejet/stable-diffusion.cpp) by Lee Jet
and contributors, and
[acestep.cpp](https://github.com/ServeurpersoCom/acestep.cpp) by its
contributors. The parity gate exists precisely because matching upstream
quality is the bar.

## License

[MIT](LICENSE)
