# Providers

A provider is the code that runs a model: llama.cpp for text, stable-diffusion.cpp
for images, and so on. InferDeck treats every provider the same way. The gateway
asks a provider to load a model, run a request, and unload. It never calls into a
provider's internals.

This page is written so that a person or a coding agent can add or swap a
provider without reading the rest of the codebase.

## Two kinds of provider

| Kind | Lives in | Use it when |
|---|---|---|
| Built-in | Compiled into `inferdeck-gateway.exe` | The runtime ships with InferDeck |
| Plugin | A shared library in the `providers/` folder | You want a different build, vendor, or language without rebuilding InferDeck |

Both end up as the same thing inside the gateway: a `ProviderDescriptor`
(`libs/model/include/model/provider.hpp`) holding a runtime name, what the
runtime can do, and a factory that creates one backend per model.

Built-ins are listed in one place, `apps/inferdeck-gateway/src/builtin_providers.ipp`.
Plugins are discovered at startup. Everything after that point only sees the
backend interfaces in `libs/model/include/model/imodel.hpp`.

## How a model picks its provider

Each model in `gateway.yml` names a runtime:

```yaml
model_registry:
  - name: "my-model"
    runtime: "llama_cpp"        # change this line to switch provider
```

Switching a model to another provider is that one line. The runtime name comes
from the provider: built-ins use the names in
`libs/model/include/model/runtime_contract.hpp`, plugins declare theirs in their
manifest.

A plugin may reuse a built-in name. Plugins are registered after built-ins, so
the plugin wins and the log records `provider_overridden`. Use this to replace
the bundled llama.cpp with, for example, a CUDA build, while every model keeps
`runtime: llama_cpp`. To run both side by side, give the plugin its own name
such as `llama_cpp_cuda` and point individual models at it.

## Installing a plugin

1. Build the provider as a shared library (`.dll` on Windows, `.so` on Linux,
   `.dylib` on macOS).
2. Copy it into `providers/` next to `inferdeck-gateway.exe`. Put any libraries
   it depends on in the same folder; they are found there automatically.
3. Set `runtime:` on the models that should use it.
4. Restart the gateway.

Use `--providers <directory>` to load from a different folder.

Check the gateway log:

- `provider_registered runtime=<name> origin=<path> version=<v>`: loaded.
- `provider_plugin_rejected <path>: <reason>`: not loaded, with the reason.
- `provider_overridden runtime=<name>`: replaced a provider of the same name.

`GET /api/inferdeck/v1/providers` returns the same information without log
access: every provider with its `runtime`, `origin`, `version`, `modalities`,
and `state` (`active`, `overridden`, `not_linked`, or `init_failed` with an
`error`), plus `rejected_plugins` with the reason for each library that was
skipped. `GET /api/inferdeck/v1/models` reports `runtime` and
`runtime_available` for every model.

To remove a plugin, delete its file and restart.

**Security.** A plugin runs inside the gateway process with the gateway's
privileges. Treat the `providers/` folder like the executable itself: only
administrators should be able to write to it, and only load libraries you
trust.

## The plugin ABI

The whole contract is one C header:
`libs/provider/include/inferdeck/provider_abi.h`. It has no InferDeck types and
no C++ in it, so any language that can export a C function can implement it:
C, C++, Rust, Swift, Zig, or a thin C shim in front of Python.

A working example is `libs/provider/examples/echo_provider.cpp`. Copy it to
start a new provider.

### Ground rules

- Text is UTF-8 JSON. Binary data (images, audio, video, PCM samples) travels
  beside the JSON as "blobs" and is never base64-encoded.
- Pointers handed to a function are valid until that function returns. Copy
  anything you need to keep.
- You never allocate memory for InferDeck to free, and InferDeck never
  allocates memory for you to free. Results are handed over through a callback
  and copied before the callback returns.
- Never let an exception or panic escape. Return a status code.
- Everything runs in the gateway process. Do not start child processes.

### Entry point

Export exactly one function:

```c
const inferdeck_provider* inferdeck_provider_v1(void);
```

It returns a table that must stay valid for the life of the process:

| Field | Purpose |
|---|---|
| `abi_version` | Must be `INFERDECK_PROVIDER_ABI_VERSION` (1). Any other value is rejected. |
| `manifest_json` | Static JSON describing the provider. |
| `create` | Make an instance for one configured model. Do not load weights yet. |
| `destroy` | Free an instance. InferDeck unloads it first. |
| `load` | Load the weights. |
| `unload` | Release the weights and GPU memory. |
| `vram_usage_mb` | GPU megabytes held right now. Return a negative number to use the model's configured `vram_required_mb`. |
| `invoke` | Run one request. |

