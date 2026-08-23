# InferDeck OpenAI-Core Overhaul Plan

## Mission

Complete a controlled overhaul of InferDeck so that:

- InferDeck Core is one native Windows executable with no runtime subprocesses,
  proxy servers, Python services, or orphan-process risk.
- `/v1` exposes only OpenAI-defined endpoints and wire formats.
- OpenAI-compatible derivative behavior is explicit, isolated, opt-in, and
  incapable of changing the strict OpenAI contract.
- InferDeck administration uses a separately authenticated control plane.
- Protocol adapters translate into a typed, protocol-neutral inference model.
- Every runtime participates in the same admission, residency, cancellation,
  observability, and shutdown lifecycle.
- Architecture rules are enforced by required CI checks instead of relying on
  documentation alone.

This file is the source of truth for the tracked goal. The goal is complete only
when every required checkbox and the final definition of done are satisfied.

## Baseline

- Baseline branch: `main`
- Baseline commit: `d1760c7f9fcad0940faa66944c75294a69b2fd39`
- Planning branch: `codex/openai-core-overhaul-plan`
- Initial GitHub issues: #99 and #103
- Initial draft PRs: #104 and #105, both addressing #103
- Audit mode: source, documentation, OpenAI contract, GitHub issue, and PR review
- Implementation rule: one phase or tightly coupled workstream per branch and PR

## Authoritative GitHub mapping

| Scope | Issue |
|---|---:|
| Overhaul epic | #107 |
| Phase 0 - Governance | #108 |
| Phase 1 - Forced-aligner extraction | #109 |
| Phase 2 - Security containment | #110 |
| Phase 3 - Strict `/v1` boundary | #111 |
| Phase 4 - Typed inference domain | #112 |
| Phase 5 - Chat Completions | #113 |
| Phase 6 - Responses | #114 |
| Phase 7 - Resource roles | #115 |
| Phase 8 - Audio | #116 |
| Phase 9 - Lifecycle | #117 |
| Phase 10 - Configuration | #118 |
| Phase 11 - Observability and pricing | #119 |
| Phase 12 - Build and CI | #120 |
| Phase 13 - Decomposition and documentation | #121 |
| Phase 14 - Migration and release | #122 |

Epic #107 defines the milestone order. Each phase issue records its predecessor
in `Blocked by`, so the dependency chain remains explicit even without relying
on a repository-specific project-board configuration.

## Non-negotiable invariants

1. Core remains a single native process.
2. Core never calls `system`, `popen`, `subprocess`, `Start-Process`, or launches
   a model/runtime server.
3. Core never proxies `llama-server`, vLLM, FastAPI, Uvicorn, or another inference
   service.
4. `/v1` is reserved for a pinned OpenAI contract.
5. Backends receive typed domain objects, never HTTP requests or protocol JSON.
6. Validation and capability checks finish before job creation, model loading,
   swapping, queue admission, or slot acquisition.
7. Unsupported behavior returns a deterministic OpenAI-shaped error and is never
   silently substituted or ignored.
8. Scheduler priority is trusted policy, never a public request-body field.
9. All runtime work uses the coordinator and canonical observability.
10. Inference never holds the coordinator mutex.
11. Active-request leases prevent backend destruction during execution.
12. Cancellation and deadlines propagate through admission, drain, unload, load,
    inference, streaming, and shutdown.
13. No destructive extraction or deletion is performed without a separate,
    explicit owner approval at the phase gate.
14. The live `C:\InferDeck` installation is not changed until a deployment phase
    explicitly authorizes and verifies it.

## Target architecture

```text
OpenAI client
    |
    v
HTTP transport and request identity
    |
    v
Strict OpenAI route adapter
  - schema validation
  - capability negotiation
  - error serialization
  - response/SSE serialization
    |
    v
Typed canonical inference domain
  - RequestContext
  - InputItem / Instruction / ToolDefinition
  - GenerationOptions
  - OutputEvent / Usage / RequestOutcome
  - DomainError
    |
    v
GenerationSession and admission policy
    |
    v
BackendCoordinator
  - queue and deadlines
  - resource planning
  - residency and slots
  - cancellation and shutdown
    |
    v
Typed native runtime interfaces

React dashboard
    |
    v
Authenticated /api/inferdeck/v1 control plane
    |
    +--> configuration repository
    +--> model store
    +--> operations and observability
```

Incompatible protocols such as Anthropic are not part of Core. A future
compatibility product must be independently named, default-off, and consume the
same public OpenAI interface or a stable native adapter boundary without
modifying Core authentication, routing, scheduling, or schemas.

## Severity and completion rules

- P0: release blocker or direct violation of a non-negotiable invariant.
- P1: correctness, reliability, security, or architectural enforcement gap.
- P2: compatibility, observability, maintainability, or reproducibility debt.
- P3: cleanup that becomes safe after the new boundaries exist.

