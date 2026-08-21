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
