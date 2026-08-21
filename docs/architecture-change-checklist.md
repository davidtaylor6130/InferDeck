# Architecture change checklist

Use this checklist for every change that crosses a runtime, protocol,
configuration, persistence, security, or deployment boundary.

## Design

- The Core remains one native process; no runtime subprocess, proxy, shell,
  Python service, FFmpeg process, or `llama-server` is introduced.
- Strict `/v1` changes are pinned OpenAI behavior and update the route
  manifest, schema snapshots, SSE goldens, both official SDK suites, and
  compatibility documentation together.
- Operational fields and mutations stay under authenticated
  `/api/inferdeck/v1`; derivative fields stay in an explicit default-off
  compatibility profile.
- Routes decode into typed canonical requests. Backends never parse transport
  JSON and never depend on the gateway.
- Queueing, residency, cancellation, deadlines, metrics, and streaming reuse
  shared domain orchestration rather than endpoint-specific copies.
- New runtimes expose native libraries, explicit resource metadata, bounded
  lifecycle operations, and honest capability discovery.

## Security and state

- Authentication, origin, media type, body limit, and malformed-input checks
  happen before admission or mutation.
- Configuration changes are schema-versioned, optimistic, transactional,
  secret-safe, reloadable, and recoverable.
- Persistent schema changes include backup, forward migration, rollback, and
  reopen tests.
- Request identity and canonical metrics remain consistent across logs, memory,
  SQLite, SSE, and dashboard aggregation.

## Verification and release

- Architecture/static policy, unit/integration labels, dashboard tests/build,
  SDK contracts, schema snapshots, SSE goldens, security matrices, and
  deliberately hung lifecycle tests pass.
- A real native model smoke test covers supported affected capabilities.
- CMake visibility is minimal and introduces no cycle or backend-to-gateway
  dependency.
- Documentation describes supported and rejected behavior without calling an
  external companion native or OpenAI-compatible.
- Release inputs remain pinned; manifests, SBOM, checksums, and signature state
  are generated and verified.
- Deployment backs up state and activates executable plus static assets from
  one revision. Live verification and rollback evidence are attached before
  closure.