Each phase must finish with:

- [x] focused unit tests;
- [x] applicable integration and contract tests;
- [x] Release build;
- [x] dashboard tests/build when affected;
- [x] source and documentation truth check;
- [x] reviewed diff with unrelated work excluded;
- [x] branch pushed and PR opened or updated;
- [x] required CI green;
- [x] linked GitHub issues updated with evidence.

## Phase 0 - Governance freeze and authoritative backlog

### Purpose

Prevent further boundary drift while the overhaul is in progress and turn the
audit into an authoritative, dependency-ordered backlog.

### Work

- [x] Adopt an ADR declaring the OpenAI-only Core, single-process rule, control
      plane boundary, canonical inference domain, and compatibility policy.
- [x] Add an architecture decision template covering protocol, process, resource,
      security, observability, migration, and contract-test impact.
- [x] Create a GitHub epic for this plan.
- [x] Create one issue for every independently deliverable work item below.
- [x] Label issues with `P0` through `P3`, `protocol`, `security`, `architecture`,
      `runtime`, `observability`, `build`, and `migration` as applicable.
- [x] Add issue dependencies and assign milestone order matching this plan.
- [x] Record #99 as blocked by Phase 7's explicit resource-role model.
- [x] Record #103 as an early correctness fix before broad configuration changes.
- [x] Select PR #105 as the candidate for #103 after diff review.
- [x] Preserve PR #104; close or supersede it only after owner authorization and
      preservation of any unique work.
- [x] Add CODEOWNERS coverage for OpenAI contracts, coordinator/runtime boundaries,
      authentication, configuration, and release workflows.
- [x] Add a PR template that requires invariant and contract impact declarations.

### Exit gate

- [x] Every item in this plan maps to an open or completed GitHub issue.
- [x] The epic ordering matches this document.
- [x] No untracked P0 or P1 finding remains outside the backlog.

## Phase 1 - Forced-aligner containment and extraction

### Decision

The current Python/FastAPI/Qwen/PyTorch/ROCm forced aligner is a companion
service, not InferDeck Core. Exact forced alignment is not an OpenAI API and the
current implementation cannot satisfy the native single-process rule.

### Work

- [x] Inventory every forced-aligner source, configuration, deployment script,
      runtime path, service/watchdog, documentation link, test asset, and live
      dependency.
- [x] Confirm whether any unique user data or secrets exist outside tracked source.
- [x] Define the companion product name, ownership, release lifecycle, and API
      status without presenting `/v1/audio/alignments` as OpenAI.
- [x] Preserve the complete feature history in a tag, archive, or companion
      repository before removing Core ownership.
- [x] Obtain explicit owner approval before moving or deleting tracked files.
- [x] Remove `apps/forced-aligner` from InferDeck Core after the preservation gate.
- [x] Remove forced-aligner service YAML and Windows launcher/watchdog/install
      scripts from Core after the preservation gate.
- [x] Remove Core documentation and tests that imply the sidecar is a native
      InferDeck runtime.
- [x] Remove live-deployment assumptions from the Core release and support matrix.
- [x] Prove that Core contains no Python service, FFmpeg subprocess, watchdog, or
      secondary inference listener.
- [x] Add a source-policy test that fails on secondary runtime/inference
      process-launch primitives in Core source and feature-specific service
      scripts. Explicitly allow only the documented boot launcher that starts the
      single gateway executable.

### Exit gate

- [x] `rg` finds no Core process-launch path.
- [x] Core build and tests do not require Python, PyTorch, ROCm, FFmpeg, FastAPI,
      Uvicorn, Docker, NSSM sidecars, or watchdog scripts.
- [x] Any retained companion is clearly outside the Core product and repository
      contract.

## Phase 2 - Control-plane security boundary

### Work

- [x] Classify every route as OpenAI data plane, read-only public status, dashboard
      session, or privileged control plane.
- [x] Introduce route-scoped principals rather than one global authentication
      wrapper.
- [x] Require control-plane authentication for every mutating operation.
- [x] Default privileged control routes to loopback-only access.
- [x] Require an explicit `control.allow_remote` setting, a non-empty control token,
      and a configured origin allowlist before remote administration can start.
- [x] Keep OpenAI data-plane authentication separately configurable.
- [x] Remove global `x-api-key` fallback.
- [x] Add constant-time credential comparison and secret masking.
- [x] Restrict control-plane CORS; prohibit wildcard origins for privileged routes.
- [x] Treat ambient loopback authority as a CSRF credential: enforce exact
      mutation origins, Fetch Metadata, and non-simple JSON mutation requests.
