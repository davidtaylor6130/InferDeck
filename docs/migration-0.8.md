# InferDeck 0.8 migration

InferDeck 0.8 makes the Core data plane strictly OpenAI and keeps privileged
operations under `/api/inferdeck/v1`. Treat the executable, dashboard static
directory, configuration schema, and StatsDb as one migration unit.

## Breaking route changes

| Previous route or behavior | 0.8 replacement |
|---|---|
| `/v1/health` | `/api/inferdeck/v1/health` |
| `/v1/swap/*` | `/api/inferdeck/v1/swap/*` |
| runtime metadata in `/v1/models` | `/api/inferdeck/v1/models` |
| vendor fields on `/v1` | default-off `/compat/openai-derivative/v1` or remove them |
| Anthropic Messages | use an OpenAI Chat Completions or Responses client |
| forced alignment through Core | operate the separately preserved companion |

Strict `/v1` contains only Chat Completions, Responses, embeddings, image
generations, speech, transcriptions, and model discovery. Responses is
stateless. Unsupported standard fields fail with an OpenAI error envelope; they
are never silently ignored.

## Consumer inventory

- InferDeck dashboard: same-origin `/api/inferdeck/v1` plus one SSE event
  stream; deploy it from the same revision as the executable.
- OpenCode and Open WebUI: configure the OpenAI base URL
  `http://HOST:11434/v1` and the data-plane bearer token.
- Official OpenAI Python and JavaScript clients: use the same base URL and
  token; contract pins are listed in `openai-compatibility.md`.
- Operator scripts: replace legacy health, swap, model metadata, jobs, logs,
  metrics, and configuration paths with their control-plane equivalents.
- Non-OpenAI clients: migrate to Chat Completions or Responses before
  activation. Core no longer supplies a protocol adapter for them.

Search local consumer configuration and scripts for `/v1/health`,
`/v1/swap`, `/v1/messages`, `/compat/anthropic`, and vendor request
fields. A zero-result search is part of pre-deployment evidence.

### Verified host inventory (2026-08-21)

- OpenCode uses the InferDeck provider at `http://100.95.44.9:11434/v1`
  and therefore needs no route migration.
- The deployed dashboard still polls legacy `/api/status`, `/api/pricing`,
  `/v1/health`, and `/v1/models`; deploying the matched 0.8 static directory
  migrates it.
- Repository smoke and lifecycle scripts already use
  `/api/inferdeck/v1`. The legacy `/v1/messages` path in the security fixture is
  a deliberate negative test.
- No Open WebUI process or local container was running, and the local Docker
  engine was unavailable. Any remote Open WebUI instance must be verified from
  its own configuration before activation.
- Recent gateway logs show dashboard polling, Chat Completions, transcription,
  and model discovery. They do not record enough client identity to prove that
  no additional remote consumer exists, so the activation window must retain a
  rollback path and monitor 404/400 responses.

## State migration

1. Record the source revision, executable hash, static asset hashes,
   configuration revision, listener owner, and boot mechanism.
2. Back up `gateway.yml`, StatsDb including WAL/SHM siblings, managed-model
   manifests, executable, static directory, and startup definition.
3. Validate configuration against the versioned schema before replacing the
   running copy.
4. Open a copied StatsDb with the new binary first. Schema migration creates a
   versioned backup and rolls back transactionally on failure.
5. Build and stage the executable and dashboard from one source revision.
6. Stop only the verified InferDeck process, atomically activate the matched
   pair and migrated configuration, then restart through the authoritative boot
   mechanism.

Dashboard pricing previously stored in localStorage is not authoritative.
Server pricing and canonical all-time usage buckets own 0.8 totals. Existing
browser overrides may be exported for reference and then cleared after values
are represented in server configuration.

## Acceptance and rollback

Run every probe in the complete verification matrix in `overhall_plan.md`.
Confirm strict route manifests, both official SDKs, stream and non-stream Chat
and Responses, media formats, control rejection, request correlation, metrics,
StatsDb, dashboard assets, and the absence of runtime child processes.

Rollback is a matched-pair operation: stop the verified new process, restore
the prior executable, complete static directory, configuration, and StatsDb
backup, restart through the same boot mechanism, and repeat health, model,
dashboard, and request probes. Never combine an executable from one revision
with static assets or migrated state from another.
