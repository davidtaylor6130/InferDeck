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

- [ ] focused unit tests;
- [ ] applicable integration and contract tests;
- [ ] Release build;
- [ ] dashboard tests/build when affected;
- [ ] source and documentation truth check;
- [ ] reviewed diff with unrelated work excluded;
- [ ] branch pushed and PR opened or updated;
- [ ] required CI green;
- [ ] linked GitHub issues updated with evidence.

## Phase 0 - Governance freeze and authoritative backlog

### Purpose

Prevent further boundary drift while the overhaul is in progress and turn the
audit into an authoritative, dependency-ordered backlog.

### Work

- [ ] Adopt an ADR declaring the OpenAI-only Core, single-process rule, control
      plane boundary, canonical inference domain, and compatibility policy.
- [ ] Add an architecture decision template covering protocol, process, resource,
      security, observability, migration, and contract-test impact.
- [ ] Create a GitHub epic for this plan.
- [ ] Create one issue for every independently deliverable work item below.
- [ ] Label issues with `P0` through `P3`, `protocol`, `security`, `architecture`,
      `runtime`, `observability`, `build`, and `migration` as applicable.
- [ ] Add issue dependencies and assign milestone order matching this plan.
- [ ] Record #99 as blocked by Phase 7's explicit resource-role model.
- [ ] Record #103 as an early correctness fix before broad configuration changes.
- [ ] Select PR #105 as the candidate for #103 after diff review.
- [ ] Close or supersede PR #104 only after owner authorization and preservation of
      any unique work.
- [ ] Add CODEOWNERS coverage for OpenAI contracts, coordinator/runtime boundaries,
      authentication, configuration, and release workflows.
- [ ] Add a PR template that requires invariant and contract impact declarations.

### Exit gate

- [ ] Every item in this plan maps to an open or completed GitHub issue.
- [ ] The epic ordering matches this document.
- [ ] No untracked P0 or P1 finding remains outside the backlog.

## Phase 1 - Forced-aligner containment and extraction

### Decision

The current Python/FastAPI/Qwen/PyTorch/ROCm forced aligner is a companion
service, not InferDeck Core. Exact forced alignment is not an OpenAI API and the
current implementation cannot satisfy the native single-process rule.

### Work

- [ ] Inventory every forced-aligner source, configuration, deployment script,
      runtime path, service/watchdog, documentation link, test asset, and live
      dependency.
- [ ] Confirm whether any unique user data or secrets exist outside tracked source.
- [ ] Define the companion product name, ownership, release lifecycle, and API
      status without presenting `/v1/audio/alignments` as OpenAI.
- [ ] Preserve the complete feature history in a tag, archive, or companion
      repository before removing Core ownership.
- [ ] Obtain explicit owner approval before moving or deleting tracked files.
- [ ] Remove `apps/forced-aligner` from InferDeck Core after the preservation gate.
- [ ] Remove forced-aligner service YAML and Windows launcher/watchdog/install
      scripts from Core after the preservation gate.
- [ ] Remove Core documentation and tests that imply the sidecar is a native
      InferDeck runtime.
- [ ] Remove live-deployment assumptions from the Core release and support matrix.
- [ ] Prove that Core contains no Python service, FFmpeg subprocess, watchdog, or
      secondary inference listener.
- [ ] Add a source-policy test that fails on secondary runtime/inference
      process-launch primitives in Core source and feature-specific service
      scripts. Explicitly allow only the documented boot launcher that starts the
      single gateway executable.

### Exit gate

- [ ] `rg` finds no Core process-launch path.
- [ ] Core build and tests do not require Python, PyTorch, ROCm, FFmpeg, FastAPI,
      Uvicorn, Docker, NSSM sidecars, or watchdog scripts.
- [ ] Any retained companion is clearly outside the Core product and repository
      contract.

## Phase 2 - Control-plane security boundary

### Work

