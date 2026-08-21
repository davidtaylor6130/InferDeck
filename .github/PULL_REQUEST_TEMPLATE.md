## Outcome

State the user-visible result and the behavior that is now proven.

## Scope and tracking

- Overhaul phase:
- Closes:
- Depends on:
- ADR:
- Explicitly out of scope:

## Architecture impact

- [ ] Core remains one native executable with no subprocess or proxy runtime.
- [ ] `/v1` changes conform only to the pinned OpenAI contract.
- [ ] Backends receive typed domain objects, not HTTP or protocol JSON.
- [ ] Validation finishes before admission, loading, swapping, or slot acquisition.
- [ ] Runtime work uses coordinator admission, cancellation, shutdown, and metrics.
- [ ] No untrusted request field controls scheduler priority.
- [ ] Unsupported behavior returns a deterministic OpenAI-shaped error.

Describe every changed invariant, or state why this change cannot affect it.

## Contract and control-plane impact

- OpenAI endpoints, schemas, errors, and SSE framing:
- `/api/inferdeck/v1` administration and authorization:
- Compatibility profiles or client migration:

## Resource and lifecycle impact

- Compute and model roles:
- Residency, slots, deadlines, and cancellation:
- Startup, shutdown, failure, and rollback behavior:

## Security and data impact

- Authentication, authorization, bind address, CORS, and secrets:
- Configuration, database, telemetry, pricing, and data migrations:

## Verification evidence

- [ ] Focused unit tests
- [ ] Applicable integration and OpenAI contract tests
- [ ] Architecture checks
- [ ] Release gateway build
- [ ] Dashboard tests and build, or not applicable
- [ ] Documentation and route inventory truth check
- [ ] Reviewed diff excludes unrelated work
- [ ] Deployment and live-path evidence, or not applicable

Commands, results, logs, screenshots, and fixture references:

## Authorization gates

- [ ] No tracked files or user data are moved or deleted.
- [ ] No live `C:\InferDeck` state is changed.

If either box cannot be checked, link the owner's explicit authorization and
describe the exact targets, preservation method, and rollback path.
