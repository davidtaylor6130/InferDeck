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
admission pool applies its concurrency limit across every member model. Queue
waiting, lifecycle-lock acquisition, capacity resize, eviction, drain, unload,
and load receive one steady-clock deadline and cancellation predicate.
Rollback uses a separate bounded recovery window so expiry of the failed
operation cannot suppress restoration. llama.cpp model loading connects the
operation predicate to its native progress callback.

Backends must implement the lifecycle-control overloads when an operation can
block. The compatibility overload checks before and after synchronous work,
but cannot interrupt an opaque third-party call by itself.

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