- [x] Apply authorization to model download, archive, permanent removal, aliases,
      config read/write/reset/reload, model load/unload, logs, jobs, cancellation,
      and benchmark operations.
- [x] Add endpoint-specific request-body limits and content-type enforcement.
- [x] Add read, write, idle, and slow-client deadlines.
- [x] Generate or accept a sanitized request ID, return it in response headers, and
      include it in every structured log and request outcome.
- [x] Update LAN configuration tests so insecure remote administration cannot be
      accidentally locked in as expected behavior.

### Exit gate

- [x] Non-loopback plus unauthenticated control startup fails validation.
- [x] Every mutating control endpoint returns 401/403 without the correct principal.
- [x] OpenAI credentials cannot administer the control plane unless explicitly
      granted that principal.
- [x] Browser-origin and request-size/deadline matrices pass.

## Phase 3 - Immediate OpenAI contract hotfixes

### Chat streaming

- [x] Emit `usage: null` on every ordinary Chat streaming chunk when
      `stream_options.include_usage` is enabled.
- [x] Emit a distinct final usage chunk with `choices: []` before `[DONE]`.
- [x] Keep the finish-reason chunk separate from the usage-only chunk.
- [x] Add byte-exact golden tests for content, reasoning-profile, tool-call, finish,
      usage, error, cancellation, and `[DONE]` ordering.

### Speech

- [x] Stop silently rewriting explicit MP3 requests to WAV.
- [x] Return an OpenAI-shaped 400 `unsupported_response_format` until a native
      in-process encoder exists.
- [x] Enforce the official 4,096-character input limit by Unicode character count.
- [x] Validate `model`, `input`, `voice`, `speed`, `instructions`,
      `response_format`, and `stream_format` before admission.
- [x] Remove the regression test that expects MP3 input to return WAV and replace it
      with the strict contract matrix.

### Pricing issue #103

- [x] Review PR #105 against current main and this plan's schema boundaries.
- [x] Verify separate uncached-input, cached-input, and output rates.
- [x] Clamp cached tokens to prompt tokens.
- [x] Verify legacy Qwen estimation is date-bounded, deterministic, and identical
      across overview and usage pages.
- [x] Run dashboard, C++, forced historical fixture, build, and live-safe validation.
- [x] Merge the selected implementation before canonical config refactoring.
- [x] Close #103 only after matching source, dashboard bundle, deployment artifact,
      and calculation evidence.

### Exit gate

- [x] Official OpenAI SDK consumers parse the corrected Chat stream.
- [x] No requested audio format is silently substituted.
- [x] #103 has one canonical merged implementation and no duplicate active PR.

## Phase 4 - Clean API namespaces and compatibility profiles

### Strict `/v1` surface

- [x] Create a route manifest for the pinned OpenAI compatibility baseline.
- [x] Limit `/v1` to supported OpenAI endpoints:
  - [x] `GET /v1/models`
  - [x] `POST /v1/chat/completions`
  - [x] `POST /v1/responses`
  - [x] `POST /v1/embeddings`
  - [x] `POST /v1/images/generations`
  - [x] `POST /v1/audio/speech`
  - [x] `POST /v1/audio/transcriptions`
- [x] Move health, metrics, history and swap operations to
      `/api/inferdeck/v1/*`.
- [x] Update dashboard calls, documentation, fixtures and operational scripts.
- [x] Remove `/v1/swap/*`, `/v1/metrics`, `/v1/stats/history`, and `/v1/health`
      after the migration window.
- [x] Remove `/v1/messages` and `/v1/messages/count_tokens` from Core.
- [x] Remove Anthropic aliases and `x-api-key` behavior from Core configuration.
- [x] Preserve Anthropic source history before deletion and obtain explicit owner
      approval for destructive removal.

### Derivative OpenAI profiles

- [x] Define `strict_openai` as the default and production-supported profile.
- [x] If a derivative profile is still required, give it a separate base path or
      listener and a named compatibility manifest.
- [x] Move `reasoning_content`, sampler extensions, and other derivative fields out
      of strict mode.
- [x] Prohibit derivative profiles from changing shared auth, scheduling, errors,
      model identity, or strict serializers.

### Exit gate

- [x] A route-snapshot test proves every `/v1` route belongs to the pinned OpenAI
      manifest.
- [x] The strict profile contains no InferDeck or Anthropic top-level fields.
- [x] Dashboard and operational workflows use the new control paths.

## Phase 5 - Typed canonical inference domain

### New domain boundary

- [x] Introduce a focused library for protocol-neutral inference contracts.
- [x] Define typed `RequestContext` with request ID, principal, endpoint, deadline,
      cancellation, requested model, resolved model, stream mode and compatibility
      profile.
- [x] Define typed instructions, messages/input items, text/image/audio content,
      function tools, tool choice, structured-output constraints and sampling.
