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
    prefill_attention: r4d
    prefill_overlay: C:/private/r4d_strided_prefill_overlay.py
    prefill_dll: C:/private/r4d_strided_attn.dll
    prefill_dll_sha256: <64-character SHA-256>
```

`artifacts.prefill_attention` accepts `r4d` (the backward-compatible default) or `upstream`. `r4d` installs the custom prefill overlay and requires its three `prefill_*` artifact fields. `upstream` uses the pinned vLLM Triton attention implementation and does not require `prefill_overlay`, `prefill_dll`, or `prefill_dll_sha256`. Both retain the same Radiance model and decode kernels. Invalid values are rejected during configuration validation and registry admission. An unexpected attention binding fails loading instead of silently selecting another path.

Selection takes effect when the model is loaded; it is not a per-request switch. Use the existing drain/unload/load lifecycle and expect cache invalidation. Successful startup logs the selected prefill implementation. The upstream option is experimental: scoped greeting and tool checks passed in standalone tests, but it was slower than custom prefill in a historical two-agent long-context workload. Those measurements do not establish current gateway performance or full quality acceptance. Subsequent isolated gateway checks passed the captured greeting, seven API checks and two long-context tool tasks. Both long continuations missed reuse at the one-second grace boundary, so cached-turn performance acceptance remains unmet.

`continuation_grace_ms` defaults to zero and accepts 0–2000. On an enabled native single-slot model, an existing client cache key can reserve one continuation before yielding to waiting peers. The measured reuse path is Chat Completions; do not assume equivalent Responses cache behavior. On the frozen two-agent 100K+4K Qwen3.8 task, the 1000 ms hold missed both continuations (308.756 s), while the 2000 ms hold reused 100,352 tokens per continuation and completed in 163.173 s at the same measured 1.0 s tool delay. Two identical alpha52 runs passed in 163.173 and 163.378 s with both cached continuations. A third identical startup failed before any request with a native tokenizer deserialization error; repeated-start reliability therefore fails and this profile is not ready for deployment.

`request_queue_timeout_seconds` defaults to 300 and accepts 1–1800. The experimental four-agent profile uses 600. This extends queue patience; it does not create additional execution slots or context capacity.

## Validation and packaging

The four-slot `r4d_int4` MTP profile uses vLLM compilation mode 3 with AOT compilation disabled, native decode through M=16, and decode graphs for one and four speculative steps. MTP3 defaults to capture sizes `[4,16]`; explicit artifact overrides still take precedence. Deployment must also carry the validated batched prefill DLL and its matching SHA-256. A matching executable version alone does not identify the Python profile or native kernels.

For the pinned four-slot MTP3 profile, memory admission adjusts the requested utilization when existing GPU allocations prevent the 0.991 budget. It retains 192 cache blocks, the measured minimum for four 100K contexts, only when the adjusted budget remains at least 0.97. Startup continues to require measured capacity for all four contexts and at least 1 GiB free after initialization. Larger competing allocations are rejected rather than reducing context capacity.

The optional `compiler_cache_root` artifact places vLLM, Inductor, Triton, and compiler temporary output beneath the selected directory. Use a volume with enough free space for cold compilation; a Windows LocalSystem service uses a different default cache from an interactive benchmark account.

The optional `hillclimb_sampling: "true"` artifact pins the HC82 sampling settings for every request: temperature 0, top-p 1, top-k 20, min-p 0, repetition penalty 1, and frequency/presence penalties 0. It overrides client sampling values for that model while retaining request reasoning, seed, structured output, and output length. Models without this artifact continue to honor client sampling values.

The INT4 attention overlay detects the optional `r4d_int4_verify_splitk` export in the hash-pinned decode DLL. This kernel handles two to four query tokens for a single sequence, partitions long-context attention across the GPU, and shares K/V reads between MTP verification queries. Older DLLs and multi-sequence batches retain the prefill path. Attention parity, CUDA graph replay, and actual request throughput must be checked separately; the HC82 generation workload used a 214-token prompt and does not establish throughput for long coding histories.

Build the verification DLL for the tested RDNA4 target from the repository root:

```powershell
New-Item -ItemType Directory -Force build/kernels | Out-Null
& 'C:/Program Files/AMD/ROCm/7.1/bin/hipcc.exe' -O3 -std=c++17 --offload-arch=gfx1201 -shared libs/vllm_radiance_wrapper/native/int4/r4d-feasibility/int4-fast/kernel/r4d_int4_decode_wave32_verify.hip -o build/kernels/r4d_int4_decode_wave32_verify.dll
Get-FileHash build/kernels/r4d_int4_decode_wave32_verify.dll -Algorithm SHA256
```

Deploy the rebuilt DLL together with the updated overlay, then set the model's `decode_dll` and `decode_dll_sha256` artifacts to that file and its actual hash. Keep the validated batched prefill DLL. The optional `hillclimb_sampling` flag enables the greedy profile; setting model defaults alone does not override an explicit client temperature. The DLL is an external runtime artifact and is not rebuilt by the gateway CMake target.

Single-token decoding shares the packed K/V loads across three Qwen attention heads. The 24-query-head, four-KV-head geometry uses this path; other geometries retain the original decode kernel. MTP verification keeps one head per wave. Both paths share the split-combiner softmax weights across output dimensions, keeping the original accumulation order. Explicit fused operations preserve the original single-token accumulation rounding with the pinned HIP compiler. The DLL exports and overlay ABI are unchanged.

On an AMD R9700, interleaved captured single-token attention calls at the coding workload's 32K/50K contexts took 0.465/0.592 ms versus 0.601/0.765 ms before this change; at 100K they took 0.896 versus 1.233 ms. Four-query MTP attention took 0.986/2.679 ms versus 1.022/2.736 ms at 32K/100K. These are attention timings, not whole-model TPS. Single-request MTP uses the grouped-head path in the drafter, so its overall gain will be smaller. Full-model gains require validation after activation.

The focused GPU check compares causal attention against vLLM for one to four queries, short/32K/50K/100K contexts, reversed physical page mappings, and CUDA graph replay. Supplying the previous decode DLL also requires bit-identical output for all cases. It requires the pinned runtime dependency tree:

```powershell
python libs/vllm_radiance_wrapper/tests/verify_int4_mtp_attention.py --prefill-dll <batched-prefill.dll> --decode-dll build/kernels/r4d_int4_decode_wave32_verify.dll --runtime-root build/runtime-radiance-probe
```

Add `--baseline-decode-dll <previous-decode.dll>` to check exact parity with the previously deployed kernel.

Before a drained Radiance engine shuts down, cleanup unregisters compiler bytecode hooks for both target and MTP draft models. The pinned vLLM finalizer covers only the target; the draft hooks otherwise retain its embedding, output, and transformer weights. After release, cleanup resets PyTorch's in-process compiler state before emptying the device allocator. This reset preserves the compiler filesystem caches.

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

## Windows GPU memory cache

Before loading HIP dependencies, the native profile defaults `GPU_RESOURCE_CACHE_SIZE` to `64` MiB. AMD PAL otherwise sizes its freed-resource cache from GPU capacity; this cache can retain memory after Torch reports no live allocations. An isolated full-model load/unload check reduced remaining dedicated GPU allocation from 3.28 GB to 0.76 GB with this limit. That measurement does not establish request-latency improvement or complete memory release.

An existing environment value is preserved. PAL reads this setting during device initialization, so changing it requires a gateway process restart; switching models in an already initialized process is insufficient. It controls the HIP resource cache, not model context, KV-cache capacity, or sampling. Validate both native requests and the Vulkan fallback before promoting a changed profile.