- [ ] Classify every route as OpenAI data plane, read-only public status, dashboard
      session, or privileged control plane.
- [ ] Introduce route-scoped principals rather than one global authentication
      wrapper.
- [ ] Require control-plane authentication for every mutating operation.
- [ ] Default privileged control routes to loopback-only access.
- [ ] Require an explicit `control.allow_remote` setting, a non-empty control token,
      and a configured origin allowlist before remote administration can start.
- [ ] Keep OpenAI data-plane authentication separately configurable.
- [ ] Remove global `x-api-key` fallback.
- [ ] Add constant-time credential comparison and secret masking.
- [ ] Restrict control-plane CORS; prohibit wildcard origins for privileged routes.
- [ ] Add CSRF protection if browser credentials are cookie-backed.
- [ ] Apply authorization to model download, archive, permanent removal, aliases,
      config read/write/reset/reload, model load/unload, logs, jobs, cancellation,
      and benchmark operations.
- [ ] Add endpoint-specific request-body limits and content-type enforcement.
- [ ] Add read, write, idle, and slow-client deadlines.
- [ ] Generate or accept a sanitized request ID, return it in response headers, and
      include it in every structured log and request outcome.
- [ ] Update LAN configuration tests so insecure remote administration cannot be
      accidentally locked in as expected behavior.

### Exit gate

- [ ] Non-loopback plus unauthenticated control startup fails validation.
- [ ] Every mutating control endpoint returns 401/403 without the correct principal.
- [ ] OpenAI credentials cannot administer the control plane unless explicitly
      granted that principal.
- [ ] Browser-origin and request-size/deadline matrices pass.

## Phase 3 - Immediate OpenAI contract hotfixes

### Chat streaming

- [ ] Emit `usage: null` on every ordinary Chat streaming chunk when
      `stream_options.include_usage` is enabled.
- [ ] Emit a distinct final usage chunk with `choices: []` before `[DONE]`.
- [ ] Keep the finish-reason chunk separate from the usage-only chunk.
- [ ] Add byte-exact golden tests for content, reasoning-profile, tool-call, finish,
      usage, error, cancellation, and `[DONE]` ordering.

### Speech

- [ ] Stop silently rewriting explicit MP3 requests to WAV.
- [ ] Return an OpenAI-shaped 400 `unsupported_response_format` until a native
      in-process encoder exists.
- [ ] Enforce the official 4,096-character input limit by Unicode character count.
- [ ] Validate `model`, `input`, `voice`, `speed`, `instructions`,
      `response_format`, and `stream_format` before admission.
- [ ] Remove the regression test that expects MP3 input to return WAV and replace it
      with the strict contract matrix.

### Pricing issue #103

- [ ] Review PR #105 against current main and this plan's schema boundaries.
- [ ] Verify separate uncached-input, cached-input, and output rates.
- [ ] Clamp cached tokens to prompt tokens.
- [ ] Verify legacy Qwen estimation is date-bounded, deterministic, and identical
      across overview and usage pages.
- [ ] Run dashboard, C++, forced historical fixture, build, and live-safe validation.
- [ ] Merge the selected implementation before canonical config refactoring.
- [ ] Close #103 only after matching source, dashboard bundle, deployment artifact,
      and calculation evidence.

### Exit gate

- [ ] Official OpenAI SDK consumers parse the corrected Chat stream.
- [ ] No requested audio format is silently substituted.
- [ ] #103 has one canonical merged implementation and no duplicate active PR.

## Phase 4 - Clean API namespaces and compatibility profiles

### Strict `/v1` surface

- [ ] Create a route manifest for the pinned OpenAI compatibility baseline.
- [ ] Limit `/v1` to supported OpenAI endpoints:
  - [ ] `GET /v1/models`
  - [ ] `POST /v1/chat/completions`
  - [ ] `POST /v1/responses`
  - [ ] `POST /v1/embeddings`
  - [ ] `POST /v1/images/generations`
  - [ ] `POST /v1/audio/speech`
  - [ ] `POST /v1/audio/transcriptions`
