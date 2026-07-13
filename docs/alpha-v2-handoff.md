# InferDeck Alpha V2 handoff

Branch: `alpha-v2` from `main`/`origin/main` at `798b2ef` (`v0.4.0`).

## Status by issue

- #52 runtime architecture: in progress. Added the shared `IBackend` lifecycle/capacity contract, runtime-keyed factories, runtime/modality/capability metadata, configuration parsing, and typed rejection of text execution on non-text backends.
- #53 image generation: pending native runtime integration.
- #54 text-to-speech: pending native runtime integration.
- #55 speech-to-text: pending native runtime integration.
- #56 resource-aware residency: pending.
- #57 model store: pending.
- #58 GUI configuration: pending.
- #59 cross-modality queue: pending.
- #60 embeddings: pending.
- #61 Responses API: pending.
- #62 capability discovery: pending.

## Verification run

- Portable model tests compiled directly with Apple Clang 21 and the repository's installed Catch2 libraries: 31 test cases, 129 assertions passed.
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
- The coordinator still has single-resident-model behavior until #56 is implemented.
