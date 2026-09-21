# Experimental native vLLM/Radiance runtime

This branch contains an opt-in Windows experiment, not a production-ready release or a proven 2,000-PP profile. The default build continues to use llama.cpp/Vulkan.

## Architecture and limitations

`vllm_radiance` embeds CPython 3.12 and vLLM V1 `InprocClient` in the gateway. The existing coordinator owns admission, cancellation and model switching. No inference server or Python worker is launched. Triton may launch short-lived compiler and toolchain-discovery processes during cold-cache compilation, so this runtime is not strictly subprocess-free.

The profile is pinned to Qwen3.8-27B Quark AWQ MXFP4, 106496 total context tokens, one sequence, 4096 batched tokens, 90% GPU memory utilization, `TRITON_ATTN`, and decode graph capture. Reserve output space within the total context. It exposes text capabilities only and rejects incompatible settings or unavailable dependencies rather than silently falling back.

Runtime changes invalidate model cache state. Keep client cache keys stable within a loaded runtime; caches are not portable across engines.

## Build and dependencies

Provide an x64 CPython 3.12 installation with `include/Python.h`, `libs/python312.lib`, `python312.dll`, its standard library and DLL directory:

```powershell
cmake -S . -B build -DINFERDECK_VLLM_RADIANCE_ENABLE=ON -DINFERDECK_PYTHON312_ROOT=C:/private/python312
cmake --build build --target inferdeck-gateway --config Release -j
```

The build packages four Python bridge modules and `python312.dll`. Model weights, ROCm packages, vLLM/Radiance source, overlays and compiled kernels remain external. This repository does not provide a complete fresh-install bundle or redistribute those dependencies.

The tested vLLM source revision is `bf87782b97a961f86ab3a709800ef1b4561d5cda`. The bridge enforces Radiance extension SHA-256 `64124749ed12f72c3d13544b313dcdcff33582e3bd7e001b519a2c5bb1f2ed4d` and the configured prefill DLL digest. Different binaries require separate validation.

Cold service-account startup uses eager Qwen class registration and selects the packaged compiler at `python_site/_rocm_sdk_core/lib/llvm/bin/clang-cl.exe`. Existing Visual Studio C++ tools, Windows SDK and Python headers/import library are required for Triton compilation. No system environment changes are needed.

## Configuration

Use an explicit model entry; existing aliases and default models remain unchanged. The loader prefers a valid `gateway.active.yml` when present, with `gateway.yml` as the base fallback. Preserve both when staging or restoring configuration.

```yaml
- name: qwen3.8-27b-radiance
  family: qwen3.8
  runtime: vllm_radiance
  modality: text
  capabilities: [chat_completions, responses]
  compute: rocm_gpu
  context_size: 106496
  n_slots: 1
  min_slots: 1
  has_vision: false
  artifacts:
    model: C:/private/models/Qwen3.8-27B-Quark-AWQ-MXFP4
    python_root: C:/private/python312
    python_site: C:/private/venv/Lib/site-packages
    vllm_source: C:/private/vLLM_for_AMD
    radiance_source: C:/private/radiance-src
    radiance_extension: C:/private/radiance-extension
    rocm: C:/private/venv/Lib/site-packages/_rocm_sdk_devel
    selector_overlay: C:/private/radiance_selector_overlay.py
    pread_overlay: C:/private/pread_safetensors_overlay.py
    prefill_overlay: C:/private/r4d_strided_prefill_overlay.py
    prefill_dll: C:/private/r4d_strided_attn.dll
    prefill_dll_sha256: <64-character SHA-256>
```

`continuation_grace_ms` defaults to zero and accepts 0–1000. On an enabled native single-slot model, an existing client cache key can reserve one continuation before yielding to waiting peers. The measured reuse path is Chat Completions; do not assume equivalent Responses cache behavior.

`request_queue_timeout_seconds` defaults to 300 and accepts 1–1800. The experimental four-agent profile uses 600. This extends queue patience; it does not create additional execution slots or context capacity.

## Validation and packaging

Executed local checks include 148 C++ unit/integration targets, 12 Python bridge tests, 58 benchmark-harness tests, and seven real-model API checks covering structured output, tools, streaming arguments, Responses and cancellation. Native loading and Vulkan recovery succeeded under LocalSystem. An earlier build completed 20 switches and 155 mixed requests; that endurance run was not repeated after the service-start fixes.

Python integration tests require the private pinned dependency tree and fixtures under `build/runtime-radiance-probe` and `build/perf`; they are not a dependency-free fresh-checkout suite. Raw hardware evidence and local deployment records are intentionally not published here.

Long-context quality, fully matched multi-agent elapsed time, final-build endurance, broader modality parity and the 2,000 uncached PP target remain incomplete. Historical timings used different boundaries or output lengths and do not establish a final speedup.

`Stage-NativeRuntime.ps1` creates a new package under `build/private-releases`, excluding generated Python bytecode. It records the executable's revision, dirty state and artifact hashes. Configuration is copied only when explicitly supplied; include any active override separately.

`Test-NativeRuntimeRelease.ps1` accepts `-CandidateManifest` and `-Acceptance`. Supply a local acceptance JSON containing `gates` with `id` and `status` fields. Statuses are `PENDING`, `IN_PROGRESS`, `PASS`, `FAIL` or `BLOCKED`. Missing evidence/configuration returns a non-pass result; unresolved gates return exit 2. The default goal-state path is local and is not distributed.

Before activation, back up the matching executable, DLLs, Python modules, static assets and configuration. Stop only the intended service, replace the package consistently, restart and verify an actual request. Restore the complete backup on failure. Selecting the original Vulkan model remains the normal runtime fallback.

## Sampling compatibility

The native adapter must resolve omitted temperature, top-p, top-k, min-p and repetition settings from the model configuration, with explicit request values taking precedence. A disabled top-k is represented as `-1` in vLLM. Native sampling diagnostics record effective values, not prompt text.

Penalty implementations have different history semantics. The native runtime uses a request-local logits processor for repetition, frequency and presence penalties over the requested generated-token window. Zero disables penalties; positive values retain that many recent output tokens; `-1` retains all output tokens. Native built-in penalties are disabled to avoid double application. Unsupported samplers such as DRY remain explicitly rejected. Matching supported settings does not imply identical sampled text across different kernels and quantizations.

For a private request replay, an operator can place capture-next-request.json beside the deployed Python profile with an expires_at Unix timestamp no more than ten minutes ahead. The next admitted native request consumes the marker and writes captured-request.json exclusively in that directory, including normalized messages, tools, settings and rendered token IDs. Existing captures are never overwritten. Capture is disabled without the marker; expired markers do not capture. Treat the resulting file as private conversation data and keep it out of commits and release packages. This diagnostic does not change sampling or establish output quality.