- [x] Define typed `OutputEvent` variants for text, reasoning, tool calls, usage,
      finish, refusal, and error.
- [x] Define canonical `RequestOutcome` and `DomainError` taxonomies.
- [x] Define capability declarations independently of HTTP fields.
- [x] Remove `openai_body_json` from `InferenceRequest`.
- [x] Remove protocol parsing from `LlamaCppModel` and every runtime backend.
- [x] Move prompt-template conversion into a typed llama adapter layer.
- [x] Prohibit `httplib`, route headers, OpenAI field names, and protocol response
      objects from model and runtime libraries.

### Generation session

- [x] Extract one `GenerationSession` service shared by Chat and Responses.
- [x] Centralize slot ownership, cancellation, backpressure, inference-thread
      lifetime, finish-once behavior and request recording.
- [x] Preserve bounded queues, UTF-8 holdback, cursor sanitization and idempotent
      release semantics.

### Exit gate

- [x] Dependency tests prove model/runtime libraries contain no HTTP or protocol
      JSON dependency.
- [x] Golden adapter tests convert OpenAI requests to canonical objects and
      canonical events back to wire output.
- [x] Chat and Responses share execution lifecycle without calling one another's
      HTTP handlers.

## Phase 6 - Complete strict OpenAI adapters

### Common parsing and errors

- [x] Pin an OpenAI compatibility baseline date and supported SDK versions.
- [x] Create typed field decoders with field-specific errors and parameter names.
- [x] Validate complete request shape before resolution or resource work.
- [x] Centralize domain-error to OpenAI status/type/code/param mapping.
- [x] Enforce JSON media types and multipart requirements.
- [x] Explicitly reject unsupported semantic features; never silently ignore them.
- [x] Accept harmless standard values when their semantics can be preserved, such
      as `store: false` in a stateless implementation.

### Chat Completions

- [x] Require and validate `model` and `messages`.
- [x] Preserve developer/system instruction precedence.
- [x] Validate all supported content, tools, tool choice, response format, stop,
      token limits, temperature/top-p ranges, seed, streaming and stream options.
- [x] Remove `:latest` normalization.
- [x] Remove public `priority`, `top_k`, repeat controls and template kwargs.
- [x] Define strict behavior for unsupported official fields.
- [x] Return canonical model identity consistently in stream and non-stream modes.
- [x] Keep derivative reasoning output outside strict mode.

### Responses

- [x] Implement Responses directly over the canonical domain.
- [x] Support string and item-array input forms required by the pinned baseline.
- [x] Preserve developer messages and function-call/output pairing.
- [x] Support current function-call output content forms.
- [x] Generate one immutable response timestamp and model identity.
- [x] Produce complete, internally consistent Response objects.
- [x] Produce typed Responses SSE events directly from canonical output events.
- [x] Define explicit behavior for storage, background, conversation,
      previous-response, include, truncation, service-tier and cache fields.

### Embeddings

- [x] Support string, string-array, token-ID array and token-ID matrix input.
- [x] Validate dimensions, encoding format, user and input limits.
- [x] Encode base64 floats with explicit little-endian representation.
- [x] Remove public priority and reject unknown unsupported fields explicitly.

### Images

- [x] Validate the pinned official image request schema.
- [x] Remove proprietary top-level generation controls from strict mode.
- [x] Support or explicitly reject quality, style, background, output format,
      moderation, user and streaming-related fields.
- [x] Ensure response encoding and MIME behavior match the requested contract.

### Audio

- [x] Complete speech instructions and stream-format behavior.
- [x] Complete transcription response formats, timestamp granularities, includes,
      logprobs, chunking and streaming behavior for the supported baseline.
- [x] Remove fabricated transcription metrics such as constant compression ratio.
- [x] Keep internal cancellation outcomes separate from client-visible HTTP status.

### Models

- [x] Make `/v1/models` return only the strict OpenAI discovery fields.
- [x] Move runtime, VRAM, slots, pricing, optimization, alias contracts and
      residency detail to `/api/inferdeck/v1/models`.

### Exit gate

- [x] Pinned official Python and JavaScript SDK contract suites pass.
- [x] Stream and non-stream schema snapshots pass for every supported endpoint.
- [x] No malformed request changes jobs, queue, residency or active-request state.

## Phase 7 - Coordinator, lifecycle and issue #99

### Explicit model resource model

- [x] Replace modality-derived lifecycle rules with explicit metadata:
  - [x] `role`: conversation, helper, media, embedding, maintenance;
  - [x] `compute`: CPU, Vulkan GPU, CUDA GPU, ROCm GPU or mixed;
  - [x] `residency`: always, managed or on-demand;
  - [x] `admission_pool` and concurrency limit;
  - [x] memory requirements and eviction eligibility.
