# InferDeck documentation

- [`adr/`](adr/) - binding architecture decisions and decision template
- [`control-plane-security.md`](control-plane-security.md) - route principals, authentication, CORS, request limits, and security probes
- [`openai-compatibility.md`](openai-compatibility.md) - pinned strict OpenAI baseline, supported subset, and explicit rejections
- [`configuration-schema.md`](configuration-schema.md) - versioned schema and transactional migration
- [`migration-0.8.md`](migration-0.8.md) - breaking routes, consumers, backups, activation, and rollback
- [`release-reproducibility.md`](release-reproducibility.md) - pinned inputs, manifests, checksums, SBOM, and signing
- [`architecture-change-checklist.md`](architecture-change-checklist.md) - required review gates for architecture changes
- [`architecture.md`](architecture.md) — system architecture, layer by layer
- [`DEPLOY.md`](DEPLOY.md) — unattended Windows deployment (scheduled tasks + watchdog)
- [`opencode-setup-guide.md`](opencode-setup-guide.md) — pointing opencode at InferDeck
- [`v2-cleanup-report.md`](v2-cleanup-report.md) — historical v2 audit, superseded by [`overhall_plan.md`](../overhall_plan.md)
- [`assets/`](assets/) — README banner and other artwork

The API surface is summarised in the [root README](../README.md#api-surface);
engineering conventions, build/test commands, and concurrency invariants live
in [`AGENTS.md`](../AGENTS.md).