- [ ] Move health, metrics, history and swap operations to
      `/api/inferdeck/v1/*`.
- [ ] Update dashboard calls, documentation, fixtures and operational scripts.
- [ ] Remove `/v1/swap/*`, `/v1/metrics`, `/v1/stats/history`, and `/v1/health`
      after the migration window.
- [ ] Remove `/v1/messages` and `/v1/messages/count_tokens` from Core.
- [ ] Remove Anthropic aliases and `x-api-key` behavior from Core configuration.
- [ ] Preserve Anthropic source history before deletion and obtain explicit owner
      approval for destructive removal.

### Derivative OpenAI profiles

- [ ] Define `strict_openai` as the default and production-supported profile.
- [ ] If a derivative profile is still required, give it a separate base path or
      listener and a named compatibility manifest.
- [ ] Move `reasoning_content`, sampler extensions, and other derivative fields out
      of strict mode.
- [ ] Prohibit derivative profiles from changing shared auth, scheduling, errors,
      model identity, or strict serializers.

### Exit gate

- [ ] A route-snapshot test proves every `/v1` route belongs to the pinned OpenAI
      manifest.
- [ ] The strict profile contains no InferDeck or Anthropic top-level fields.
- [ ] Dashboard and operational workflows use the new control paths.

## Phase 5 - Typed canonical inference domain

### New domain boundary

- [ ] Introduce a focused library for protocol-neutral inference contracts.
- [ ] Define typed `RequestContext` with request ID, principal, endpoint, deadline,
      cancellation, requested model, resolved model, stream mode and compatibility
      profile.
- [ ] Define typed instructions, messages/input items, text/image/audio content,
      function tools, tool choice, structured-output constraints and sampling.
- [ ] Define typed `OutputEvent` variants for text, reasoning, tool calls, usage,
      finish, refusal, and error.
- [ ] Define canonical `RequestOutcome` and `DomainError` taxonomies.
- [ ] Define capability declarations independently of HTTP fields.
- [ ] Remove `openai_body_json` from `InferenceRequest`.
- [ ] Remove protocol parsing from `LlamaCppModel` and every runtime backend.
- [ ] Move prompt-template conversion into a typed llama adapter layer.
- [ ] Prohibit `httplib`, route headers, OpenAI field names, and protocol response
      objects from model and runtime libraries.

### Generation session

- [ ] Extract one `GenerationSession` service shared by Chat and Responses.
- [ ] Centralize slot ownership, cancellation, backpressure, inference-thread
      lifetime, finish-once behavior and request recording.
- [ ] Preserve bounded queues, UTF-8 holdback, cursor sanitization and idempotent
      release semantics.

### Exit gate

- [ ] Dependency tests prove model/runtime libraries contain no HTTP or protocol
      JSON dependency.
- [ ] Golden adapter tests convert OpenAI requests to canonical objects and
      canonical events back to wire output.
- [ ] Chat and Responses share execution lifecycle without calling one another's
      HTTP handlers.

## Phase 6 - Complete strict OpenAI adapters

### Common parsing and errors

- [ ] Pin an OpenAI compatibility baseline date and supported SDK versions.
- [ ] Create typed field decoders with field-specific errors and parameter names.
- [ ] Validate complete request shape before resolution or resource work.
- [ ] Centralize domain-error to OpenAI status/type/code/param mapping.
- [ ] Enforce JSON media types and multipart requirements.
- [ ] Explicitly reject unsupported semantic features; never silently ignore them.
- [ ] Accept harmless standard values when their semantics can be preserved, such
      as `store: false` in a stateless implementation.

### Chat Completions

- [ ] Require and validate `model` and `messages`.
- [ ] Preserve developer/system instruction precedence.
- [ ] Validate all supported content, tools, tool choice, response format, stop,
      token limits, temperature/top-p ranges, seed, streaming and stream options.