- [x] Separate default/selected model, requested model, resolved model, resident set
      and executing model in coordinator state and APIs.
- [x] Validate impossible role/compute/residency combinations in configuration.

### Deadlines, cancellation and shutdown

- [x] Replace ignored swap timeouts with one propagated deadline.
- [x] Propagate cancellation through queue wait, drain, resize, eviction, unload,
      load, inference and stream delivery.
- [x] Make residency changes transactional and restore the last usable state after
      failure where possible.
- [x] Replace polling plus unconditional join with a bounded shutdown state machine.
- [x] Add fake backends that block each lifecycle stage for deterministic tests.
- [x] Preserve the invariant that inference never holds the coordinator mutex.

### Voice reservations

- [x] Replace source-IP fallback with an explicit opaque session identity scoped to
      an authenticated principal.
- [x] Ensure clients sharing an IP cannot affect one another's priority session.
- [x] Keep voice priority internal rather than request-body controlled.

### Issue #99

- [x] Revalidate Needle 2 licensing, native integration feasibility, package size,
      quality and performance prerequisites against current requirements; record
      that the upstream GNU/LLVM archive cannot enter the pinned MSVC build.
- [x] Supersede the Needle-specific integration rather than adding a subprocess,
      proxy, second C++ runtime, or unpinned binary artifact.
- [x] Retain the generic explicit CPU-helper lifecycle for native OpenAI models.
- [x] Keep helper-task selection internal and schema-bound.
- [x] Prove native CPU helpers cannot replace the selected conversation model or
      enter GPU swap state.
- [x] Close #99 with the owner-approved native-link no-go evidence.

### Exit gate

- [x] Cancellation and shutdown remain bounded with deliberately hung fake backends.
- [x] CPU helpers and media runtimes cannot evict or redefine conversation models.
- [x] Requested, resolved, selected, resident and executing identities are testable
      and unambiguous.

## Phase 8 - Transactional configuration and state

### Work

- [x] Extract configuration structs, schema, validation, decoding, persistence and
      reload coordination from `config.hpp`.
- [x] Add include guards to all remaining headers.
- [x] Introduce one versioned configuration schema.
- [x] Reject unknown keys with precise paths unless an explicitly versioned
      extension container owns them.
- [x] Replace hard-coded runtime/modality matrices with runtime registration and
      capability validation.
- [x] Implement one configuration repository with one transaction lock.
- [x] Apply optimistic revision checks to base, active, alias and reset operations.
- [x] Serialize deletion/reset with writes and reload scheduling.
- [x] Preserve atomic replacement, secret restoration, masking and validation.
- [x] Define rollback behavior when persistence succeeds but reload fails.
- [x] Remove Anthropic and forced-aligner configuration from Core.
- [x] Update pricing configuration after the #103 schema is settled.

### Exit gate

- [x] Concurrent config/alias/reset tests cannot lose updates.
- [x] Unknown-key, secret, revision-conflict, rollback and recovery matrices pass.
- [x] Configuration behavior is independent of route implementation files.

## Phase 9 - Canonical observability and correct metrics

### Work

- [x] Record request ID, principal class, endpoint, protocol/profile, modality,
      requested model, resolved model, stream mode and finish/error code.
- [x] Record queue, swap/load, prefill, generation, total and first-token durations
      separately.
- [x] Record prompt, cached prompt, cache-write, completion and reasoning tokens.
- [x] Record audio input/output duration, speech characters and image counts using
      modality-specific fields.
- [x] Calculate generation TPS only from text completion tokens and generation time.
- [x] Ensure media activity cannot change LLM TPS.
- [x] Preserve StatsDb WAL, prepared statements and cached-token persistence.
- [x] Migrate the SQLite schema with backup, versioning and rollback tests.
- [x] Update SSE request events and dashboard consumers.
- [x] Replace full-file/O(n) log tailing with bounded tail reading or a ring buffer.
- [x] Add protocol/endpoint filters to diagnostic views.

### Exit gate

- [x] Metrics agree across memory, SQLite, SSE and dashboard aggregation.
- [x] Usage and cost totals use one canonical bucket source.
- [x] Request IDs correlate access logs, inference logs, DB records and client
      responses.

## Phase 10 - Dependency and release reproducibility

### Work

- [x] Add a vcpkg `builtin-baseline` and repository configuration.
- [x] Pin GitHub Actions by immutable commit SHA.
- [x] Pin Vulkan SDK/toolchain versions and verify installer checksums.
- [x] Remove the developer-machine Vulkan path fallback or make it an explicit,
      validated user override.
- [x] Keep llama.cpp and Vulkan-Headers submodules pinned and record update policy.
- [x] Establish one authoritative product version consumed by CMake, dashboard,
      packaging and API metadata.
