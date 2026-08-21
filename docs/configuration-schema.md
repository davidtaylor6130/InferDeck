# Configuration schema and transactions

InferDeck configuration uses YAML schema version 1:

```yaml
schema_version: 1
server:
  host: 127.0.0.1
  port: 11434
```

Tracked configurations declare the version explicitly. Unversioned
configurations are accepted as legacy version 1 during migration. The
`extensions` mapping is available for version-owned third-party data only when
`schema_version` is explicit.

## Validation

Schema validation runs before semantic decoding. Unknown keys are rejected with
their complete path, including sequence indexes, for example:

```text
unknown configuration key: gateway.sampling.tempertaure
unknown configuration key: model_registry[0].runtme
```

Runtime declarations are checked through registered runtime contracts. Each
contract owns its modalities, capabilities, artifact policy, and support for
vision, reasoning, and speculative decoding. A runtime cannot silently accept a
modality or capability it did not register.

## Files and selection

- The stable base is the requested `gateway.yml`.
- The active profile is `gateway.active.yml` beside the base.
- A valid active profile is selected at startup.
- An invalid active profile is rejected and startup falls back to the base while
  retaining the validation error as the fallback reason.

The gateway exposes masked base and active documents through
`GET /api/inferdeck/v1/config`.

## Revisions and mutations

Revisions are deterministic hashes of the exact persisted document. Base save,
active save, aliases, model unregister, and active reset require the revision
last read by the client. A stale revision returns HTTP 409 and does not mutate
disk or in-memory state.

- Base and active saves send `revision` in the JSON body.
- Alias saves send `revision` in the JSON body.
- Alias deletion and active reset send the active revision in `If-Match`.

All operations use one `ConfigRepository` transaction lock. Alias and model
registry mutations execute inside that transaction, so concurrent config,
alias, unregister, and reset requests cannot overwrite one another.

## Secrets

`auth.token`, `control.token`, and `model_store.hf_token` are returned as
`__INFERDECK_SECRET__`. Saving the masked document restores the corresponding
persisted values inside `ConfigRepository` before validation. Secret handling
preserves surrounding YAML and comments.

## Atomicity and recovery

Persistence writes a sibling temporary file, flushes it, and atomically replaces
the destination. Active mutations then schedule reload while retaining the
transaction lock.

If reload scheduling fails:

1. The previous active file is restored, or a newly created active file is
   removed.
2. In-memory alias or model-registry state is rolled back.
3. The request fails and reports that recovery occurred.

Reset uses the inverse sequence: it removes the active file, schedules reload,
and recreates the prior active file if reload scheduling fails.

## Version migration

Version 1 is the only supported schema. Migration to a future version must:

1. Back up the base and active files.
2. Transform a complete document without discarding comments or secrets.
3. Validate the target version before replacement.
4. Atomically replace the destination.
5. Restore the backup if validation, persistence, or reload fails.

Unknown future versions are rejected rather than interpreted as version 1.
