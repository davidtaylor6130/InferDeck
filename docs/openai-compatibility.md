# OpenAI compatibility baseline

InferDeck Core exposes a strict OpenAI-compatible data plane at `/v1`.
The contract snapshot is pinned to 2026-08-20 and is tested with:

- OpenAI Python SDK 3.3.1
- OpenAI JavaScript SDK 7.5.0

Only routes listed in `gateway/route_manifest.hpp` belong to the strict data
plane. InferDeck operational metadata and mutations use `/api/inferdeck/v1`.
OpenAI-derivative and non-OpenAI routes are not registered by Core.

The pinned versions are compatibility fixtures, not floating minimums. A
baseline update must change the manifest fixture, SDK contract locks, schema
snapshots, and this document together, then pass both official SDK suites.

For every endpoint InferDeck exposes, compatibility is field-complete against
the pinned SDK schema. InferDeck does not define an arbitrary subset of official
fields and call that strict compatibility. A field may produce a model-specific
OpenAI error only when the selected native model genuinely cannot provide the
requested capability; fields with native llama.cpp semantics are implemented.

## Cross-cutting semantics

The requested public model identity is returned to the client. A configured
alias resolves to one concrete backend with a capability and minimum-context
contract; the requested alias and resolved model are both retained in canonical
observability, while strict responses expose only the public identity.

Errors use the OpenAI envelope with stable `type`, `code`, `message`, and
`param` behavior. Shape and value validation names the rejected top-level field
and completes before model resolution, queueing, residency changes, or slot
acquisition. Valid requests for an unavailable model feature are resolved first
and return `unsupported_capability` without queueing or admission.
Internal cancellation accounting is never substituted directly for an HTTP
status.

Chat streaming emits OpenAI chat completion chunks and terminates with exactly
`data: [DONE]`. Chat and Responses streams include randomized `obfuscation` by
default and honor `stream_options.include_obfuscation: false`. Responses
streaming emits typed Responses events from the
canonical output stream and one terminal completion event. A disconnect
requests native cancellation, completes request accounting once, releases its
slot once, and does not retain response state.

`GET /v1/models` returns only `id`, `object`, `created`, and `owned_by`.
Runtime availability, residency, VRAM, slots, pricing, aliases, and optimization
metadata belong to authenticated control routes.

`POST /v1/embeddings` accepts a string, string array, token-ID array, or
token-ID matrix. Float output and little-endian base64 float encoding are
supported. Unsupported dimensions, encoding formats, extra fields, and input
limits fail before admission.

Run the pinned client contracts with:

```powershell
pnpm test:openai-contract
python -m pip install -r Testing/requirements-openai-contract.txt
python -m unittest -v Testing.openai_sdk_contract_test
```

Both suites exercise every route in the strict manifest through the official
client request serializers and response parsers. Chat Completions and Responses
also cover their supported streaming forms.

The C++ route suite locks every successful strict response shape to
`tests/fixtures/openai_schema_snapshots.json`. It also sends malformed requests
to every strict generation endpoint and verifies that jobs, queue depth, model
residency, active requests, metrics, and request history remain unchanged.
Typed request decoders attach the failing top-level field to the standard
`error.param` member; transport and model-resolution failures keep it null when
there is no request field to identify.

## Strict Chat Completions contract

POST /v1/chat/completions requires a non-empty model and message array and
decodes developer, system, user, assistant, and tool messages in their original
order. Strict content parts use Chat Completions names (`text` and
`image_url`); Responses-only content tags are rejected. Nested content, tool,
tool-call, tool-choice, response-format, stop, token-limit, sampling, seed, and
stream-option shapes are validated before model resolution or slot admission.
`frequency_penalty` and `presence_penalty` accept the OpenAI range from -2.0
through 2.0 and map directly to llama.cpp's native frequency and presence
penalty sampler inputs.
`logit_bias`, token `logprobs`, and `top_logprobs` map to native llama.cpp
sampling data and are returned in OpenAI Chat output shapes. Deprecated
function fields and function-role history translate to canonical function tools.
`prompt_cache_key` provides stable native cache affinity.

The response `model` is the requested public identity, including an alias, in
both streamed and non-streamed output; the resolved backend identity remains
internal operational metadata. Native priority, template kwargs, and
`reasoning_content` are not part of strict `/v1`. They remain available only
through the explicitly enabled OpenAI-derivative profile, and strict output
never emits derivative reasoning fields.

Open WebUI may forward its advanced model settings at the top level even when
the target is an OpenAI-compatible connection. Core therefore recognizes the
complete Open WebUI advanced request set without adding routes or response
fields to the pinned OpenAI contract. `num_ctx` is a native per-request context
ceiling and cannot exceed the selected model's configured context.
`num_predict`, `top_k`, `min_p`, repetition controls, Mirostat controls,
`think`, and `format` map to canonical native generation settings. Load-time
hints such as `num_batch`, `num_thread`, `num_gpu`, `use_mmap`, and
`use_mlock` are type-validated compatibility hints; model loading remains
owned by InferDeck's configured single-process runtime. Unknown request fields
still fail before model resolution or admission.

## Native Audio contract

POST /v1/audio/speech supports the pinned OpenAI request shape with native
runtime limits. Input is capped at 4096 Unicode characters, speed must be from
0.25 through 4.0, voice strings and `{ "id": ... }` references are validated,
and `stream_format: audio` is supported. Empty instructions are a harmless
default. Non-empty instructions, SSE speech events, and response formats that a
selected native model cannot produce return `unsupported_capability` after
model resolution and before slot admission.

POST /v1/audio/transcriptions accepts one file and one model plus language,
prompt, temperature, response format, `stream: false`, and segment timestamp
granularity for `verbose_json`. JSON, text, verbose JSON, SRT, and VTT outputs
are supported. Streaming, word timestamps, logprob inclusion, diarized output,
server-side chunking/VAD, keywords, language lists, and known-speaker fields are
recognized, validated, and reported as selected-model capability errors because
the native Whisper contract cannot produce those semantics. Client-visible
cancellation uses HTTP 408 with the
OpenAI error envelope; internal request accounting retains status 499 and the
media job retains its `cancelled` state.

## Native Images contract

POST /v1/images/generations accepts the pinned OpenAI request field names.
The native runtime produces non-streamed PNG data, so strict mode accepts
compatible defaults (background: auto, moderation: auto, output_format: png,
quality: auto, response_format: b64_json, partial_images: 0, and stream:
false). `n` supports the pinned OpenAI range from 1 through 10. Valid requests
for another format, quality/style/background semantics, compression, URL output,
or image streaming return `unsupported_capability`; invalid enum/range values
remain request errors. Native-only sampler controls remain available only in the
explicitly enabled derivative profile.

## Stateless Responses contract

POST /v1/responses decodes directly into the canonical inference domain. It
does not construct a Chat Completions request or call the Chat adapter.
String and item-array inputs preserve developer/system roles, function calls,
function-call outputs, text/image content, tools, sampling, reasoning effort,
and structured-output configuration.

InferDeck is stateless: store and background are accepted natively as false.
Conversation state, previous-response continuation, hosted prompts, background
work, storage, context compaction, moderation, built-in service tools, extended
cache retention, automatic truncation, and non-default service tiers are shape
validated and then return selected-model capability errors. Function tools,
structured output, sampling, native prompt-cache affinity, token logprobs, and
the `message.output_text.logprobs` include value are implemented. One response
ID, requested model identity, and
creation timestamp are reused throughout each streaming response.