- [x] Generate dependency manifests and an SBOM.
- [x] Generate SHA-256 checksums for release artifacts.
- [x] Sign the executable and release manifest when signing credentials are
      available.
- [x] Remove ad-hoc startup `DEBUG:` stderr output in favor of structured logs.
- [x] Reproduce a clean build from documented prerequisites on a fresh Windows
      runner.

### Exit gate

- [x] Two clean builds from the same revision produce an explained, minimized
      artifact difference or reproducible output.
- [x] Release archives include version, checksums, dependency manifest and SBOM.
- [x] No dependency resolution relies on an unspecified latest version.

## Phase 11 - Required CI and architecture enforcement

### Work

- [x] Add a pull-request and normal-push workflow separate from release publishing.
- [x] Run dashboard install, tests and production build.
- [x] Configure and build the native gateway with tests enabled.
- [x] Run only InferDeck unit/integration labels, excluding vendored test noise.
- [x] Run strict OpenAI Python SDK contract tests.
- [x] Run strict OpenAI JavaScript SDK contract tests.
- [x] Run route-manifest, schema snapshot and SSE golden tests.
- [x] Run forbidden-process, dependency-direction and protocol-leak checks.
- [x] Run config concurrency, coordinator cancellation and shutdown tests.
- [x] Run security matrices for control auth, origins, media types and size limits.
- [x] Add static analysis and formatting checks that do not rewrite the worktree.
- [x] Make the checks required through repository branch protection.
- [x] Keep the release workflow as an additional full packaging gate.

### Exit gate

- [x] A deliberately broken API chunk, forbidden subprocess call, `/v1` vendor
      route, backend JSON parse and unauthenticated control route each fail CI.
- [x] Main cannot merge without required architecture and contract checks.

## Phase 12 - File and dependency decomposition

### Work

- [x] Split OpenAI parsing, validation and serialization by endpoint.
- [x] Split control-plane status, configuration, model store, aliases, jobs, logs and
      events into focused modules.
- [x] Split coordinator queueing, residency planning, lifecycle transactions and
      voice reservations.
- [x] Split llama prompt construction, cache management, decoding, sampling,
      streaming/tool parsing and runtime lifecycle.
- [x] Reduce CMake public dependencies to the smallest necessary surfaces.
- [x] Keep foundation and the canonical inference domain dependency-light.
- [x] Remove duplicate stream orchestration after Chat and Responses share
      `GenerationSession`.
- [x] Remove obsolete Anthropic and forced-aligner test targets after approved
      extraction.
- [x] Preserve existing hardening tests while moving them with their owners.

### Exit gate

- [x] Module boundaries match the target architecture.
- [x] No protocol or administration god-file remains.
- [x] CMake dependency graph has no cycle or backend-to-gateway dependency.

## Phase 13 - Documentation and compatibility truth pass

### Work

- [x] Rewrite AGENTS.md around the completed architecture and current invariants.
- [x] Rewrite `docs/architecture.md` with the strict OpenAI and control-plane
      boundaries.
- [x] Update README API claims and remove Anthropic/Core sidecar claims.
- [x] Mark `docs/v2-cleanup-report.md` historical and link to this completed plan.
- [x] Remove recurrent-cache work from known-open items because checkpoints exist.
- [x] Document the exact supported OpenAI baseline and capability subset.
- [x] Document every explicit unsupported standard feature.
- [x] Document model identity, aliases, errors, cancellation and stream semantics.
- [x] Document secure LAN deployment and credential rotation.
- [x] Document config schema and migration.
- [x] Document release reproducibility and verification.
- [x] Add an architecture-change checklist for future agents and developers.

### Exit gate

- [x] Documentation claims are verified against route manifests, registered
      capabilities, configuration schema and tests.
- [x] No document calls an external service native or OpenAI-compatible without a
      matching contract.

## Phase 14 - Migration, deployment and release

### Pre-deployment

- [x] Produce a breaking-change and route-migration guide.
- [x] Identify every local consumer: OpenCode, Open WebUI, dashboard, scripts,
      benchmarks, editor integrations and health monitors.
- [x] Update consumers to strict OpenAI or the new control-plane endpoints.
- [x] Generate OpenCode provider configuration from the live model and stable
      alias catalog; track implementation and deployment in #142.
- [x] Back up configuration, StatsDb and managed-model manifests.
- [x] Build matched gateway and dashboard artifacts from one revision.
- [x] Run the complete clean verification matrix.

### Live activation

- [x] Obtain explicit authorization before modifying `C:\InferDeck` or restarting
      the live service.
