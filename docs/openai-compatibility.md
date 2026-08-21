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
