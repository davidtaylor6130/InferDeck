# Architecture Decision Records

Architecture Decision Records (ADRs) document decisions that change InferDeck's
protocol, process, resource, security, observability, or deployment boundaries.
They are part of the implementation contract, not retrospective notes.

## Statuses

- `Proposed`: open for review and not yet binding.
- `Accepted`: approved and binding on new work.
- `Superseded`: replaced by a later ADR that links back to it.
- `Rejected`: considered but not adopted.

## Workflow

1. Copy [template.md](template.md) to the next zero-padded number.
2. Describe the current facts, decision, consequences, migration, and proof.
3. Link the ADR from the implementing issue and pull request.
4. Obtain review from the owners of every affected boundary.
5. Mark the ADR `Accepted` before merging dependent implementation work.
6. Supersede decisions with a new ADR; do not rewrite accepted history.

## Decisions

| ADR | Status | Decision |
|---|---|---|
| [0001](0001-openai-core-boundary.md) | Accepted | OpenAI-only Core architecture boundary |
| [0002](0002-authenticated-remote-dashboard.md) | Accepted | Authenticated remote dashboard sessions |