- [x] Verify the authoritative boot mechanism and executable path.
- [x] Deploy both gateway executable and matched static assets.
- [x] Deploy config/schema migrations with rollback artifacts.
- [x] Restart the authorized service.
- [x] Verify the new process, binary hash, static bundle and configuration revision.
- [x] Probe `/v1/models`, Chat stream/non-stream, Responses stream/non-stream,
      embeddings, supported media, control authentication, status, swap and SSE.
- [x] Verify official SDK clients against the live listener.
- [x] Verify unauthorized control access fails from loopback, LAN and a browser
      origin as applicable.
- [x] Verify no sidecar, Python, FFmpeg, llama-server or orphan runtime process is
      required or launched.
- [x] Verify request IDs, metrics, StatsDb and dashboard values from real requests.
- [x] Exercise rollback and prove the service can return to the previous matched
      artifact/config set.

### Release and closure

### Verified release-candidate evidence (2026-08-21)

- Protected implementation merges: #136, #137 and #138; release candidate source
  revision `96249fb54219ef394597319854e4e2e78046318f`.
- Release dry run `32525060137` completed the clean dashboard, SDK contract,
  required-sherpa, Release build, 130-test, loader and packaging gates.
- Downloaded archive SHA-256:
  `ea1bf0c5f736eca48b3c10f5c1d6d6d2394dfeb3ab24bb90ff3f5d4dc17891cb`;
  packaged executable SHA-256:
  `ed7fccac5f283be0ebe18880d709fd65303c1c75f8d32c376963a6c936948925`.
- Live official Python and JavaScript SDK probes passed Chat and Responses in
  streaming and non-streaming modes. Real tool, vision, TTS-to-STT, request-ID,
  metrics, StatsDb and dashboard-aggregation probes passed.
- Live control-plane probes rejected an untrusted browser origin and LAN access
  with 403 while preserving permitted loopback behavior. Removed derivative and
  Anthropic routes returned 404.
- The LocalSystem NSSM service ran exactly one `inferdeck-gateway.exe`; no Python,
  FFmpeg, forced-aligner, `llama-server` or other sidecar process was present.
- Rollback was exercised before the corrected forward deployment. The stopped
  StatsDb backup contained 13,065 rows and exact totals of 446,566,532 prompt,
  121,948,123 cached and 10,348,428 completion tokens. File integrity and those
  totals were preserved; the post-verification ledger increased monotonically to
  13,084 rows, 446,568,471 prompt, 121,949,239 cached and 10,348,913 completion
  tokens. Deployment backups remain under `C:\InferDeck\deploy-backups`.
- The final protected tag `v0.8.0` points to
  `1a3421619ea62301b5a9a96a1b16043f88e9d3da`. Publishing workflow
  `32529339107` passed and produced archive SHA-256
  `dc3b00f48ffa91bcd93181fe0defb491ca22d017161605273e11628db51c126c`.
  All 35 manifest entries, the SBOM and dependency metadata verified after
  download. The exact tagged executable is live with SHA-256
  `4bc4d34be27647951ca263c5aaaac8d0d33b1b336a8cd865a262fd8c80fa7882`.
- The tag deployment retained a stopped StatsDb backup with identical SHA-256
  `52d0870394a226487ce959f4236132b6038320ba50e54ad6fa1129a7fd50ec8d`.
  Post-deployment totals increased to 446,722,132 prompt, 121,949,271 cached
  and 10,353,144 completion tokens.

- [x] Tag and publish the release with executable, static assets, config templates,
      checksums, SBOM and migration notes.
- [x] Confirm release CI and downloadable artifact integrity.
- [x] Close linked issues only with source, test, artifact and live evidence.
- [x] Close the overhaul epic after every child issue is resolved.
- [x] Update this file so every completed checkbox reflects verified reality.

### Post-release remediation (2026-08-23)

- [x] Reopen the overhaul epic and Phase 14 after discovering that OpenCode had
      only a stale hand-maintained consumer file and no exporter.
- [x] Back up and repair the rejected live active profile by removing only its
      obsolete top-level `anthropic` block.
- [x] Verify the repaired profile is active and that lifetime token totals did
      not decrease; StatsDb was not modified or migrated.
- [x] Restore and persist `Normal`, `Pro`, and `n8n-model` after the active
      profile reload exposed their absence from the persisted profile.
- [x] Merge the already-deployed authenticated LAN dashboard change in #141.
- [x] Merge #142 implementation through protected CI.
- [x] Deploy its matched gateway artifact and re-export OpenCode from the live
      post-deployment catalog.
- [ ] Close #142, Phase 14, and the epic only after the new live evidence is
      attached.