- [ ] Remove `:latest` normalization.
- [ ] Remove public `priority`, `top_k`, repeat controls and template kwargs.
- [ ] Define strict behavior for unsupported official fields.
- [ ] Return canonical model identity consistently in stream and non-stream modes.
- [ ] Keep derivative reasoning output outside strict mode.

### Responses

- [ ] Implement Responses directly over the canonical domain.
- [ ] Support string and item-array input forms required by the pinned baseline.
- [ ] Preserve developer messages and function-call/output pairing.
- [ ] Support current function-call output content forms.
- [ ] Generate one immutable response timestamp and model identity.
- [ ] Produce complete, internally consistent Response objects.
- [ ] Produce typed Responses SSE events directly from canonical output events.
- [ ] Define explicit behavior for storage, background, conversation,
      previous-response, include, truncation, service-tier and cache fields.

### Embeddings

- [ ] Support string, string-array, token-ID array and token-ID matrix input.
- [ ] Validate dimensions, encoding format, user and input limits.
- [ ] Encode base64 floats with explicit little-endian representation.
- [ ] Remove public priority and reject unknown unsupported fields explicitly.

### Images

- [ ] Validate the pinned official image request schema.
- [ ] Remove proprietary top-level generation controls from strict mode.
- [ ] Support or explicitly reject quality, style, background, output format,
      moderation, user and streaming-related fields.
- [ ] Ensure response encoding and MIME behavior match the requested contract.

### Audio

- [ ] Complete speech instructions and stream-format behavior.
- [ ] Complete transcription response formats, timestamp granularities, includes,
      logprobs, chunking and streaming behavior for the supported baseline.
- [ ] Remove fabricated transcription metrics such as constant compression ratio.
- [ ] Keep internal cancellation outcomes separate from client-visible HTTP status.

### Models

- [ ] Make `/v1/models` return only the strict OpenAI discovery fields.
- [ ] Move runtime, VRAM, slots, pricing, optimization, alias contracts and
      residency detail to `/api/inferdeck/v1/models`.

### Exit gate

- [ ] Pinned official Python and JavaScript SDK contract suites pass.
- [ ] Stream and non-stream schema snapshots pass for every supported endpoint.
- [ ] No malformed request changes jobs, queue, residency or active-request state.

## Phase 7 - Coordinator, lifecycle and issue #99

### Explicit model resource model

- [ ] Replace modality-derived lifecycle rules with explicit metadata:
  - [ ] `role`: conversation, helper, media, embedding, maintenance;
  - [ ] `compute`: CPU, Vulkan GPU, CUDA GPU, ROCm GPU or mixed;
  - [ ] `residency`: always, managed or on-demand;
  - [ ] `admission_pool` and concurrency limit;
  - [ ] memory requirements and eviction eligibility.
- [ ] Separate default/selected model, requested model, resolved model, resident set
      and executing model in coordinator state and APIs.
- [ ] Validate impossible role/compute/residency combinations in configuration.

### Deadlines, cancellation and shutdown

- [ ] Replace ignored swap timeouts with one propagated deadline.
- [ ] Propagate cancellation through queue wait, drain, resize, eviction, unload,
      load, inference and stream delivery.
- [ ] Make residency changes transactional and restore the last usable state after
      failure where possible.
- [ ] Replace polling plus unconditional join with a bounded shutdown state machine.
- [ ] Add fake backends that block each lifecycle stage for deterministic tests.
- [ ] Preserve the invariant that inference never holds the coordinator mutex.

### Voice reservations

- [ ] Replace source-IP fallback with an explicit opaque session identity scoped to
      an authenticated principal.
- [ ] Ensure clients sharing an IP cannot affect one another's priority session.
- [ ] Keep voice priority internal rather than request-body controlled.

### Issue #99

- [ ] Revalidate Needle 2 licensing, native integration feasibility, package size,
      quality and performance against current requirements.