Every function except `destroy` and `vram_usage_mb` returns a status. `0` is
success. The other values are listed in the header (`INFERDECK_NOT_LOADED`,
`INFERDECK_CANCELLED`, `INFERDECK_OUT_OF_MEMORY`, and so on) and map directly to
the HTTP errors InferDeck returns to clients.

### Manifest

```json
{
  "runtime": "echo",
  "version": "1.0.0",
  "modalities": {
    "text": ["chat_completions", "responses"],
    "image": ["image_generation"]
  },
  "model_source": "artifacts",
  "vision": false,
  "reasoning": false
}
```

| Key | Meaning |
|---|---|
| `runtime` | Name models select. Lowercase letters, digits, underscores. |
| `version` | Free text shown in the log. |
| `modalities` | Each modality the provider serves, with the capabilities it supports. Config validation rejects a model that asks for anything not listed. |
| `model_source` | `gguf_path`: the model must set `gguf_path`. `artifacts` (default): the model must set an `artifacts` map. `none`: neither is required. |
| `vision`, `reasoning` | Whether text models may enable image input or reasoning. Default `false`. |

Known modalities and capabilities:

| Modality | Capabilities | Operation |
|---|---|---|
| `text` | `chat_completions`, `responses` | `chat` |
| `embedding` | `embeddings` | `embed` |
| `image` | `image_generation`, `image_editing` | `image` |
| `audio_generation` | `audio_generation` | `audio_generation` |
| `video` | `video_generation` | `video` |
| `audio_speech` | `audio_speech` | `speech` |
| `audio_transcription` | `audio_transcription` | `transcribe` |
| `decision` | `decisions` | `decide` |

### The call object

Every call receives an `inferdeck_call` with a `context` pointer and three
callbacks. Pass `context` back unchanged as the first argument.

| Callback | Use |
|---|---|
| `cancelled(context)` | Returns nonzero when the client went away or the model is being unloaded. Check it between steps and return `INFERDECK_CANCELLED`. |
| `emit(context, event_json, blobs, blob_count)` | Send a streaming event. Returns nonzero when you should stop. |
| `reply(context, result_json, blobs, blob_count)` | Set the final result. Call it once, before returning. |

On failure, call `reply` with `{"message":"what went wrong"}` and return a
nonzero status. The message is logged and returned to the client.

### Lifecycle

```
create  ->  load  ->  invoke (many times)  ->  unload  ->  destroy
                 ^                               |
                 +----------- load again --------+
```

- `create` receives the model's settings as JSON (below). It should be fast.
- `load` and `unload` are never called while an `invoke` is running.
- `invoke` may run on several threads at once, up to the model's `n_slots`.
  Each concurrent request carries a different `slot` number, from `0` to
  `n_slots - 1`. Use it to index per-request state such as a KV cache.
- InferDeck owns scheduling, queuing, VRAM budgeting, and eviction. A provider
  only loads, runs, and unloads.

### Model settings passed to `create`

```json
{
  "name": "my-model",
  "family": "qwen",
  "runtime": "echo",
  "modality": "text",
  "capabilities": ["chat_completions", "responses"],
  "compute": "vulkan_gpu",
  "model_path": "C:/models/model.gguf",
  "mmproj_path": "",
  "chat_template_path": "",
  "n_slots": 2,
  "context_size": 32768,
  "default_max_output_tokens": 0,
  "vram_required_mb": 8000,
  "has_vision": false,
  "n_gpu_layers": 99,
  "artifacts": {"any_key": "any string value"},
  "sampling": {"temperature": 0.8, "top_p": 0.95, "top_k": 40,
               "min_p": 0.05, "repeat_penalty": 1.0, "repeat_last_n": 64}
}
```

`model_path` is the model's `gguf_path`. `n_gpu_layers` is omitted when unset.
`artifacts` is the free-form map from `gateway.yml`; use it for any setting
specific to your provider (extra weight files, a device index, a backend
name). Ignore keys you do not recognise.

### Operations

`invoke` receives an operation name, a request as JSON, and optional blobs.
Return `INFERDECK_UNAVAILABLE` for operations you do not implement.

Every request contains `"slot"`. Unknown keys may be added in later versions,
so ignore keys you do not recognise.

#### `chat`

Request:

```json
{
  "slot": 0,
  "stream": true,
  "prompt": "",
  "messages": [
    {
      "role": "user",
      "content": [
        {"type": "text", "text": "Describe this"},
        {"type": "image", "blob": 0, "media_type": "image/png", "detail": "auto"},
        {"type": "audio", "blob": 1, "media_type": "audio/wav"}
      ],
      "tool_calls": [{"id": "call_1", "name": "search", "arguments": "{\"q\":\"x\"}"}],
      "reasoning": "",
      "tool_call_id": "",
      "name": ""
    }
  ],
  "tools": [{"name": "search", "description": "...", "parameters_schema": "{...}"}],
  "tool_choice": {"kind": "auto", "function_name": ""},
  "output": {"kind": "text", "name": "", "schema": "", "strict": false},
  "sampling": {"seed": -1, "temperature": 0.7},
  "stop": ["</s>"],
  "max_output_tokens": -1,
  "parallel_tool_calls": true,
  "reasoning_effort": "medium",
  "enable_reasoning": true,
  "context_window": 16384
}
```

