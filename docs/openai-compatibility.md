# OpenAI compatibility baseline

InferDeck Core exposes a strict OpenAI-compatible data plane at `/v1`.
The contract snapshot is pinned to 2026-08-20 and is tested with:

- OpenAI Python SDK 3.3.1
- OpenAI JavaScript SDK 7.5.0

Only routes listed in `gateway/route_manifest.hpp` belong to the strict data
plane. InferDeck operational metadata and mutations use `/api/inferdeck/v1`.
Optional OpenAI-derivative behavior is isolated under
`/compat/openai-derivative/v1` and is disabled unless configured explicitly.

The pinned versions are compatibility fixtures, not floating minimums. A
baseline update must change the manifest fixture, SDK contract locks, schema
snapshots, and this document together, then pass both official SDK suites.

## Cross-cutting semantics

The requested public model identity is returned to the client. A configured
alias resolves to one concrete backend with a capability and minimum-context
contract; the requested alias and resolved model are both retained in canonical
observability, while strict responses expose only the public identity.

Errors use the OpenAI envelope with stable `type`, `code`, `message`, and
`param` behavior. Validation names the rejected top-level field and completes
before model resolution, queueing, residency changes, or slot acquisition.
Internal cancellation accounting is never substituted directly for an HTTP
status.

Chat streaming emits OpenAI chat completion chunks and terminates with exactly
`data: [DONE]`. Responses streaming emits typed Responses events from the
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

The response `model` is the requested public identity, including an alias, in
both streamed and non-streamed output; the resolved backend identity remains
internal operational metadata. Native priority, sampler controls, template
kwargs, and `reasoning_content` are not part of strict `/v1`. They remain
available only through the explicitly enabled OpenAI-derivative profile, and
strict output never emits derivative reasoning fields.

## Native Audio contract

POST /v1/audio/speech supports the pinned OpenAI request shape with native
runtime limits. Input is capped at 4096 Unicode characters, speed must be from
0.25 through 4.0, voice strings and `{ "id": ... }` references are validated,
and `stream_format: audio` is supported. Empty instructions are a harmless
default; non-empty instructions and SSE speech streaming are rejected because
the linked native runtimes cannot preserve those semantics. Each runtime also
rejects response formats it cannot encode before slot admission.

POST /v1/audio/transcriptions accepts one file and one model plus language,
prompt, temperature, response format, `stream: false`, and segment timestamp
granularity for `verbose_json`. JSON, text, verbose JSON, SRT, and VTT outputs
are supported. Streaming, word timestamps, logprob inclusion, diarized output,
server-side chunking/VAD, keywords, language lists, and known-speaker fields are
rejected before model resolution because the native Whisper contract cannot
produce those semantics. Client-visible cancellation uses HTTP 408 with the
OpenAI error envelope; internal request accounting retains status 499 and the
media job retains its `cancelled` state.

## Native Images contract

POST /v1/images/generations accepts the pinned OpenAI request field names.
The native runtime produces non-streamed PNG data, so strict mode accepts
compatible defaults (background: auto, moderation: auto, output_format: png,
quality: auto, response_format: b64_json, partial_images: 0, and stream:
false). It rejects requests for another format, quality/style/background
semantics, compression, URL output, or image streaming before model resolution
or admission. Native-only sampler controls remain available only in the
explicitly enabled derivative profile.

## Stateless Responses contract

POST /v1/responses decodes directly into the canonical inference domain. It
does not construct a Chat Completions request or call the Chat adapter.
String and item-array inputs preserve developer/system roles, function calls,
function-call outputs, text/image content, tools, sampling, reasoning effort,
and structured-output configuration.

InferDeck is stateless: store and background are accepted only as false;
conversation, previous_response_id, and cache-control objects must be null;
include must be empty; truncation must be disabled; and service_tier accepts
only auto or default. Unsupported stateful semantics fail before model
resolution or admission. One response ID, requested model identity, and
creation timestamp are reused throughout each streaming response.