- [ ] Register it as an explicit CPU helper with always-resident lifecycle.
- [ ] Expose it as a normal OpenAI model where client invocation is required.
- [ ] Keep helper-task selection internal and schema-bound.
- [ ] Prove it cannot replace the selected conversation model or enter GPU swap
      state.
- [ ] Close #99 only after concurrency, lifecycle, fallback and quality gates pass.

### Exit gate

- [ ] Cancellation and shutdown remain bounded with deliberately hung fake backends.
- [ ] CPU helpers and media runtimes cannot evict or redefine conversation models.
- [ ] Requested, resolved, selected, resident and executing identities are testable
      and unambiguous.

## Phase 8 - Transactional configuration and state

### Work

- [ ] Extract configuration structs, schema, validation, decoding, persistence and
      reload coordination from `config.hpp`.
- [ ] Add include guards to all remaining headers.
- [ ] Introduce one versioned configuration schema.
- [ ] Reject unknown keys with precise paths unless an explicitly versioned
      extension container owns them.
- [ ] Replace hard-coded runtime/modality matrices with runtime registration and
      capability validation.
- [ ] Implement one configuration repository with one transaction lock.
- [ ] Apply optimistic revision checks to base, active, alias and reset operations.
- [ ] Serialize deletion/reset with writes and reload scheduling.
- [ ] Preserve atomic replacement, secret restoration, masking and validation.
- [ ] Define rollback behavior when persistence succeeds but reload fails.
- [ ] Remove Anthropic and forced-aligner configuration from Core.
- [ ] Update pricing configuration after the #103 schema is settled.

### Exit gate

- [ ] Concurrent config/alias/reset tests cannot lose updates.
- [ ] Unknown-key, secret, revision-conflict, rollback and recovery matrices pass.
- [ ] Configuration behavior is independent of route implementation files.

## Phase 9 - Canonical observability and correct metrics

### Work

- [ ] Record request ID, principal class, endpoint, protocol/profile, modality,
      requested model, resolved model, stream mode and finish/error code.
- [ ] Record queue, swap/load, prefill, generation, total and first-token durations
      separately.
- [ ] Record prompt, cached prompt, cache-write, completion and reasoning tokens.
- [ ] Record audio input/output duration, speech characters and image counts using
      modality-specific fields.
- [ ] Calculate generation TPS only from text completion tokens and generation time.
- [ ] Ensure media activity cannot change LLM TPS.
- [ ] Preserve StatsDb WAL, prepared statements and cached-token persistence.
- [ ] Migrate the SQLite schema with backup, versioning and rollback tests.
- [ ] Update SSE request events and dashboard consumers.
- [ ] Replace full-file/O(n) log tailing with bounded tail reading or a ring buffer.
- [ ] Add protocol/endpoint filters to diagnostic views.

### Exit gate

- [ ] Metrics agree across memory, SQLite, SSE and dashboard aggregation.
- [ ] Usage and cost totals use one canonical bucket source.
- [ ] Request IDs correlate access logs, inference logs, DB records and client
      responses.

## Phase 10 - Dependency and release reproducibility

### Work

- [ ] Add a vcpkg `builtin-baseline` and repository configuration.
- [ ] Pin GitHub Actions by immutable commit SHA.
- [ ] Pin Vulkan SDK/toolchain versions and verify installer checksums.
- [ ] Remove the developer-machine Vulkan path fallback or make it an explicit,
      validated user override.
- [ ] Keep llama.cpp and Vulkan-Headers submodules pinned and record update policy.
- [ ] Establish one authoritative product version consumed by CMake, dashboard,
      packaging and API metadata.
- [ ] Generate dependency manifests and an SBOM.
- [ ] Generate SHA-256 checksums for release artifacts.
- [ ] Sign the executable and release manifest when signing credentials are
      available.
- [ ] Remove ad-hoc startup `DEBUG:` stderr output in favor of structured logs.
- [ ] Reproduce a clean build from documented prerequisites on a fresh Windows
      runner.

