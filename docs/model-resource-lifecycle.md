# Model resource and lifecycle contract

InferDeck keeps every runtime in the gateway process. Model metadata determines
resource ownership; runtime names and modalities do not control coordinator
lifecycle once explicit metadata is present.

## Required resource metadata

An explicitly configured model supplies all of these fields together:

| Field | Values or meaning |
|---|---|
| `role` | `conversation`, `helper`, `media`, `embedding`, `maintenance` |
| `compute` | `cpu`, `vulkan_gpu`, `cuda_gpu`, `rocm_gpu`, `mixed` |
| `residency` | `always`, `managed`, `on_demand` |
| `admission_pool` | Name of the shared concurrency pool |
| `concurrency_limit` | Maximum active leases across that pool |
| `memory_required_mb` | Required host memory |
| `eviction_eligible` | Whether capacity preparation may evict it |

Configuration rejects partial resource declarations, pool members with
different limits, always-resident models that are eviction eligible, GPU
helpers, non-resident helpers, CPU always-resident conversation models, and
embedding roles without embedding capability.

Legacy configurations are normalized once at the registry boundary. Their
admission pool is the model ID, preserving the former per-model concurrency
behavior. New configuration should use explicit metadata; legacy inference is
a migration path rather than coordinator policy.

## Identity model

The coordinator distinguishes five identities:

- requested: the ID supplied by the client;
- resolved: the concrete registry target after alias resolution;
- selected: the conversation model selected for default chat work;
- resident: every loaded backend, including CPU helpers and media runtimes;
- executing: models with active slot leases.

`GET /api/inferdeck/v1/swap/status` exposes selected, resident, and executing
sets under `identities`. Request metrics retain requested and resolved IDs.
Loading a helper or media backend cannot replace the selected conversation
model. Always-resident and non-eviction-eligible backends are excluded from GPU
capacity eviction.

## Admission and lifecycle

Slot leases are coordinator-owned and idempotently released. An explicit
admission pool applies its concurrency limit across fixed-capacity member models.
A llama.cpp model opting into `concurrency_auto` uses its hardware-fitted sequence
capacity instead of that pool limit; shared KV admission still checks each request's
prompt/output budget. Leave automatic concurrency disabled when a fixed shared
quota or a single-request exclusive profile is required. Queue waiting, lifecycle-lock acquisition, capacity resize, eviction, drain, unload,
and load receive one steady-clock deadline and cancellation predicate.
Rollback uses a separate bounded recovery window so expiry of the failed
operation cannot suppress restoration. llama.cpp model loading connects the
operation predicate to its native progress callback.

Backends must implement the lifecycle-control overloads when an operation can
block. The compatibility overload checks before and after synchronous work,
but cannot interrupt an opaque third-party call by itself.

GPU capacity admission prefers a fresh observed used/total VRAM sample. It
reserves the configured safety margin plus any lazy runtime allocation not yet
represented in that sample. Live headroom is accepted only when every resident
GPU runtime reports complete accounting. Invalid or lifecycle-invalidated
telemetry falls back to declared model footprints. With fresh telemetry and an
incomplete runtime, the lower of declared availability and observed headroom
with that runtime's full declared footprint reserved is used.
Models in different admission pools may execute concurrently once resident;
native load, resize, and eviction operations remain serialized.

## Voice sessions

Voice priority is internal coordinator policy. A reservation requires both a
valid bearer principal and `X-InferDeck-Voice-Session`, an opaque 8-128
character identifier containing letters, digits, `.`, `_`, or `-`. The
internal key combines the authenticated principal and opaque session. There is
no source-IP fallback, so clients sharing an address cannot affect each
other's reservation.

## Needle 2 decision record

The 2026-08-21 revalidation found Apache-2.0 licensing and an in-process C ABI,
but the official Windows x64 package supplies a Clang/libc++ `libneedle.a` whose
objects require GNU exception ABI and libc++ symbols absent from the MSVC
InferDeck build. The package also supplies `needle.exe`; InferDeck cannot use it
because subprocess integration violates the single-process architecture.

Needle remains disabled until upstream supplies an MSVC-compatible pinned
library with complete runtime dependencies, or source that builds within the
pinned InferDeck toolchain. Issue #99 records artifact sizes, symbols, hashes,
and upstream links. No Needle artifact is shipped by InferDeck Core.

## Request demand and idle growth

A resident llama.cpp request is rendered and tokenized before lease acquisition. Its context and sequence demand is retained while queued, while active leases retain their own reservations. Fitting requests acquire normally and can run together. Growth is performed only after active leases finish, because context recreation clears KV and recurrent caches. Context growth is geometric and bounded by the configured model context, native sequence limit, batch capacity, and actual allocation result. An unspecified `max_tokens` reserves the remaining requested context budget. For `concurrency_auto`, status `slots` reports currently allocated sequences. Idle reclamation can reduce the context pool to its initial batch-sized minimum; recreation discards retained caches.

If a native model load runs out of memory after admission, managed loading can reclaim an idle context and retry once under the original deadline. This retry does not evict another model.
