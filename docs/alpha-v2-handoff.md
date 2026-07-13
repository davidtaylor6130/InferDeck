# InferDeck Alpha V2 handoff

Branch: `alpha-v2` from `main`/`origin/main` at `798b2ef` (`v0.4.0`).

## Status by issue

- #52 runtime architecture: portable implementation complete. Added the shared `IBackend` lifecycle/capacity contract, runtime-keyed factories, runtime/modality/capability metadata, configuration parsing, and typed rejection of text execution on non-text backends.
- #53 image generation: pending native runtime integration.
- #54 text-to-speech: pending native runtime integration.
- #55 speech-to-text: pending native runtime integration.
- #56 resource-aware residency: portable implementation complete. Windows VRAM calibration and live multi-model validation remain.
- #57 model store: pending.
- #58 GUI configuration: pending.
- #59 cross-modality queue: portable implementation complete. Admission is bounded, priority-aware with aging, cancellable, visible in status, and spans tracked swaps.
- #60 embeddings: pending.
- #61 Responses API: pending.
- #62 capability discovery: portable implementation complete through `/v1/models`, swap/status, SSE stats, dashboard status, and the Models page.

## Verification run

- Portable model tests compiled directly with Apple Clang 21 and the repository's installed Catch2 libraries: 41 test cases, 184 assertions passed.
- OpenAI route tests: 15 test cases, 106 assertions passed. The binary requires unsandboxed loopback access for its local HTTP servers.
- Anthropic route tests: 9 test cases, 46 assertions passed with loopback access.
- Dashboard tests: 15 tests passed.
- Dashboard TypeScript check and production Vite build: passed; committed static output rebuilt.
- `main.cpp`, `llama_cpp_model.cpp`, dashboard routes, and GPU telemetry sources passed Apple Clang syntax/object compilation where link-free checks were possible.
- `git diff --check`: passed.
- Root CMake configure on macOS: unavailable. The project forces the Windows Vulkan loader and the local environment lacks the required SPIR-V package. This is not evidence of a Windows build failure.

## Windows validation still required

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DINFERDECK_BUILD_TESTS=ON
cmake --build build --target inferdeck-gateway --config Release -j
ctest --test-dir build -C Release --output-on-failure -L "unit|integration"
pnpm --filter dashboard test
pnpm --filter dashboard build
```

Do not deploy Alpha V2 artifacts to `C:\InferDeck` until the complete issue set has passed Windows/Vulkan and real-model validation.

## Required native runtime dependencies

- Existing llama.cpp Vulkan dependency for text and embeddings.
- Image, TTS, and STT runtime choices remain pending; each must expose a native in-process Windows API and must not require a CLI/server subprocess.

## Known risks and assumptions

- No Windows compilation, Vulkan execution, VRAM measurement, or real-model inference has been performed on this branch yet.
- Runtime identifiers default to `llama_cpp`, preserving existing YAML files.
- Multi-residency activates only after DXGI reports total VRAM or `gateway.vram_budget_mb` is configured. With no budget, legacy single-resident swap behavior remains.
- Automatic slot shrinking requires calibrated `vram_fixed_mb` and `vram_per_slot_mb` values per model. Without them, the planner can keep models resident when declared totals fit and can evict idle residents, but it will not guess unsafe slot savings.
