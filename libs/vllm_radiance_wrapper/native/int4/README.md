# R4D INT4 kernels

This is a source preservation copy of the private alpha64 probe implementation. Provenance is limited to that local probe tree; upstream authorship and licensing are uncertain. No license is asserted here. The alpha66 isolated InferDeck run rebuilt both DLLs from these files and exercised this overlay, but the build and measurement artifacts are ignored and not included in this source copy. This profile is experimental and is not deployed by default.

## Windows builds

Run from the repository root with ROCm 7.1 installed at the path below. Outputs go to the user temp directory, not this source tree:

```powershell
$hipcc = 'C:\Program Files\AMD\ROCm\7.1\bin\hipcc.exe'
$src = 'libs/vllm_radiance_wrapper/native/int4/r4d-feasibility/int4-fast/kernel'
& $hipcc -O3 -std=c++17 --offload-arch=gfx1201 -shared "$src/r4d_int4_tiled_prefill.hip" -o "$env:TEMP/r4d_int4_tiled.dll"
& $hipcc -O3 -std=c++17 --offload-arch=gfx1201 -shared "$src/r4d_int4_decode_wave32.hip" -o "$env:TEMP/r4d_int4_decode_wave32.dll"
```

The prefill command matches the recorded ROCm 7.1 Windows hipcc flags and source. The decode command uses the same compiler/link flags for the wave32 translation unit. Both target AMD gfx1201; a compatible ROCm HIP compiler/device library is required. They include HIP runtime headers/libraries supplied by ROCm and the adjacent copied R4D headers. No extra third-party native library is used.

## ABI and Python runtime

The prefill export is `r4d_int4_tiled_prefill_h256_gqa6(R4DInt4TiledArgs*, hipStream_t)`; the Win64 struct is 208 bytes with `softmax_scale` at offset 200. The decode export is `r4d_int4_decode_splitk(Int4DecodeArgs*, hipStream_t)` and uses its own ctypes struct declared in the overlay. Both use raw device pointers and the active HIP stream. The overlay loads DLLs through `ctypes` and requires caller-supplied expected SHA-256 values for each DLL; it reports the source/header hashes and pins the vLLM source revision to `bf87782b97a961f86ab3a709800ef1b4561d5cda`. Do not use a DLL whose hash was not independently recorded and reviewed.

Using the overlay additionally requires a Python environment with PyTorch built for ROCm, the matching vLLM checkout and its `vllm.v1.attention.ops.int4_per_token_head` module, and an AMD HIP device. InferDeck loads it only when the `vllm_radiance` model's `prefill_overlay` artifact selects this file alongside the `r4d_int4` profile and hash-pinned prefill/decode DLLs.

The local alpha66 evidence is `.UAM/AINotes/alpha66-int4layout-smoke-20260924T011354761Z.json` and the matching staged logs/stats. Four simultaneous short requests and the two-agent 64K+2K tool loop passed; the loop took 151.354 s versus the BF16/R4D 133.684 s baseline median, so this is not a speed win. Four near-100K contexts and long-term reliability remain unverified. No DLLs, weights, logs, or result files are part of this copy.
