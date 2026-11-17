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
2. [Recipe A: add a new runtime as a provider DLL](#recipe-a-add-a-new-runtime-as-a-provider-dll)
3. [Recipe B: upgrade a runtime by dropping in a new DLL](#recipe-b-upgrade-a-runtime-by-dropping-in-a-new-dll)
4. [Recipe C: check a provider](#recipe-c-check-a-provider)
5. [Installing, verifying, and rolling back](#installing-verifying-and-rolling-back)
6. [ABI v2 reference](#abi-v2-reference)
7. [Upgrading a built-in runtime in the gateway itself](#upgrading-a-built-in-runtime-in-the-gateway-itself)
8. [Limits and known gaps](#limits-and-known-gaps)

## Concepts

| Kind | Lives in | Use it when |
|---|---|---|
| Built-in | Compiled into `inferdeck-gateway.exe` | The runtime ships with InferDeck |
| Provider DLL | `providers/<runtime>.provider.dll` next to the exe | You want a different build, a newer runtime, a vendor runtime, or another language, without rebuilding InferDeck |

Both become a `ProviderDescriptor` (`libs/model/include/model/provider.hpp`): a
runtime name, what it can do (its contract), and a factory that creates one
backend per configured model. A provider DLL is wrapped by `PluginBackend`
(`libs/provider/src/plugin_provider.cpp`), which implements every backend
interface the coordinator uses, so the scheduler cannot tell a DLL from a
built-in.

Each model in `gateway.yml` selects its runtime by name:

```yaml
model_registry:
  - name: "my-model"
    runtime: "llama_cpp"        # the provider that runs this model
```

Provider DLLs are registered after built-ins. A DLL that declares a built-in
name (`llama_cpp`) replaces the built-in for every model using that name; the
log says `provider_overridden`. To run both side by side, give the DLL its own
name (`llama_cpp_next`) and point individual models at it.

Pieces you will touch:

| Path | What it is |
|---|---|
| `libs/provider/include/inferdeck/provider_abi.h` | The C ABI. The whole contract, plain C. |
| `libs/provider/include/provider/sdk.hpp` | C++ SDK: an `IBackend` factory becomes a DLL in a few lines. |
| `libs/provider/src/codec.cpp` | The JSON + blob wire format for every request and result. Compiled into the gateway and every SDK DLL. Authoritative for field names. |
| `libs/provider/examples/echo_provider_v2.cpp` | Reference SDK provider. Copy it to start. |
| `libs/provider/examples/echo_provider.cpp` | Raw-C v1 provider, kept for compatibility tests. |
| `libs/provider/runtimes/` | InferDeck's own runtimes packaged as DLLs, and the CMake that builds them. |
| `libs/provider/tools/provider_check.cpp` | `inferdeck-provider-check`, the validator. |
| `libs/model/include/model/imodel.hpp`, `ibackend.hpp` | The C++ interfaces ABI v2 mirrors. |

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

   This produces `${BUILD}/providers/my_runtime.provider.dll`. If your runtime
   uses ggml (llama.cpp, stable-diffusion.cpp, whisper.cpp, ...) it must link
   ggml **statically** and be built in the static build of Recipe B, step 2:
   Windows reuses an already-loaded module with the same base name, so a
   `ggml.dll` shipped next to your DLL would silently resolve to the gateway's.

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

Example: Qwen4 needs a newer llama.cpp than the gateway was built with. Instead
of rebuilding the gateway, build `llama_cpp.provider.dll` from the newer
llama.cpp and drop it into `providers/`. It replaces the built-in `llama_cpp`
for every model; deleting it rolls back.

1. **Move the runtime.** In a separate worktree or branch, check out the new
   revision of the vendored runtime:

   ```bash
   git -C libs/third_party/llama.cpp fetch origin
   git -C libs/third_party/llama.cpp checkout <new-revision>
   ```

   Record the old and new revisions. If `llama_cpp_wrapper` no longer compiles
   against the new API, fix it in this worktree; the fix ships inside the DLL.

2. **Configure a static-ggml build directory** (once). It reuses the main
   build's vcpkg packages, so nothing is downloaded:

   ```bash
   PROVIDERS=F:/InferDeck-build/qwen4-providers
   cmake -S . -B $PROVIDERS -G "Visual Studio 18 2026" -A x64 \
     -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake \
     -DVCPKG_TARGET_TRIPLET=x64-windows \
     -DVCPKG_INSTALLED_DIR=$BUILD/vcpkg_installed -DVCPKG_MANIFEST_INSTALL=OFF \
     -DBUILD_SHARED_LIBS=OFF -DINFERDECK_BUILD_RUNTIME_PROVIDERS=ON \
     -DINFERDECK_BUILD_TESTS=OFF
   ```

   `INFERDECK_BUILD_RUNTIME_PROVIDERS` refuses to configure with shared libs on
   purpose (see Recipe A, step 4). Pass the same `INFERDECK_*_ROOT` cache
   values as the main build if your runtimes live outside the repository.

3. **Build the provider DLL.**

   ```bash
   cmake --build $PROVIDERS --config Release -j 8 --target inferdeck-provider-llama_cpp
   ```

   Other targets: `inferdeck-provider-jev_style`,
   `inferdeck-provider-stable_diffusion_cpp`, `inferdeck-provider-ltx_video_cpp`,
   `inferdeck-provider-ace_step_cpp`, `inferdeck-provider-whisper_cpp`, or
   `inferdeck-runtime-providers` for all of them. Output:
   `$PROVIDERS/providers/<runtime>.provider.dll`. The first build compiles
   llama.cpp and the Vulkan shaders and takes a while; later builds are
   incremental.

4. **Check that ggml is private.** The DLL must not import `ggml*.dll` or
   `llama*.dll`:

   ```bash
   dumpbin /dependents $PROVIDERS/providers/llama_cpp.provider.dll
   ```

   Expected: Windows system DLLs, `vulkan-1.dll`, the MSVC runtime,
   `spdlog.dll`, `fmt.dll`. Nothing named `ggml` or `llama`.

5. **Check it without loading weights** (loads the library, runs `init`,
   creates an instance per modality):

   ```bash
   $BUILD/bin/Release/inferdeck-provider-check $PROVIDERS/providers/llama_cpp.provider.dll --no-load
   ```

6. **Check it with the model**, where GPU work is allowed:

   ```bash
   $BUILD/bin/Release/inferdeck-provider-check $PROVIDERS/providers/llama_cpp.provider.dll \
     --model C:/models/<model>.gguf --modality text
   ```

7. **Install it** next to a test gateway, run one real request per affected
   modality, then roll it out: [Installing, verifying, and rolling back](#installing-verifying-and-rolling-back).

When the gateway's own shared llama.cpp/ggml must move instead, follow
[Upgrading a built-in runtime in the gateway itself](#upgrading-a-built-in-runtime-in-the-gateway-itself).

## Recipe C: check a provider

`inferdeck-provider-check` loads a library exactly as the gateway does and, for
every modality in its manifest, runs `init`, creates an instance, checks that a
slot is refused before load, loads, checks the status queries and slot
accounting, sends one small request, checks cancellation, releases the slot,
unloads, and loads again.

```bash
$BUILD/bin/Release/inferdeck-provider-check <library> [--model <path>] [--artifact key=value]... \
    [--modality <name>]... [--skip-invoke] [--no-load]
```

| Option | Meaning |
|---|---|
| `--model <path>` | Sets the model's `gguf_path` (`model_path` in JSON). |
| `--artifact key=value` | Adds one `artifacts` entry. Repeatable. |
| `--modality <name>` | Check only this modality. Repeatable. |
| `--skip-invoke` | Lifecycle and slots only; send no request. Use for slow media models. |
| `--no-load` | Stop after `init` and `create`. Use where weights or a GPU are unavailable. |

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

## Installing, verifying, and rolling back

1. Copy the DLL into `providers/` next to `inferdeck-gateway.exe`, plus any
   non-ggml libraries it needs that are not already next to the gateway:

   ```bash
   mkdir -p <gateway dir>/providers
   cp $PROVIDERS/providers/llama_cpp.provider.dll <gateway dir>/providers/
   ```

   For a test gateway, point it at a staging folder instead of touching the
   executable's folder:
   `inferdeck-gateway.exe -c config/gateway.test.yml --providers F:/staging/providers`.

2. Restart the gateway. Providers load once, at process start.

3. Verify:
   - Log: `provider_registered runtime=llama_cpp origin=<path> version=<v>` and,
     when it replaced a built-in, `provider_overridden runtime=llama_cpp`.
     Rejected libraries log `provider_plugin_rejected <path>: <reason>`.
   - `GET /api/inferdeck/v1/providers`: each provider's `runtime`, `origin`,
     `version`, `modalities`, `state` (`active`, `overridden`, `not_linked`,
     `init_failed` with `error`), plus `rejected_plugins`.
   - `GET /api/inferdeck/v1/models`: `runtime` and `runtime_available` per model.
   - One real request per modality the DLL serves.

4. Roll back: delete the DLL from `providers/` and restart. The built-in (or
   the previous DLL) takes over. Keep the previous DLL beside the new one under
   a name that does not end in `.provider.dll` (for example
   `llama_cpp.provider.dll.previous`), so rollback is a rename.

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
`reasoning_format`, `continuation_grace_ms`, `artifacts`, `sampling` including
the `dry_*` keys, `reasoning`, ...), plus `model_path` (= `gguf_path`) for v1
compatibility. Unset optionals are `null`. Put anything runtime-specific in
`artifacts`.

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

## Upgrading a built-in runtime in the gateway itself

Built-in runtimes are pinned git submodules that share one ggml with
llama.cpp. When the gateway's own copy must move:

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

- **One runtime per DLL.** Runtimes that share process-wide state inside the
  gateway lose it when split: the built-in stable-diffusion.cpp and LTX share
  one diffusion lock, but two DLLs each have their own, so an image job and a
  video job can run at the same time and need VRAM for both.
- **Built-in setup still runs.** When a DLL overrides a built-in name, the
  built-in's own setup (for example `llama_backend_init` on the gateway's
  ggml) still runs, because other built-ins such as `jev_style` rely on it.
- **Voice listing** has no method in `ISpeechBackend`, so there is no ABI
  operation for it; `speech_validate` is how a provider rejects a voice.
- **Profile benchmarking and GGUF quantisation** in the dashboard use the
  gateway's built-in llama.cpp, not a `llama_cpp` DLL.