- `role` is `developer`, `system`, `user`, `assistant`, `tool`, or `function`.
- `"blob": n` refers to the nth blob passed to `invoke`.
- `tool_choice.kind` is `auto`, `none`, `required`, or `function`.
- `output.kind` is `text`, `json_object`, `json_schema`, or `grammar`.
- `sampling` keys other than `seed` appear only when the client set them. Fall
  back to the model's `sampling` settings from `create`.
- `max_output_tokens` of `-1` means "use whatever context is left".
- `reasoning_effort`, `enable_reasoning`, and `context_window` are omitted when
  unset.

When `stream` is true, emit an event per piece of output:

```json
{"type": "delta", "content": "Hel", "reasoning": "",
 "tool_calls": [{"index": 0, "id": "call_1", "name": "search", "arguments": "{\"q\""}]}
```

All keys except `type` are optional. Stop when `emit` returns nonzero.

Final reply, always required:

```json
{
  "text": "Hello",
  "reasoning_text": "",
  "finish_reason": "stop",
  "prompt_tokens": 12,
  "cached_prompt_tokens": 0,
  "completion_tokens": 3,
  "reasoning_tokens": 0,
  "tool_calls": [{"id": "call_1", "name": "search", "arguments": "{\"q\":\"x\"}"}]
}
```

`finish_reason` is `stop`, `length`, or `tool_calls`. Return the full text in
the reply even when you streamed it. InferDeck measures timings itself.

#### `embed`

Request: `{"slot": 0, "inputs": ["some text", [101, 2023, 102]], "dimensions": 256}`.
Each input is either a string or a list of token IDs. `dimensions` is optional.

Reply: `{"embeddings": [[0.1, 0.2], [0.3, 0.4]], "prompt_tokens": 7}`, one
vector per input, in order.

#### `image`

Request:

```json
{"slot": 0, "prompt": "a red bicycle", "negative_prompt": "", "width": 1024,
 "height": 1024, "count": 1, "steps": 20, "seed": -1, "guidance_scale": 7.0,
 "reference_image_count": 0}
```

`steps` of `0` and a negative `guidance_scale` mean "use the model's
default".

When `reference_image_count` is above zero the request is an edit: the first
that many blobs are encoded reference images (PNG or JPEG), in order. Declare
the `image_editing` capability to receive them.

Progress events: `{"type": "progress", "percent": 40}`.

Reply: `{}` with one PNG file per blob, `count` blobs in total.

#### `audio_generation`

Request: `{"slot": 0, "prompt": "...", "lyrics": "...", "duration_seconds": 30.0,
"seed": -1, "steps": 0, "guidance_scale": 0.0}`. Progress events as for `image`.

Reply: `{"seed": 1234, "output_audio_seconds": 30.0}` with blob 0 holding a WAV
file.

#### `video`

Request: `{"slot": 0, "prompt": "...", "negative_prompt": "", "width": 512,
"height": 320, "frames": 33, "fps": 24, "steps": 20, "seed": -1,
"guidance_scale": 6.0}`. Progress events as for `image`.

Reply: `{"content_type": "video/mp4", "output_video_seconds": 1.4}` with blob 0
holding the encoded video.

#### `speech`

Request: `{"slot": 0, "input": "Hello", "voice": "default", "format": "wav",
"speed": 1.0, "stream": false}`.

When `stream` is true, emit `{"type": "audio"}` events with blob 0 holding the
next chunk of encoded audio.

Reply: `{"content_type": "audio/wav", "output_audio_seconds": 0.8}` with blob 0
holding the complete audio.

#### `transcribe`

Request: `{"slot": 0, "sample_rate": 16000, "language": "en", "prompt": "",
"temperature": 0.0, "pcm_blob": 0}`. The blob is mono 32-bit float PCM in the
machine's native byte order. Its sample count is the blob size divided by 4.

Progress events as for `image`.

Reply:

```json
{"text": "hello world", "language": "en", "duration_seconds": 1.9,
 "segments": [{"id": 0, "start_seconds": 0.0, "end_seconds": 1.9, "text": "hello world"}]}
```

#### `decide`

Request:

```json
{"slot": 0, "input": "The parcel arrived broken.",
 "questions": [
   {"type": "predicate", "instructions": "The item is damaged.", "options": []},
   {"type": "choice", "instructions": "Which team?",
    "options": [{"label": "billing", "description": "payments"}, {"label": "support", "description": ""}]}
 ]}
```

`type` is `predicate`, `choice`, or `score`. A score's options are its levels,
lowest first.

Reply: `{"answers": [{"probabilities": [0.1, 0.9]}, {"probabilities": [0.8, 0.2]}], "input_tokens": 120}`.
One answer per question, in order. Probabilities follow the option order and
sum to 1. A predicate has two: false, then true.

## Building a plugin

The reference provider builds with the rest of InferDeck:

```bash
cmake --build <build-dir> --config Release --target inferdeck-provider-echo
```

A provider outside this repository needs only the header:

```cmake
add_library(my-provider MODULE my_provider.cpp)
target_include_directories(my-provider PRIVATE <InferDeck>/libs/provider/include)
target_compile_definitions(my-provider PRIVATE INFERDECK_PROVIDER_BUILD)
```

Defining `INFERDECK_PROVIDER_BUILD` before including the header declares the
entry point with the right export attribute.

For another language, export a C-ABI function named `inferdeck_provider_v1`
that returns a pointer to a struct laid out as `inferdeck_provider`. In Rust
that is `#[no_mangle] pub extern "C" fn` with `#[repr(C)]` structs. In Swift it
is `@_cdecl("inferdeck_provider_v1")`.

## Checking a plugin

Run the checker before installing a library. It loads the library the same way
the gateway does, then for every modality in the manifest creates an instance,
loads it, sends one small request, unloads it, and loads it again.

```bash
inferdeck-provider-check my-provider.dll --model C:/models/model.gguf --artifact key=value
```

| Option | Meaning |
|---|---|
| `--model <path>` | Sets the model's `model_path`. |
| `--artifact key=value` | Adds one `artifacts` entry. Repeatable. |
| `--modality <name>` | Check only this modality. Repeatable. |
| `--skip-invoke` | Lifecycle only; send no request. Use for slow media models. |

It prints one `PASS` or `FAIL` line per step, with the provider's own error
text on a failure, and exits with 0 only when everything passed.

`libs/provider/tests/test_plugin_provider.cpp` drives the echo provider through
the same adapter and is the place to add a regression test.

For a live check, start a gateway on a spare port with a config that contains
only your model, then call `POST /v1/chat/completions` (or the media route for
your modality).

## Adding a built-in provider

1. Implement `IBackend` (plus `IModel` for text, or the matching media
   interface) in its own library under `libs/`.
2. Add its contract to `standard_runtime_contracts()` in
   `libs/model/include/model/runtime_contract.hpp`.
3. Add one `ProviderDescriptor` to `builtin_providers()` in
   `apps/inferdeck-gateway/src/builtin_providers.ipp`, or to
   `native_runtimes::providers()` for a media runtime.

Nothing else in the gateway needs to change.

## Upgrading a built-in runtime

Most built-in runtimes are pinned git submodules that share one ggml with
llama.cpp. An upgrade is a pin change plus proof that nothing else moved.

1. Change one pin per pull request and record the old and new revisions.
2. Configure. The build stops if stable-diffusion.cpp and InferDeck disagree
   on `GGML_MAX_NAME`. Change the value in the root `CMakeLists.txt` and in
   `scripts/Test-ArchitecturePolicy.ps1` together.
3. Build everything, not only the runtime you touched. A new stable-diffusion.cpp
   can need ggml functions the pinned llama.cpp does not have; that shows up
   as a compile error in stable-diffusion.cpp. Either pick an older revision or
   move llama.cpp as well.
4. Run `ctest -L "unit|integration"` and `scripts/Test-ArchitecturePolicy.ps1`.
5. Run one real request per affected modality on the GPU, on a test gateway
   with a spare port, not on the live service. Unit tests stub the native
   libraries and cannot see an ABI mismatch: the 2026-11 stable-diffusion.cpp
   upgrade passed every test and aborted on its first real image.
6. Read the gateway log for `diffusion_runtime` warnings and errors. The
   native runtime reports memory fallbacks and failed stages there.
7. Update `docs/release-reproducibility.md` with the new revision.

## What the plugin ABI does not cover

These llama.cpp-specific features stay with the built-in `llama_cpp` provider:
profile benchmarking and optimisation, GGUF quantisation, the automatic context
pool, slot resizing, and MTP speculative decoding. A plugin gets fixed slots
and the configured VRAM estimate. Token log-probabilities are not carried over
the ABI.

## Versioning

The entry point name carries the major ABI version. A change that would break
existing providers ships as `inferdeck_provider_v2` beside v1. Within v1, new
JSON keys may appear in requests and are safe to ignore.