### Exit gate

- [ ] Two clean builds from the same revision produce an explained, minimized
      artifact difference or reproducible output.
- [ ] Release archives include version, checksums, dependency manifest and SBOM.
- [ ] No dependency resolution relies on an unspecified latest version.

## Phase 11 - Required CI and architecture enforcement

### Work

- [ ] Add a pull-request and normal-push workflow separate from release publishing.
- [ ] Run dashboard install, tests and production build.
- [ ] Configure and build the native gateway with tests enabled.
- [ ] Run only InferDeck unit/integration labels, excluding vendored test noise.
- [ ] Run strict OpenAI Python SDK contract tests.
- [ ] Run strict OpenAI JavaScript SDK contract tests.
- [ ] Run route-manifest, schema snapshot and SSE golden tests.
- [ ] Run forbidden-process, dependency-direction and protocol-leak checks.
- [ ] Run config concurrency, coordinator cancellation and shutdown tests.
- [ ] Run security matrices for control auth, origins, media types and size limits.
- [ ] Add static analysis and formatting checks that do not rewrite the worktree.
- [ ] Make the checks required through repository branch protection.
- [ ] Keep the release workflow as an additional full packaging gate.

### Exit gate

- [ ] A deliberately broken API chunk, forbidden subprocess call, `/v1` vendor
      route, backend JSON parse and unauthenticated control route each fail CI.
- [ ] Main cannot merge without required architecture and contract checks.

## Phase 12 - File and dependency decomposition

### Work

- [ ] Split OpenAI parsing, validation and serialization by endpoint.
- [ ] Split control-plane status, configuration, model store, aliases, jobs, logs and
      events into focused modules.
- [ ] Split coordinator queueing, residency planning, lifecycle transactions and
      voice reservations.
- [ ] Split llama prompt construction, cache management, decoding, sampling,
      streaming/tool parsing and runtime lifecycle.
- [ ] Reduce CMake public dependencies to the smallest necessary surfaces.
- [ ] Keep foundation and the canonical inference domain dependency-light.
- [ ] Remove duplicate stream orchestration after Chat and Responses share
      `GenerationSession`.
- [ ] Remove obsolete Anthropic and forced-aligner test targets after approved
      extraction.
- [ ] Preserve existing hardening tests while moving them with their owners.

### Exit gate

- [ ] Module boundaries match the target architecture.
- [ ] No protocol or administration god-file remains.
- [ ] CMake dependency graph has no cycle or backend-to-gateway dependency.

## Phase 13 - Documentation and compatibility truth pass

### Work

- [ ] Rewrite AGENTS.md around the completed architecture and current invariants.
- [ ] Rewrite `docs/architecture.md` with the strict OpenAI and control-plane
      boundaries.
- [ ] Update README API claims and remove Anthropic/Core sidecar claims.
- [ ] Mark `docs/v2-cleanup-report.md` historical and link to this completed plan.
- [ ] Remove recurrent-cache work from known-open items because checkpoints exist.
- [ ] Document the exact supported OpenAI baseline and capability subset.
- [ ] Document every explicit unsupported standard feature.
- [ ] Document model identity, aliases, errors, cancellation and stream semantics.
- [ ] Document secure LAN deployment and credential rotation.
- [ ] Document config schema and migration.
- [ ] Document release reproducibility and verification.
- [ ] Add an architecture-change checklist for future agents and developers.

### Exit gate

- [ ] Documentation claims are verified against route manifests, registered
      capabilities, configuration schema and tests.
- [ ] No document calls an external service native or OpenAI-compatible without a
      matching contract.

## Phase 14 - Migration, deployment and release

### Pre-deployment

- [ ] Produce a breaking-change and route-migration guide.
- [ ] Identify every local consumer: OpenCode, Open WebUI, dashboard, scripts,
      benchmarks, editor integrations and health monitors.
