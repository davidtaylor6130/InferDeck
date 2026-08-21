# ADR 0001: OpenAI-only Core architecture boundary

- Status: Accepted
- Date: 2026-08-20
- Owners: @davidtaylor6130
- Related issues: #99, #103
- Supersedes: None

## Context

InferDeck is intended to be a local, native Windows AI gateway that embeds its
runtimes in one executable. The current repository has accumulated endpoints,
wire formats, administration operations, and a Python forced-alignment service
that do not belong to the same product boundary. Some HTTP handlers also pass
raw wire JSON far enough into the runtime path that transport compatibility can
change backend behavior.

That combination makes OpenAI compatibility difficult to prove and allows a
feature to bypass the coordinator, lifecycle, security, and observability rules
that protect the rest of the gateway.

## Decision

InferDeck Core is one native executable with a strict OpenAI API and a separate
InferDeck control plane.

1. Core does not launch or proxy subprocesses or runtime servers. Native runtime
   libraries execute in-process and participate in coordinated shutdown.
2. `/v1` is reserved for endpoints and wire formats defined by the pinned OpenAI
   contract. InferDeck operations move to `/api/inferdeck/v1`.
3. Non-OpenAI protocols and products are excluded from Core. A compatibility
   product must be independently named, default-off, and unable to modify the
   strict Core contract.
4. HTTP adapters validate and translate requests into a typed, protocol-neutral
   inference domain. Backends never receive HTTP objects or raw wire JSON.
5. Validation and capability negotiation finish before admission, model loading,
   swapping, job creation, or slot acquisition.
6. Every runtime operation uses the coordinator for admission, residency,
   cancellation, deadlines, and shutdown, and emits canonical observability.
7. Unsupported inputs produce deterministic OpenAI-shaped errors. The gateway
   does not silently substitute formats, ignore fields, or expose scheduling
   priority from an untrusted request body.
8. Compatibility behavior is isolated in explicit profiles. Strict OpenAI
   behavior is the default and remains independently testable.

## Boundary impact

### Protocol

OpenAI Chat Completions, Responses, audio, model, error, and streaming contracts
will be tested as independent adapters. Anthropic routes, forced alignment, swap,
health, metrics, and stats are not OpenAI endpoints and therefore cannot remain
under `/v1`.

### Process

The shipping Core remains a single Windows executable. Python, FastAPI, Uvicorn,
`llama-server`, and other sidecar or proxy processes are outside the Core product
and release lifecycle.

### Resource and runtime

All inference and media work uses typed runtime interfaces and explicit compute,
model-role, admission, residency, cancellation, and deadline policies. Inference
does not hold the coordinator mutex, and active leases prevent backend removal.

### Security

Administrative routes use the authenticated `/api/inferdeck/v1` control plane.
Public OpenAI data-plane authorization does not implicitly grant model lifecycle,
configuration, metrics, logs, or shutdown authority.

### Observability

Adapters and runtimes report a canonical request identity, outcome, token usage,
latency, cancellation state, model role, and protocol name through the shared
observability path.

## Alternatives considered

- Treat every OpenAI-like or vendor-specific route as Core. Rejected because it
  prevents a stable, testable OpenAI contract.
- Keep companion services in the repository and present them as one product.
  Rejected because separate processes violate the native lifecycle invariant.
- Let each backend parse protocol JSON. Rejected because transport concerns then
  leak into model execution and create divergent validation behavior.

## Consequences

The boundary reduces the Core surface and makes compatibility testable. It also
requires route migration, typed-domain work, companion-product extraction, and a
versioned control plane. Some existing clients will need a documented migration
or an explicitly separate compatibility layer.

## Migration and rollback

Work is ordered by [overhall_plan.md](../../overhall_plan.md). Destructive source
extraction and live deployment each require separate owner approval. Every phase
uses a focused branch and pull request, maintains rollback notes, and preserves
existing data before schema or filesystem changes.

Rollback restores the last verified release as a matched executable, static
bundle, configuration, and database set. It must not restore non-OpenAI routes
under `/v1` after the strict contract release is declared stable.

## Verification

Completion requires architecture linting, route inventory tests, pinned OpenAI
contract fixtures, negative schema tests, runtime lifecycle tests, security
tests, Release builds, dashboard tests, migration tests, and full live-path
release evidence defined by the overhaul plan.
