# Control-plane security boundary

- Status: Implemented on Phase 2 branch
- Date: 2026-08-20
- Tracking: #110
- Depends on: ADR 0001 and Phase 1

## Security model

InferDeck assigns one principal to every API request before its handler runs.

| Principal | Default authority | Authentication |
|---|---|---|
| Public status | reserved for a future minimal liveness route | none |
| OpenAI data plane | OpenAI inference and model discovery | independently configured `auth` bearer token |
| Dashboard session | live status, pricing, and event stream | direct loopback only |
| Control read | configuration, logs, jobs, metrics, model-store state | loopback, or remote control principal |
| Control write | every operation that changes runtime, files, configuration, aliases, or jobs | loopback, or remote control principal |

Loopback addresses are `127.0.0.0/8`, `::1`, and IPv4-mapped loopback. Ambient
loopback authority additionally requires a numeric loopback or `localhost`
`Host` header and rejects `Forwarded`, `X-Forwarded-*`, `X-Real-IP`, and `Via`.
An unknown peer, DNS-rebinding host, or proxy-indicated request is not trusted.

The OpenAI bearer token does not grant control authority. An operator can opt in
to sharing that token with `control.allow_data_plane_token: true`, but the remote
control token must still be non-empty and the configuration must explicitly
acknowledge the shared principal.

The global `x-api-key` promotion has been removed. It cannot authenticate either
plane. Optional compatibility profiles use separate `/compat/*` paths and the
same data-plane bearer authentication; they are disabled by default.

## Route inventory

The paths below are the canonical Phase 4 routes.

### OpenAI data plane

- `GET /v1/models`
- `POST /v1/chat/completions`
- `POST /v1/embeddings`
- `POST /v1/responses`
- `POST /v1/images/generations`
- `POST /v1/audio/speech`
- `POST /v1/audio/transcriptions`

### Dashboard session

- `GET /api/inferdeck/v1/status`
- `GET /api/inferdeck/v1/pricing`
- `GET /api/inferdeck/v1/events/stream`

### Control read

- `GET /api/inferdeck/v1/swap/status`
- `GET /api/inferdeck/v1/health`
- `GET /api/inferdeck/v1/metrics`
- `GET /api/inferdeck/v1/stats/history`
- `GET /api/inferdeck/v1/media/jobs`
- `GET /api/inferdeck/v1/models`
- `GET /api/inferdeck/v1/usage/daily`
- `GET /api/inferdeck/v1/optimize/benchmark`
- `GET /api/inferdeck/v1/optimize/schedule`
- `GET /api/inferdeck/v1/model-store/search`
- `GET /api/inferdeck/v1/model-store/inspect`
- `GET /api/inferdeck/v1/model-store/downloads`
- `GET /api/inferdeck/v1/model-aliases`
- `GET /api/inferdeck/v1/config`
- `GET /api/inferdeck/v1/jobs`
- `GET /api/inferdeck/v1/logs`

### Control write

- `POST /api/inferdeck/v1/swap/to/:name`
- `POST /api/inferdeck/v1/swap/cancel`
- `POST /api/inferdeck/v1/media/jobs/:id/cancel`
- `POST /api/inferdeck/v1/optimize/profile`
- `POST /api/inferdeck/v1/optimize/benchmark`
- `POST /api/inferdeck/v1/optimize/benchmark/cancel`
- `POST /api/inferdeck/v1/model-store/downloads`
- `POST /api/inferdeck/v1/model-store/downloads/:id/cancel`
- `POST /api/inferdeck/v1/model-store/downloads/:id/resume`
- `POST /api/inferdeck/v1/model-store/remove`
- `POST /api/inferdeck/v1/model-store/archive`
- `POST /api/inferdeck/v1/model-store/unregister`
- `PUT /api/inferdeck/v1/model-aliases/:name`
- `DELETE /api/inferdeck/v1/model-aliases/:name`
- `PUT /api/inferdeck/v1/config`
- `PUT /api/inferdeck/v1/config/active`
- `DELETE /api/inferdeck/v1/config/active`
- `POST /api/inferdeck/v1/models/load`
- `POST /api/inferdeck/v1/models/unload`

All future `/api` mutations default to the control-write principal through the
central classifier, even before they are added to this human-readable inventory.
The route matrix test lists every current mutating operation so omissions fail
review visibly.

## Configuration contract

`auth` configures only the OpenAI data plane. `cors.origins` is likewise a
data-plane allowlist and may remain wildcard for explicitly public local-model
clients.

`control` configures administrative access:

```yaml
control:
  allow_remote: false
  allow_data_plane_token: false
  token: ""
  origins:
    - "http://127.0.0.1:11434"
    - "http://localhost:11434"
    - "http://[::1]:11434"

compatibility:
  openai_derivative:
    enabled: false
  anthropic:
    enabled: false
```

When `allow_remote` is false, non-loopback control requests and preflights return
403 regardless of the OpenAI token. When it is true, startup validation requires:

- a `control.token` containing at least 32 non-whitespace characters;
- at least one exact `control.origins` entry;
- no empty or wildcard control origin;
- a control token distinct from the OpenAI token unless sharing is explicitly
  enabled.

Control and model-store tokens are masked as `__INFERDECK_SECRET__` by the
configuration API and restored server-side during an update. A parsed-YAML
fallback covers flow mappings, quoted keys, block scalars, and duplicate secret
keys when the comment-preserving fast path cannot prove complete redaction.
Credential checks use the constant-time bearer comparison shared by both
principals.

Remote bearer administration is an API mode, not a remote-dashboard login. The
shipped dashboard and native `EventSource` remain direct-loopback only. The
native listener has no TLS, so remote control must travel over an encrypted
overlay that preserves the non-loopback client peer. Transparent reverse proxies
are untrusted and require the remote-control token; they do not inherit loopback
authority.

## Browser and request policy

Control CORS returns only an exact configured HTTP(S) origin. Empty, wildcard,
`null`, credential-bearing, and path-bearing origins are invalid in every mode,
and `X-Api-Key` is no longer advertised. Ambient loopback authority is treated as
a CSRF credential: every control mutation rejects an unallowlisted `Origin` or
`Sec-Fetch-Site: cross-site`, and even empty mutations require
`Content-Type: application/json`. Cross-origin mutation attempts therefore fail
or require a denied preflight. Adding cookie credentials later requires a new ADR
and a session-specific CSRF design before merge.

Request policy is enforced before handlers:

| Request class | Maximum body | Content type |
|---|---:|---|
| `GET`, `HEAD`, `OPTIONS` | none | none |
| control-plane mutation | 2 MiB | `application/json` when a body is present |
| OpenAI transcription | 26 MiB total, with a 25 MiB file | `multipart/form-data` |
| other OpenAI data-plane mutation | 16 MiB | `application/json` |
| server hard ceiling | 26 MiB | endpoint rule still applies |

Empty control bodies remain valid only with the JSON media type. Authentication,
`Content-Length`, endpoint limits, and media types are checked in the pre-routing
hook before httplib buffers a body. Chunked API request bodies are rejected with
411, closing the route-cap bypass. Oversized, body-forbidden, and wrong-media-type
requests return 413, 400, and 415 before a handler can mutate resources.

The server applies a 30-second total header-and-body read deadline, a 30-second
per-read timeout, a 30-second blocked-write deadline, and a 5-second idle
keep-alive deadline. Rejected bodies are subject to the same total deadline, so
a client cannot retain a worker by slowly dripping a body. Any `Expect` header
is rejected with 417; exact `100-continue` is rejected before a body is accepted,
and all other values are rejected in pre-routing. Rejected eager bodies receive
a bounded graceful drain so the complete structured error reaches the client
without allowing an unbounded worker hold. A
slow-body socket probe verifies that a client sending bytes throughout the
window is still released after approximately 30 seconds.

## Request identity

`X-Request-Id` values are accepted only when they contain 1 through 64 ASCII
letters, digits, dots, underscores, or hyphens. Missing or invalid values are
replaced with a process-unique `req_<time>_<sequence>` value. The selected value
is returned in the response header and appears in server-wide structured
request-begin and response-committed records plus handler exceptions. This
covers OPTIONS, static files, unmatched routes, authorization failures, and
pre-buffer request rejection. Response commit is intentionally not labelled as
stream completion: asynchronous inference completion and disconnect outcomes
are recorded by each route's request-outcome path.

## Verification

The Release gateway, `auth_tests`, `route_tests`, and `config_tests` build with
the normalized Windows child environment. Focused CTest execution passes all
three test targets.

`Testing/Test-ControlPlaneSecurity.ps1` launches the no-model Release fixture and
proves both modes over the real listener:

- OpenAI data request without token: 401;
- OpenAI data request with OpenAI token: 200;
- public LAN health: 200;
- loopback control without token: 200;
- remote-disabled control with no token, OpenAI token, or control token: 403;
- remote-enabled control without token or with the OpenAI token: 401;
- remote-enabled control with the control token: 200;
- remote dashboard with the control token: 403;
- remote-disabled control preflight: 403;
- exact remote control origin: returned;
- untrusted remote control origin: 403;
- cross-origin loopback mutation: 403;
- empty mutation without JSON media type: 415;
- proxy-indicated loopback request: 401/403, never ambient authority;
- unauthenticated declared 500 MB body: 401 before upload;
- authenticated 17 MiB JSON body: 413 before upload;
- authenticated 27 MiB multipart body: 413 before upload;
- chunked JSON body: 411;
- exact, mixed-case, unsupported, and duplicate `Expect` headers: 417;
- eager 64 KiB unauthorized body: complete structured 401 without TCP reset;
- wrong OpenAI content type: 415;
- valid request ID: echoed;
- invalid request ID: replaced;
- slow-drip body client: released after approximately 30 seconds despite activity.

The fixture starts a separate gateway on ports 11439 or 11440, contains no model
registry entries, and stops only the process it created. It does not modify or
restart the live gateway on port 11434.