- [ ] Update consumers to strict OpenAI or the new control-plane endpoints.
- [ ] Back up configuration, StatsDb and managed-model manifests.
- [ ] Build matched gateway and dashboard artifacts from one revision.
- [ ] Run the complete clean verification matrix.

### Live activation

- [ ] Obtain explicit authorization before modifying `C:\InferDeck` or restarting
      the live service.
- [ ] Verify the authoritative boot mechanism and executable path.
- [ ] Deploy both gateway executable and matched static assets.
- [ ] Deploy config/schema migrations with rollback artifacts.
- [ ] Restart the authorized service.
- [ ] Verify the new process, binary hash, static bundle and configuration revision.
- [ ] Probe `/v1/models`, Chat stream/non-stream, Responses stream/non-stream,
      embeddings, supported media, control authentication, status, swap and SSE.
- [ ] Verify official SDK clients against the live listener.
- [ ] Verify unauthorized control access fails from loopback, LAN and a browser
      origin as applicable.
- [ ] Verify no sidecar, Python, FFmpeg, llama-server or orphan runtime process is
      required or launched.
- [ ] Verify request IDs, metrics, StatsDb and dashboard values from real requests.
- [ ] Exercise rollback and prove the service can return to the previous matched
      artifact/config set.

### Release and closure

- [ ] Tag and publish the release with executable, static assets, config templates,
      checksums, SBOM and migration notes.
- [ ] Confirm release CI and downloadable artifact integrity.
- [ ] Close linked issues only with source, test, artifact and live evidence.
- [ ] Close the overhaul epic after every child issue is resolved.
- [ ] Update this file so every completed checkbox reflects verified reality.

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

- [ ] strict OpenAI Python SDK contract suite;
- [ ] strict OpenAI JavaScript SDK contract suite;
- [ ] exact Chat and Responses SSE golden suite;
- [ ] security/auth/origin/body-limit/deadline suite;
- [ ] config transaction and concurrency suite;
- [ ] coordinator deadline/cancellation/shutdown suite;
- [ ] process-policy and dependency-direction suite;
- [ ] real small-model Chat/Responses/tool/vision smoke suite;
- [ ] media request/format/signature suite for linked native runtimes;
- [ ] matched dashboard artifact verification;
- [ ] live deployment and rollback verification when authorized.

## Final definition of done

The overhaul goal is complete only when all of the following are true:

- [ ] Every checkbox in Phases 0 through 14 is complete or explicitly removed by
      an owner-approved amendment to this plan.
- [ ] InferDeck Core builds and operates as one native executable.
- [ ] No Core runtime or deployment path starts a subprocess or proxy service.
- [ ] `/v1` contains only the pinned strict OpenAI surface.
- [ ] Anthropic and forced alignment are absent from Core ownership.
- [ ] All privileged administration is separately authenticated and safe by
      default on non-loopback hosts.
- [ ] Backends receive only typed canonical requests and produce typed canonical
      output events.
- [ ] Chat, Responses, embeddings, images, speech, transcription and models pass
      their declared OpenAI contract suites.
- [ ] No validation failure can load, swap, queue or acquire a model.
- [ ] Swap, cancellation and shutdown deadlines are real and tested.
- [ ] Explicit resource roles support CPU helpers without conflating them with the
      selected conversation model.
- [ ] Configuration writes are transactional and schema-validated.
- [ ] Observability is correlated, protocol-aware and mathematically correct.
- [ ] Dependencies and release artifacts are reproducibly pinned and verifiable.
- [ ] Required CI prevents every invariant regression listed in this plan.
- [ ] #99 and #103 are resolved with evidence or superseded by owner-approved
      decisions.
- [ ] All new audit issues and the overhaul epic are closed with evidence.
- [ ] Documentation matches source, tests, packaged artifacts and live behavior.
- [ ] The matched release is built, published and live-verified when deployment is
      authorized.
