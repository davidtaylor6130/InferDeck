# Providers

A provider is the code that runs a model: llama.cpp for text, stable-diffusion.cpp
for images, whisper.cpp for transcription, and so on. The gateway asks a provider
to load a model, run requests, report its state, and unload. It never calls into
a provider's internals.

This page is written so a person or a coding agent can add a runtime, or upgrade
one (for example a newer llama.cpp for a new model family), without reading the
rest of the codebase. Follow the recipes in order; every command is copy-paste.

## Contents

1. [Concepts](#concepts)
2. [Building](#building)
3. [Recipe A: add a new runtime as a provider DLL](#recipe-a-add-a-new-runtime-as-a-provider-dll)
4. [Recipe B: upgrade a runtime by dropping in a new DLL](#recipe-b-upgrade-a-runtime-by-dropping-in-a-new-dll)
5. [Recipe C: check a provider](#recipe-c-check-a-provider)
6. [Installing, verifying, and rolling back](#installing-verifying-and-rolling-back)
7. [ABI v2 reference](#abi-v2-reference)
8. [Moving a vendored runtime pin](#moving-a-vendored-runtime-pin)
9. [Limits and known gaps](#limits-and-known-gaps)

## Concepts

The gateway executable contains no runtime. Every runtime, for every
modality, is a provider DLL loaded from `providers/` next to
`inferdeck-gateway.exe`:

| Runtime | DLL | Modalities |
|---|---|---|
| `llama_cpp` | `llama_cpp.provider.dll` | text (chat, tools, reasoning, vision via mmproj, MTP and DFlash/DFlash2 speculative decoding), embeddings; also GGUF quantisation and GPU device listing |
| `jev_style` | `jev_style.provider.dll` | decisions |
| `stable_diffusion_cpp` | `stable_diffusion_cpp.provider.dll` | images (SD 1.5, SDXL, Qwen-Image, edits) |
| `ltx_video_cpp` | `ltx_video_cpp.provider.dll` | video |
| `ace_step_cpp` | `ace_step_cpp.provider.dll` | music |
| `whisper_cpp` | `whisper_cpp.provider.dll` | transcription |
| `sherpa_onnx` | `sherpa_onnx.provider.dll` (+ `sherpa-onnx-c-api.dll`, `onnxruntime*.dll`, `libmp3lame.DLL`) | speech, transcription |
| `windows_sapi` | `windows_sapi.provider.dll` | speech |
| `vllm_radiance` | `vllm_radiance.provider.dll` (+ `python312.dll`) | text (embedded CPython vLLM/Radiance) |

Each ggml runtime links its own static llama.cpp/ggml, so any one DLL can be
rebuilt from a newer upstream and swapped without touching the gateway or the
other runtimes. A runtime whose DLL is missing is reported as
`not_installed` by `GET /api/inferdeck/v1/providers`, and its models report
`runtime_available: false`.

Inside the gateway each DLL becomes a `ProviderDescriptor`
(`libs/model/include/model/provider.hpp`): a runtime name, its contract, and a
factory that creates one backend per configured model. `PluginBackend`
(`libs/provider/src/plugin_provider.cpp`) implements every backend interface
the coordinator uses, so the scheduler sees exactly what it saw when runtimes
were compiled in.

Each model in `gateway.yml` selects its runtime by name:

```yaml
model_registry:
  - name: "my-model"
    runtime: "llama_cpp"        # the provider that runs this model
```

When two DLLs declare the same runtime name, the later file name (sorted)
wins and the log says `provider_overridden`. To run two builds side by side,
give one its own name (`llama_cpp_next`) and point individual models at it.

Pieces you will touch:

| Path | What it is |
|---|---|
| `libs/provider/include/inferdeck/provider_abi.h` | The C ABI. The whole contract, plain C. |
| `libs/provider/include/provider/sdk.hpp` | C++ SDK: an `IBackend` factory becomes a DLL in a few lines. |
| `libs/provider/src/codec.cpp` | The JSON + blob wire format for every request and result. Compiled into the gateway and every SDK DLL. Authoritative for field names. |
| `libs/provider/examples/echo_provider_v2.cpp` | Reference SDK provider. Copy it to start. |
| `libs/provider/examples/echo_provider.cpp` | Raw-C v1 provider, kept for compatibility tests. |
| `libs/provider/runtimes/` | The entry point of every InferDeck runtime DLL, the CMake that builds them, and the native-API stubs used by tests. |
| `libs/provider/tools/provider_check.cpp` | `inferdeck-provider-check`, the validator. |
| `libs/model/include/model/imodel.hpp`, `ibackend.hpp` | The C++ interfaces ABI v2 mirrors. |

## Building

One build produces the gateway and every runtime DLL:

```bash
export TEMP='F:\InferDeck-build\tmp' TMP='F:\InferDeck-build\tmp'
BUILD=F:/InferDeck-build/qwen4
cmake -S . -B $BUILD -G "Visual Studio 18 2026" -A x64 -DINFERDECK_BUILD_TESTS=ON
cmake --build $BUILD --config Release -j 8 --target inferdeck-gateway
```

Building `inferdeck-gateway` builds the `inferdeck-runtime-providers` target
too. The result is ready to copy:

```
$BUILD/bin/Release/inferdeck-gateway.exe   + spdlog.dll, fmt.dll, ...  (core only)
$BUILD/bin/Release/static/                 dashboard
$BUILD/bin/Release/python/                 Radiance's Python modules
$BUILD/bin/Release/providers/*.provider.dll + the libraries they import
```

The root `CMakeLists.txt` forces `BUILD_SHARED_LIBS=OFF`: every runtime links
llama.cpp/ggml statically into its own DLL. Windows reuses an already-loaded
module with the same base name, so a shared `ggml.dll` could never be
upgraded per runtime.

## Recipe A: add a new runtime as a provider DLL

Use the C++ SDK unless you are writing in another language (then implement the
C table in [ABI v2 reference](#abi-v2-reference) directly).

Shell setup used by every command below (Git Bash on Windows; set TEMP
explicitly because some shells inherit a broken one):

```bash
export TEMP='F:\InferDeck-build\tmp' TMP='F:\InferDeck-build\tmp'
BUILD=F:/InferDeck-build/qwen4        # the main build directory
```

1. **Pick a runtime name.** Lowercase letters, digits, underscores, e.g.
   `my_runtime`. The DLL file must be named `my_runtime.provider.dll`; the
   gateway ignores every other file in `providers/`.

2. **Implement the backend.** Write a class that derives from
   `model::IBackend` (use `model::FixedBackend` for fixed slots) plus the
   interface for each modality it serves:

   | Modality | Interface (`imodel.hpp`) | Methods |
   |---|---|---|
   | `text` | `IModel` | `predict`, `predict_cancellable`, `predict_stream`, optional `estimate_request_demand`, `ensure_request_capacity`, `chat_template_meta` |
   | `embedding` | `IEmbeddingBackend` | `embed` |
   | `image` | `IImageBackend` | `generate_images` |
   | `audio_generation` | `IAudioGenerationBackend` | `generate_audio` |
   | `video` | `IVideoBackend` | `generate_video` |
   | `audio_speech` | `ISpeechBackend` | `validate_speech_request`, `synthesize` |
   | `audio_transcription` | `ITranscriptionBackend` | `transcribe` |
   | `decision` | `IDecisionBackend` | `decide` |

   The meaning of every method is in [What each operation must do](#what-each-operation-must-do).
   Override the `IBackend` capacity methods (`estimate_vram_mb`,
   `additional_vram_reserve_mb`, `live_vram_accounting_complete`,
   `resize_slots`, `reclaim_idle_context`, ...) only if your runtime can
   answer them; the defaults are correct for a fixed-size runtime.

3. **Add the entry point.** One file, next to your backend:

   ```cpp
   #include "provider/sdk.hpp"

   namespace {
   inferdeck::provider::sdk::Definition define()
   {
       return {R"({
                   "runtime": "my_runtime",
                   "version": "1.0.0",
                   "modalities": {"image": ["image_generation"]},
                   "model_source": "artifacts"
               })",
               [](const inferdeck::model::ModelInfo& info, const nlohmann::json& gateway)
               {
                   return make_my_backend(info);
               }};
   }
   }

   INFERDECK_PROVIDER_SDK(define)
   ```

   `Definition` also takes optional `init(host_json)` and `shutdown()`
   callbacks (see `libs/provider/runtimes/llama_cpp_provider.cpp`). For one of
   InferDeck's standard runtime names, use
   `sdk::contract_manifest("<runtime>", version)` instead of writing the
   manifest by hand. The SDK forwards every ABI operation to the backend method
   of the same name, catches every exception, serialises streaming events, and
   routes `foundation::LOG_*` into the gateway log.

4. **Add the CMake target.** In `libs/provider/runtimes/CMakeLists.txt`:

   ```cmake
   inferdeck_add_provider(inferdeck-provider-my_runtime RUNTIME my_runtime
       SOURCES my_runtime_provider.cpp my_backend.cpp
       LIBRARIES <your runtime's libraries>)
   ```

   This produces `${BUILD}/bin/Release/providers/my_runtime.provider.dll`. If
   your runtime uses ggml (llama.cpp, stable-diffusion.cpp, whisper.cpp, ...)
   link it **statically**, as every target in this build does: Windows reuses
   an already-loaded module with the same base name, so a `ggml.dll` shipped
   next to your DLL would silently resolve to another runtime's copy. Copy any
   non-ggml DLL your runtime imports next to the provider with a `POST_BUILD`
   step (see `sherpa_onnx` in `libs/provider/runtimes/CMakeLists.txt`).

5. **Add a test.** In the same CMake file:

   ```cmake
   inferdeck_provider_check_test(provider_dll_my_runtime inferdeck-provider-my_runtime
       --artifact weights=<a file that exists>)
   ```

   To test without a GPU, build a second target from the same backend source
   with a stub of the native C API, the way `stubs/stable_diffusion_stub.cpp`
   and `stubs/whisper_stub.cpp` do. The stub test proves the adapter and the
   backend's own logic; the real GPU run in step 7 proves the native library.

6. **Build and run the checks.**

   ```bash
   cmake -S . -B $BUILD
   cmake --build $BUILD --config Release -j --target inferdeck-provider-my_runtime inferdeck-provider-check
   ctest --test-dir $BUILD -C Release -R provider_dll_my_runtime --output-on-failure
   ```

7. **Run it for real** with [Installing, verifying, and rolling back](#installing-verifying-and-rolling-back),
   on a test gateway with a spare port, never on the live service.

## Recipe B: upgrade a runtime by dropping in a new DLL

Example: Qwen4 needs a newer llama.cpp than the installed
`llama_cpp.provider.dll` was built with. Build a new `llama_cpp.provider.dll`
from the newer llama.cpp and replace the old one in `providers/`. The gateway
and every other runtime stay as they are; putting the old DLL back rolls back.

1. **Move the runtime.** In a separate worktree or branch, check out the new
   revision of the vendored runtime:

   ```bash
   git -C libs/third_party/llama.cpp fetch origin
   git -C libs/third_party/llama.cpp checkout <new-revision>
   ```

   Record the old and new revisions. If `llama_cpp_wrapper` no longer compiles
   against the new API, fix it in this worktree; the fix ships inside the DLL.

2. **Build only that runtime's DLL** in the worktree's build directory:

   ```bash
   cmake -S . -B $BUILD -G "Visual Studio 18 2026" -A x64
   cmake --build $BUILD --config Release -j 8 --target inferdeck-provider-llama_cpp
   ```

   Targets: `inferdeck-provider-llama_cpp`, `inferdeck-provider-jev_style`,
   `inferdeck-provider-stable_diffusion_cpp`, `inferdeck-provider-ltx_video_cpp`,
   `inferdeck-provider-ace_step_cpp`, `inferdeck-provider-whisper_cpp`,
   `inferdeck-provider-sherpa_onnx`, `inferdeck-provider-windows_sapi`,
   `inferdeck-provider-vllm_radiance`, or `inferdeck-runtime-providers` for all.
   Output: `$BUILD/bin/Release/providers/<runtime>.provider.dll`. The first
   build compiles llama.cpp and the Vulkan shaders; later builds are
   incremental.

3. **Keep the old DLL** for rollback: copy the running gateway's
   `providers/llama_cpp.provider.dll` to `llama_cpp.provider.dll.previous`
   (any name not ending in `.provider.dll` is ignored by the loader).

4. **Check that ggml is private.** The DLL must not import `ggml*.dll` or
   `llama*.dll`:

   ```bash
   dumpbin /dependents $BUILD/bin/Release/providers/llama_cpp.provider.dll
   ```

   Expected: Windows system DLLs, `vulkan-1.dll`, the MSVC runtime,
   `spdlog.dll`, `fmt.dll`. Nothing named `ggml` or `llama`.

5. **Check it without loading weights** (loads the library, runs `init`,
   creates an instance per modality):

   ```bash
   $BUILD/bin/Release/inferdeck-provider-check $BUILD/bin/Release/providers/llama_cpp.provider.dll --no-load
   ```

6. **Check it with the model**, where GPU work is allowed:

   ```bash
   $BUILD/bin/Release/inferdeck-provider-check $BUILD/bin/Release/providers/llama_cpp.provider.dll \
     --model C:/models/<model>.gguf --modality text
   ```

7. **Install it** next to a test gateway, run one real request per affected
   modality, then roll it out: [Installing, verifying, and rolling back](#installing-verifying-and-rolling-back).

To move the pin for every runtime at once, follow
[Moving a vendored runtime pin](#moving-a-vendored-runtime-pin).

## Recipe C: check a provider

`inferdeck-provider-check` loads a library exactly as the gateway does and, for
every modality in its manifest, runs `init`, creates an instance, checks that a
slot is refused before load, loads, checks the status queries and slot
accounting, sends one small request, checks cancellation, releases the slot,
unloads, and loads again.

```bash
$BUILD/bin/Release/inferdeck-provider-check <library> [--model <path>] [--artifact key=value]... \
    [--modality <name>]... [--skip-invoke] [--no-load] [--context <tokens>]     [--speculative mtp|dflash|dflash2] [--draft-model <path>] [--draft-tokens <n>]
```

| Option | Meaning |
|---|---|
| `--model <path>` | Sets the model's `gguf_path` (`model_path` in JSON). |
| `--artifact key=value` | Adds one `artifacts` entry. Repeatable. |
| `--modality <name>` | Check only this modality. Repeatable. |
| `--skip-invoke` | Lifecycle and slots only; send no request. Use for slow media models. |
| `--no-load` | Stop after `init` and `create`. Use where weights or a GPU are unavailable. |
| `--context <tokens>` | Sets `context_size` (default 65536). |
| `--speculative <type>`, `--draft-model <path>`, `--draft-tokens <n>` | Sets the model's speculative decoding (`speculative_type`, `draft_model_path`, `mtp_draft_tokens`). |

What each request checks:

| Modality | Checks |
|---|---|
| `text` | `predict` succeeds; streamed content and reasoning add up to the final result; a callback returning false ends the stream with `Cancelled`; `estimate_request_demand` succeeds. |
| `embedding` | One non-empty vector for one input. |
| `decision` | Two probabilities summing to 1 for a predicate. |
| `image`, `audio_generation`, `video`, `audio_transcription` | Non-empty output; when the provider reported progress, a progress callback returning false ends the call with `Cancelled`. |
| `audio_speech` | `validate_speech_request` accepts a plain request; synthesis returns audio. |

Exit code 0 means every line was `PASS`. The provider's own error text follows
each `FAIL`. Adapter regression tests live in
`libs/provider/tests/test_plugin_provider.cpp`.

Proven in `ctest -L unit` without a GPU:

| ctest name | Library | Runtime code |
|---|---|---|
| `provider_tests`, `provider_check_echo` | `echo.provider.dll` | SDK reference: chat, stream, tools, reasoning, logprobs, request progress, vision blobs, embeddings, decisions, status queries, resize, cancel, rejected options |
| `provider_check_echo_v1` | `echo_v1.provider.dll` | raw v1 table |
| `provider_dll_windows_sapi` | `windows_sapi.provider.dll` | real Windows SAPI speech (CPU) |
| `provider_dll_vllm_radiance` | `vllm_radiance.provider.dll` | real Radiance; `init` starts CPython; `--no-load` |
| `provider_dll_stable_diffusion_cpp` | stub build | real backend source, stubbed stable-diffusion.cpp |
| `provider_dll_ltx_video_cpp` | stub build | real backend source, stubbed stable-diffusion.cpp video |
| `provider_dll_ace_step_cpp` | stub build | real backend source, stubbed ACE-Step pipeline |
| `provider_dll_whisper_cpp` | stub build | real backend source, stubbed whisper.cpp |

Validated on the GPU (2026-11-17, AMD Radeon AI PRO R9700, Vulkan) through a
test gateway running only from `bin/Release` + `providers/`, every runtime
reported with its DLL as `origin`:

| Modality | Model (runtime DLL) | Result |
|---|---|---|
| chat, stream, tools, reasoning, logprobs, vision | qwen3.5-4b, Qwen3.8-27B IQ4_XS + MTP at 32k (`llama_cpp`) | all pass; 27B: 58.6 t/s decode, 556 t/s prefill, TTFT 1.4 s (`bench.mjs C`, 784-token prompt) |
| embeddings | qwen2.5-0.5b (`llama_cpp`) | 896-d vectors, cos(cat, kitten) 0.74 > cos(cat, GPU) 0.45 |
| decisions | Jev-Style 0.8B v3 (`jev_style`) | predicate 0.97, choice billing 0.99 |
| image | SD 1.5 512², SDXL 1024² (`stable_diffusion_cpp`) | 14 s and 41 s via `Testing/Test-ImageGeneration.ps1` |
| music | ACE-Step 1.5 turbo q4 (`ace_step_cpp`) | 10 s stereo 48 kHz WAV in 41 s |
| video | LTX-2.3 22B q4, 512x320, 9 frames (`ltx_video_cpp`) | MP4 in 109 s including load |
| speech | Supertonic 3 (`sherpa_onnx`), Windows SAPI (`windows_sapi`) | WAV; voice validation answered by the DLL |
| transcription | Parakeet TDT 0.6B (`sherpa_onnx`), whisper base.en (`whisper_cpp`) | exact text; whisper segments carry tokens, avg_logprob, no_speech_prob |

## Installing, verifying, and rolling back

1. A full install is `inferdeck-gateway.exe`, the DLLs beside it, `static/`,
   `python/` and `providers/` from `$BUILD/bin/Release`, all from the same
   build. To swap one runtime, copy only its DLL (and any library it imports)
   into `providers/`:

   ```bash
   mkdir -p <gateway dir>/providers
   cp $BUILD/bin/Release/providers/llama_cpp.provider.dll <gateway dir>/providers/
   ```

   For a test gateway, point it at a staging folder instead of touching the
   executable's folder:
   `inferdeck-gateway.exe -c config/gateway.test.yml --providers F:/staging/providers`.

2. Restart the gateway. Providers load once, at process start.

3. Verify:
   - Log: `provider_registered runtime=llama_cpp origin=<path> version=<v>`.
     Rejected libraries log `provider_plugin_rejected <path>: <reason>`.
   - `GET /api/inferdeck/v1/providers`: each provider's `runtime`, `origin`,
     `version`, `modalities`, `state` (`active`, `overridden`, `not_installed`,
     `init_failed` with `error`), plus `rejected_plugins`. `origin` is the DLL
     path.
   - `GET /api/inferdeck/v1/models`: `runtime` and `runtime_available` per model.
   - One real request per modality the DLL serves.

4. Roll back: rename `llama_cpp.provider.dll.previous` back to
   `llama_cpp.provider.dll` and restart. Deleting a runtime's DLL leaves its
   models unavailable (`not_installed`), not served by some fallback.

On the live Windows service the folder is `C:\InferDeck\providers`; restarting
the `InferDeck` service needs the owner's authorization (see `AGENTS.md`,
Deployment).

**Security.** A provider runs inside the gateway process with the gateway's
privileges. Only administrators should be able to write to `providers/`; only
load libraries you trust.

## ABI v2 reference

The header `libs/provider/include/inferdeck/provider_abi.h` is the contract.
The gateway accepts version 2 (`inferdeck_provider_v2`) and, for compatibility,
version 1 (`inferdeck_provider_v1`). A library exporting both is loaded as v2.

### Rules

- **Strings** are UTF-8 JSON objects. **Binary data** (images, audio, video,
  float PCM) travels beside the JSON as `inferdeck_blob` views and is never
  base64-encoded. A JSON field `"blob": n` refers to the nth blob.
- **Ownership.** Every pointer you receive is valid until the function returns.
  Copy what you keep. Results go out through `reply`, which copies before it
  returns. No memory is ever freed across the boundary.
- **Exceptions and panics never cross the boundary.** Catch everything and
  return a status. The SDK does this for you.
- **Struct sizes.** Every v2 struct starts with `uint32_t struct_size`. Set it
  to `sizeof` the struct you compiled against. A reader never touches fields
  past the size the writer set; the gateway rejects a table smaller than the
  v2.0 layout.
- **In-process only.** Do not start child processes or proxy to a server.
- **Unknown JSON keys** may appear in later versions; ignore them.

### Entry point and table

```c
INFERDECK_PROVIDER_EXPORT const inferdeck_provider_v2* inferdeck_provider_v2(void);
```

The returned table must stay valid for the life of the process.

| Member | Required | Blocking | Default when NULL |
|---|---|---|---|
| `struct_size`, `abi_version` (= 2), `manifest_json` | yes | | |
| `init(host, call)` | no | may block | nothing |
| `shutdown()` | no | may block | nothing |
| `create(model_json, call, &instance)` | yes | fast | |
| `destroy(instance)` | yes | may block | |
| `load(instance, call)`, `unload(instance, call)` | yes | may block | |
| `query(instance, query, arg)` | no | **never** | defaults below |
| `acquire_slot(instance, &slot)` | no; with `release_slot` and `query` | **never** | host keeps `n_slots` fixed slots |
| `release_slot(instance, slot)` | no | **never** | host bookkeeping |
| `request_cancel(instance)` | no | **never** | host cancels its own calls only |
| `reset_all_slots(instance, call)` | no | may block | success |
| `resize_slots(instance, slots, call)` | no | may block | `Unavailable` "backend capacity is fixed" |
| `reclaim_idle_context(instance, reserve_mb, call)` | no | may block | nothing reclaimed |
| `invoke(instance, operation, json, blobs, count, call)` | yes | may block | |
| `provider_invoke(operation, json, blobs, count, call)` | no | may block | operation unavailable |

**Never-blocking members** are called by the scheduler while it holds its
global lock. Answer from state you already have: no I/O, no waiting on a
running generation, no lock a generation holds for long. A slow answer here
stalls every request in the gateway.

### Lifecycle and threading

```
init  ->  create  ->  load  ->  invoke / query / slots (many, concurrent)  ->  unload  ->  destroy
                       ^                                                         |
                       +------------------------ load again ---------------------+
shutdown: once, at process exit
```

- `init` runs on the gateway's startup thread before any `create`, once per
  gateway (re)start, so it can run more than once per process; make it
  idempotent. Initialise thread-affine runtimes here (CPython, COM, a backend
  registry). `shutdown` runs once at process exit if `init` ever succeeded.
- `create` gets the model's settings and must not load weights.
- `load`, `unload`, `resize_slots`, `reclaim_idle_context` and
  `reset_all_slots` are never concurrent with `invoke` on the same instance.
- `invoke` runs concurrently, one call per acquired slot; the request's
  `"slot"` field says which.
- Different instances (models) of one provider run concurrently.

### The call object

Every potentially long function receives an `inferdeck_call_v2`. Pass
`call->context` back as the first argument.

| Member | Threads | Use |
|---|---|---|
| `cancelled(ctx)` | any | `INFERDECK_CALL_RUNNING` (0); `INFERDECK_CALL_CANCELLED` (1): return `INFERDECK_CANCELLED`; `INFERDECK_CALL_DEADLINE` (2): return `INFERDECK_TIMEOUT`. Poll between steps. |
| `remaining_ms(ctx)` | any | Milliseconds to the deadline, or -1 for none. |
| `emit(ctx, event_json, blobs, n)` | any; never two at once for one call; never after return | One streaming event. Nonzero return: stop and return `INFERDECK_CANCELLED`. |
| `reply(ctx, result_json, blobs, n)` | at most once, before return | Final result, or `{"message":"..."}` with an error status. |

### The host object

`init` receives `inferdeck_host_v2`, valid until `shutdown`:

- `host_json`: `{"abi_version": 2, "gateway": {...}, "models": [<model_json>...]}`.
  `gateway` carries gateway-wide runtime defaults (`n_batch`, `n_ubatch`,
  `use_mmap`, `use_mlock`, `n_gpu_layers`, `flash_attn`, `kv_offload`,
  `op_offload`, `cache_type_k`, `cache_type_v`, `swa_full`,
  `truncate_prompt`, `vram_safety_margin_mb`); a model's own setting wins over
  these. `models` lists the configured models that use this runtime.
- `log(ctx, level, event, message)`: any thread; levels 0 trace to 5 fatal.

### Status codes

`0` is success. Others become gateway errors and HTTP statuses:

| Code | Name | Meaning |
|---|---|---|
| 1 | `INFERDECK_INVALID_ARGUMENT` | Bad request or settings (HTTP 400) |
| 2 | `INFERDECK_CONTEXT_LENGTH_EXCEEDED` | Prompt too long |
| 3 | `INFERDECK_NOT_FOUND` | Missing file or entity |
| 4 | `INFERDECK_NOT_LOADED` | Called before `load` |
| 5 | `INFERDECK_ALREADY_EXISTS` | |
| 6 | `INFERDECK_IO_ERROR` | |
| 7 | `INFERDECK_PARSE_ERROR` | Malformed JSON |
| 8 | `INFERDECK_OUT_OF_MEMORY` | VRAM or RAM exhausted |
| 9 | `INFERDECK_RESOURCE_BUSY` | |
| 10 | `INFERDECK_TIMEOUT` | Deadline passed |
| 11 | `INFERDECK_CANCELLED` | Cancelled by the client or the gateway |
| 12 | `INFERDECK_UNAVAILABLE` | Capacity or feature unavailable right now |
| 13 | `INFERDECK_INTERNAL` | Anything else |
| 14 | `INFERDECK_NOT_IMPLEMENTED` | v2: optional operation not implemented; the host uses its default |

### Manifest

```json
{
  "runtime": "my_runtime",
  "version": "1.0.0",
  "modalities": {"text": ["chat_completions", "responses"], "embedding": ["embeddings"]},
  "model_source": "gguf_path",
  "vision": true,
  "reasoning": true,
  "speculative": false,
  "unsupported_options": ["mirostat", "tfs_z"]
}
```

| Key | Meaning |
|---|---|
| `runtime` | Name models select. Lowercase letters, digits, underscores. |
| `version` | Shown in the log and the providers endpoint. Include the runtime's upstream revision. |
| `modalities` | Each modality served, with its capabilities. Config validation rejects models asking for anything else. |
| `model_source` | `gguf_path` (model must set `gguf_path`), `artifacts` (default; an `artifacts` map), or `none`. |
| `vision`, `reasoning`, `speculative` | Whether text models may use image input, reasoning, MTP. Default false. |
| `unsupported_options` | Request features this provider cannot honour. The gateway rejects such a request with HTTP 400 instead of silently dropping the option. Values: `logprobs`, `logit_bias`, `mirostat`, `tfs_z`, `prompt_keep_tokens`, `add_generation_prompt`, `reasoning_format`, `tools`, `structured_output`, `image_input`, `audio_input`. |

| Modality | Capabilities | Operations |
|---|---|---|
| `text` | `chat_completions`, `responses` | `chat`, `estimate_request_demand`, `ensure_request_capacity`, `chat_template_meta` |
| `embedding` | `embeddings` | `embed` |
| `image` | `image_generation`, `image_editing` | `image` |
| `audio_generation` | `audio_generation` | `audio_generation` |
| `video` | `video_generation` | `video` |
| `audio_speech` | `audio_speech` | `speech_validate`, `speech` |
| `audio_transcription` | `audio_transcription` | `transcribe` |
| `decision` | `decisions` | `decide` |

### Status queries

`query(instance, what, arg)` returns an `int64_t`, or `INFERDECK_QUERY_DEFAULT`
for anything not implemented. Never blocks.

| Query | `arg` | IBackend method | Default |
|---|---|---|---|
| `INFERDECK_QUERY_LOADED` | | `is_loaded` | host's own flag |
| `INFERDECK_QUERY_HEALTHY` | | `execution_healthy` | 1 |
| `INFERDECK_QUERY_VRAM_USAGE_MB` | | `vram_usage_mb` | model's `vram_required_mb` while loaded |
| `INFERDECK_QUERY_N_SLOTS` | | `n_slots` | model's `n_slots` |
| `INFERDECK_QUERY_N_FREE_SLOTS` | | `n_free_slots` | host bookkeeping |
| `INFERDECK_QUERY_SLOT_BUSY` | slot | `slot_busy` | host bookkeeping |
| `INFERDECK_QUERY_CONTEXT_POOL_CAPACITY` | | `context_pool_capacity` | 0 |
| `INFERDECK_QUERY_MIN_SLOTS` | | `min_slots` | `n_slots` |
| `INFERDECK_QUERY_CAN_RESIZE_SLOTS` | | `can_resize_slots` | 0 |
| `INFERDECK_QUERY_CAN_RECLAIM_IDLE_CONTEXT` | | `can_reclaim_idle_context` | 0 |
| `INFERDECK_QUERY_ESTIMATE_VRAM_MB` | slots | `estimate_vram_mb` | current usage |
| `INFERDECK_QUERY_ADDITIONAL_VRAM_RESERVE_MB` | | `additional_vram_reserve_mb` | 0 |
| `INFERDECK_QUERY_LIVE_VRAM_ACCOUNTING_COMPLETE` | | `live_vram_accounting_complete` | 0 |

### What each operation must do

Exact JSON field names are defined once, in `libs/provider/src/codec.cpp`; the
shapes below summarise it. Every `invoke` request has `"slot"`.

**`model_json`** (to `create`, and each entry of `host_json.models`): every
field of `model::ModelInfo` under its C++ name (`name`, `runtime`,
`modality`, `capabilities`, `gguf_path`, `mmproj_path`, `chat_template_path`,
`n_slots`, `min_slots`, `context_size`, `vram_required_mb`, `kv_unified`,
`context_pool_size`, `context_pool_auto`, `concurrency_auto`, `n_batch`,
`n_ubatch`, `cache_type_k`, `cache_type_v`, `n_gpu_layers`, `mtp_*`,
`speculative_type`, `draft_model_path`,
`reasoning_format`, `continuation_grace_ms`, `artifacts`, `sampling` including
the `dry_*` keys, `reasoning`, ...), plus `model_path` (= `gguf_path`) for v1
compatibility. Unset optionals are `null`. Put anything runtime-specific in
`artifacts`.

`speculative_type` (`mtp`, `dflash`, `dflash2`, or empty) and
`draft_model_path` carry DFlash settings to the `llama_cpp` DLL. They were
added to the codec without an ABI version change: a DLL built before them
ignores both keys and loads the model without DFlash.

**`chat`** (text). Request: `prompt`, `messages[]` (`role`; `content[]` of
`{"type":"text","text"}`, `{"type":"image","blob","media_type","detail"}`,
`{"type":"audio","blob","media_type"}`; `tool_calls[]` of
`{id,name,arguments}`; `reasoning`; `tool_call_id`; `name`), `tools[]`
(`name`, `description`, `parameters_schema`), `tool_choice`
(`kind`: `auto|none|required|function`, `function_name`), `output` (`kind`:
`text|json_object|json_schema|grammar`, `name`, `description`, `schema`,
`strict`), `sampling` (`seed`, and only when set: `temperature`, `top_p`,
`top_k`, `min_p`, `repeat_penalty`, `repeat_last_n`, `frequency_penalty`,
`presence_penalty`, `tfs_z`, `mirostat`, `mirostat_eta`, `mirostat_tau`,
`logit_bias` as `[[token, bias]...]`; fall back to the model's `sampling`),
`stop[]`, `max_output_tokens` (-1: whatever context is left),
`parallel_tool_calls`, `logprobs`, `top_logprobs`, `add_generation_prompt`,
`reasoning_format` (`auto|none|deepseek|deepseek_legacy`), optional
`reasoning_effort`, `enable_reasoning`, `context_window`,
`prompt_keep_tokens`, `stream`, and `progress` (true when the gateway wants
request progress).

Events while running:

- `{"type":"delta","content","reasoning","tool_calls":[{index,id,type,name,arguments}],"logprobs":[...]}`
  for each piece of output when `stream` is true. Content and reasoning deltas
  must add up to the final `text` and `reasoning_text`.
- `{"type":"request_progress","phase","prompt_tokens","processed_tokens","cached_tokens","output_tokens","prompt_ms","generation_ms"}`
  when `progress` is true, whenever it changes (phase 2 prefill, 3 decode).

Reply: `text`, `reasoning_text`, `finish_reason` (`stop|length|tool_calls`),
`prompt_tokens`, `cached_prompt_tokens`, `completion_tokens`,
`reasoning_tokens`, `tool_calls[]` (`{id,type,name,arguments}`), `logprobs[]`
(`{token,logprob,bytes,top_logprobs:[{token,logprob,bytes}]}`), and timings
`duration_ms`, `prompt_duration_ms`, `first_token_duration_ms`,
`generation_duration_ms`, `tokens_per_second`, `prompt_decode_duration_ms`,
`prompt_decode_tokens`, `mtp_drafted_tokens`, `mtp_accepted_tokens`. When
`duration_ms` is 0 the gateway measures the timings itself.

**`estimate_request_demand`** (text; must be fast): request as for `chat`.
Reply `{prompt_positions, output_tokens, required_context, required_sequences,
aggregate_context, aggregate_sequences}`; all zero means "no constraint".

**`ensure_request_capacity`** (text): the request is a demand object. Grow
context capacity if needed, honouring `cancelled` and the deadline.

**`chat_template_meta`** (text): called once after each `load`. Reply
`{thinking_start_tag, thinking_end_tags[], preserved_tokens[], supports_thinking}`.

**`embed`**: `{"inputs": ["text", [101, 2023]], "dimensions"?}` ->
`{"embeddings": [[...]...], "prompt_tokens"}`, one vector per input, in order.

**`decide`**: `{"input", "questions": [{"type": "predicate|choice|score",
"instructions", "options": [{label, description}]}]}` ->
`{"answers": [{"probabilities": [...]}], "input_tokens"}`. Probabilities
follow option order and sum to 1; a predicate has two: false, then true.

**`image`**: `{prompt, negative_prompt, width, height, count, steps, seed,
guidance_scale, reference_image_count}`, the first `reference_image_count`
blobs being encoded reference images (an edit). `steps` 0 and a negative
`guidance_scale` mean the model default. Events:
`{"type":"progress","percent":n}`; a nonzero `emit` return means stop and
return `INFERDECK_CANCELLED`. Reply `{"count"}` with one PNG per blob.

**`audio_generation`**: `{prompt, lyrics, duration_seconds, seed, steps,
guidance_scale}`. Progress as for `image`. Reply `{seed,
output_audio_seconds}` with blob 0 a WAV file.

**`video`**: `{prompt, negative_prompt, width, height, frames, fps, steps,
seed, guidance_scale}`. Progress as for `image`. Reply `{content_type,
output_video_seconds}` with blob 0 the encoded video.

**`speech_validate`** (must be fast; may be called before load): request as
for `speech`. Reject an unknown voice or format with
`INFERDECK_INVALID_ARGUMENT`.

**`speech`**: `{input, voice, format, speed, stream}`. With `stream`, emit
`{"type":"audio"}` events with blob 0 the next encoded chunk; the chunks must
concatenate to the final audio. Reply `{content_type, output_audio_seconds}`
with blob 0 the complete audio.

**`transcribe`**: `{sample_rate, language, prompt, temperature, pcm_blob}`;
the blob is mono 32-bit float PCM in native byte order. Progress as for
`image`. Reply `{text, language, duration_seconds, inference_ms, segments:
[{id, start_seconds, end_seconds, text, tokens[], avg_logprob,
no_speech_probability}]}`.

Return `INFERDECK_NOT_IMPLEMENTED` for an operation you do not serve.

### Process-level operations

`provider_invoke` runs an operation that needs no model instance; the gateway
calls it through `model::invoke_provider_operation(runtime, operation, json)`.
With the SDK, set `Definition::operation`. `llama_cpp` implements:

| Operation | Request | Reply | Used by |
|---|---|---|---|
| `quantize` | `{source, destination, quantization: q4_k_m|q5_k_m|q6_k|q8_0, threads}` | `{}` | model store GGUF quantisation |
| `devices` | `{}` | `{devices: [{index, name, description, type}]}` | `gpuBackend` in status and metrics |

### Version 1

Version 1 (`inferdeck_provider_v1`, `inferdeck_call` without `struct_size`)
still loads. It has `create`, `destroy`, `load`, `unload`, `vram_usage_mb` and
`invoke` for `chat`, `embed`, `image`, `audio_generation`, `video`, `speech`,
`transcribe` and `decide`, with host-managed slots and the IBackend defaults
for everything else. It receives the same JSON as v2. Requests using options
it predates (`logprobs`, `logit_bias`, `mirostat`, `tfs_z`,
`prompt_keep_tokens`, `add_generation_prompt`, `reasoning_format`) are
rejected with HTTP 400. New providers should use v2.

### Another language

Export a C function `inferdeck_provider_v2` returning a pointer to a struct
laid out as `inferdeck_provider_v2` (`#[repr(C)]` in Rust, `@_cdecl` in Swift),
set `struct_size`, and implement the required members. The JSON shapes above
are the whole wire format. Name the file `<runtime>.provider.dll` (`.so` or
`.dylib` elsewhere).

## Moving a vendored runtime pin

The vendored runtimes are pinned git submodules. stable-diffusion.cpp and
ACE-Step compile against the vendored llama.cpp/ggml tree (each DLL still gets
its own static copy). To move a pin for every runtime DLL:

1. Change one pin per pull request and record the old and new revisions.
2. Configure. The build stops if stable-diffusion.cpp and InferDeck disagree
   on `GGML_MAX_NAME`. Change the value in the root `CMakeLists.txt` and in
   `scripts/Test-ArchitecturePolicy.ps1` together.
3. Build everything, not only the runtime you touched. A new
   stable-diffusion.cpp can need ggml functions the pinned llama.cpp does not
   have; that shows up as a compile error in stable-diffusion.cpp.
4. Run `ctest -L "unit|integration"` and `scripts/Test-ArchitecturePolicy.ps1`.
5. Run one real request per affected modality on the GPU, on a test gateway
   with a spare port, not on the live service. Unit tests stub the native
   libraries and cannot see an ABI mismatch: the 2026-11 stable-diffusion.cpp
   upgrade passed every test and aborted on its first real image.
6. Read the gateway log for `diffusion_runtime` warnings and errors.
7. Update `docs/release-reproducibility.md` with the new revision.

## Limits and known gaps

- **Shared diffusion lock.** stable-diffusion.cpp and LTX run in separate
  DLLs but still take turns on the GPU: the lock in
  `native_runtimes/diffusion_runtime_lock.hpp` is a named kernel mutex keyed by
  the process id, which is the same object in every module.
- **Voice listing** has no method in `ISpeechBackend`, so there is no ABI
  operation for it; `speech_validate` is how a provider rejects a voice.
- **Profile benchmarks** create candidates through the installed `llama_cpp`
  provider; per-candidate `flash_attn` travels as the `flash_attn` artifact.
