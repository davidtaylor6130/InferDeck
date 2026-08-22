# ADR 0002: Authenticated remote dashboard sessions

- Status: Accepted
- Date: 2026-08-22
- Owners: @davidtaylor6130
- Related issues: None
- Supersedes: None

## Context

InferDeck listens on LAN and encrypted-overlay interfaces so OpenAI clients can
use the data plane remotely. The static dashboard was also served remotely, but
its status, pricing and SSE routes were unconditionally limited to direct
loopback. The page therefore loaded while every data request returned 403.

Bearer authentication alone is insufficient for the shipped dashboard because
native `EventSource` cannot attach an Authorization header.

## Decision

When remote control is enabled, a dashboard may exchange the separate control
token at `POST /api/inferdeck/v1/dashboard/session`. A valid exchange creates
an HTTP-only, `SameSite=Strict`, path-scoped session cookie. The existing
control authorizer accepts that cookie as the control credential for dashboard
reads, SSE and administrative operations. Direct loopback retains ambient
passwordless authority.

The session-bootstrap request bypasses only the normal pre-handler credential
check. It still requires remote control to be enabled, an exact allowed origin,
JSON request policy and a constant-time control-token comparison.

## Boundary impact

### Protocol

- OpenAI endpoint or schema impact: None.
- `/v1` route impact: None.
- Compatibility-profile impact: None.
- Error and streaming contract impact: remote dashboard routes return 401 before
  login and preserve their existing payloads and native SSE after login.

### Process

- Executable, service, thread, subprocess, or proxy impact: none; the gateway
  remains one native process.
- Startup and shutdown impact: none.

### Resource and runtime

- Coordinator, admission, residency, slot, cancellation, or deadline impact:
  none.
- CPU, GPU, memory, or model-role impact: none.

### Security

- Authentication and authorization impact: remote dashboard sessions use only
  the separate control token and never inherit unauthenticated data-plane access.
- Bind address, CORS, secrets, and administrative-operation impact: every remote
  origin is exact; mutations retain origin and `Sec-Fetch-Site` enforcement.
  The token is held in an HTTP-only session cookie rather than JavaScript
  storage. The built-in HTTP listener does not provide transport encryption, so
  deployment is limited to trusted LAN or encrypted overlay interfaces.

### Observability

- Metrics, events, logs, traces, usage, pricing, or persistence impact: existing
  request IDs and response logs cover login and authenticated dashboard routes.
  The token and cookie value are not logged.

## Alternatives considered

- Allow unauthenticated remote dashboard routes. Rejected because the dashboard
  includes privileged administration.
- Put the token in the SSE query string. Rejected because credentials can leak
  through URLs and intermediaries.
- Replace native `EventSource` with a custom streaming client. Rejected because
  it adds parser and reconnection complexity solely to attach a header.

## Consequences

Remote LAN and overlay dashboards work with native SSE after one login per
browser session. Operators must configure a strong separate token and exact
origins. The session cookie cannot be marked `Secure` on the built-in HTTP
listener, so untrusted networks remain unsupported.

## Migration and rollback

Enable `control.allow_remote`, configure a token of at least 32 cookie-safe ASCII
characters and
add exact LAN or overlay origins. Roll back by restoring
`control.allow_remote: false`; loopback dashboard access remains available.
No StatsDb, model or usage migration occurs.

## Verification

- Unit-test remote dashboard authorization and exact cookie parsing.
- Verify dashboard typechecking, tests and production build.
- Prove unauthenticated 401, valid login 200, HTTP-only same-site cookie,
  authenticated status/SSE 200, wrong-token 401 and wrong-origin 403.
- Run architecture policy, native tests and a Release build before deployment.