Remediation evidence: PR #143 merged as `0be8e008c006ba0d7cfb7d05f454ff7ffbe86578`
after all protected checks passed. The matched gateway and dashboard were deployed
from that revision; the live executable SHA-256 is
`7FA1960F34E99C584C2D76681B34B01AE06262F344C7D5355B82C6E815888037`. The stopped
StatsDb backup is retained under
`C:\InferDeck\deploy-backups\overhaul-remediation-20260823-0019` with SHA-256
`E14F6BF1F394C65B2636271472C945F9E67C41FE075FAAB8A51AC4C0768FABFF`, and live
token totals remained monotonic after restart. The live export advertises the
stable `Normal` and `n8n-model` aliases for Ornith 1.5 and `Pro` for Qwen 3.8,
while excluding speech-only models. Final issue closure remains gated on the
0.8.1 release and exact-artifact live verification.

## Audit finding coverage matrix

| Finding family | Phases |
|---|---|
| Forced aligner subprocess, second server, non-OpenAI route and resource bypass | 1, 7, 10, 14 |
| Unauthenticated LAN control plane, wildcard CORS and global auth | 2, 11, 14 |
| `/v1` namespace pollution and Anthropic coupling | 4, 5, 11, 13 |
| MP3-to-WAV substitution and Chat usage framing | 3, 6, 11 |
| Raw OpenAI JSON in backends | 5, 12 |
| Chat validation, proprietary fields, model identity and reasoning extensions | 4, 6 |
| Responses Chat shim and stale field allowlist | 5, 6 |
| Embeddings, images, speech, transcription and models contract gaps | 6 |
| Swap timeout, cancellation and unbounded shutdown | 7, 11 |
| Modality-derived lifecycle, selected/resident ambiguity and issue #99 | 7 |
| Configuration races, duplicated parsing and unknown keys | 8 |
| Missing request dimensions, misleading TPS and log tailing | 9 |
| Cached-input pricing and issue #103 | 3, 8, 9 |
| Missing PR CI and official SDK contract gates | 0, 11 |
| Dependency pinning, Vulkan fallback, versions, checksums and SBOM | 10 |
| God-files and broad CMake dependency surfaces | 12 |
| Stale AGENTS, architecture, README and cleanup report | 13 |

## Complete verification matrix

The exact commands may evolve with the implementation, but the final gate must
include equivalent coverage for all entries.

```powershell
cmake -S . -B build-clean -G "Visual Studio 17 2022" -A x64 -DINFERDECK_BUILD_TESTS=ON
cmake --build build-clean --target inferdeck-gateway --config Release --parallel
ctest --test-dir build-clean -C Release --output-on-failure -L "unit|integration"
pnpm install --frozen-lockfile
pnpm --filter dashboard test
pnpm --filter dashboard build
```

Additionally required:

- [x] strict OpenAI Python SDK contract suite;
- [x] strict OpenAI JavaScript SDK contract suite;
- [x] exact Chat and Responses SSE golden suite;
- [x] security/auth/origin/body-limit/deadline suite;
- [x] config transaction and concurrency suite;
- [x] coordinator deadline/cancellation/shutdown suite;
- [x] process-policy and dependency-direction suite;
- [x] real small-model Chat/Responses/tool/vision smoke suite;
- [x] media request/format/signature suite for linked native runtimes;
- [x] matched dashboard artifact verification;
- [x] live deployment and rollback verification when authorized.

## Final definition of done

The overhaul goal is complete only when all of the following are true:

- [x] Every checkbox in Phases 0 through 14 is complete or explicitly removed by
      an owner-approved amendment to this plan.
- [x] InferDeck Core builds and operates as one native executable.
- [x] No Core runtime or deployment path starts a subprocess or proxy service.
- [x] `/v1` contains only the pinned strict OpenAI surface.
- [x] Anthropic and forced alignment are absent from Core ownership.
- [x] All privileged administration is separately authenticated and safe by
      default on non-loopback hosts.
- [x] Backends receive only typed canonical requests and produce typed canonical
      output events.
- [x] Chat, Responses, embeddings, images, speech, transcription and models pass
      their declared OpenAI contract suites.
- [x] No validation failure can load, swap, queue or acquire a model.
- [x] Swap, cancellation and shutdown deadlines are real and tested.
- [x] Explicit resource roles support CPU helpers without conflating them with the
      selected conversation model.
- [x] Configuration writes are transactional and schema-validated.
- [x] Observability is correlated, protocol-aware and mathematically correct.
- [x] Dependencies and release artifacts are reproducibly pinned and verifiable.
- [x] Required CI prevents every invariant regression listed in this plan.
- [x] #99 and #103 are resolved with evidence or superseded by owner-approved
      decisions.
- [x] All new audit issues and the overhaul epic are closed with evidence.
- [x] Documentation matches source, tests, packaged artifacts and live behavior.
- [x] The matched release is built, published and live-verified when deployment is
      authorized.
