# InferDeck documentation

- [`adr/`](adr/) - binding architecture decisions and decision template
- [`control-plane-security.md`](control-plane-security.md) - route principals, authentication, CORS, request limits, and security probes
- [`api-keys.md`](api-keys.md) - create, prioritize, use, and revoke managed client keys
- [`background-leases.md`](background-leases.md) - check inactivity and coordinate low-priority background clients
- [`openai-compatibility.md`](openai-compatibility.md) - pinned strict OpenAI baseline, supported subset, and explicit rejections
- [`configuration-schema.md`](configuration-schema.md) - versioned schema and transactional migration
- [`migration-0.8.md`](migration-0.8.md) - breaking routes, consumers, backups, activation, and rollback
- [`release-reproducibility.md`](release-reproducibility.md) - pinned inputs, manifests, checksums, SBOM, and signing
- [`architecture-change-checklist.md`](architecture-change-checklist.md) - required review gates for architecture changes
- [`decisions.md`](decisions.md) - `POST /v1/decisions`, decision models, and the Jev-Style runtime
- [`providers.md`](providers.md) - how runtimes plug in, and the C ABI for provider libraries in any language
- [`qwen-image-2.1.md`](qwen-image-2.1.md) - Qwen-Image-2.1 weights, configuration, image-edit API, and runtime pin
- [`architecture.md`](architecture.md) — system architecture, layer by layer
- [`DEPLOY.md`](DEPLOY.md) — unattended Windows deployment (scheduled tasks + watchdog)
- [`opencode-setup-guide.md`](opencode-setup-guide.md) — pointing opencode at InferDeck
- [`v2-cleanup-report.md`](v2-cleanup-report.md) — historical v2 audit, superseded by [`overhall_plan.md`](../overhall_plan.md)
- [`assets/`](assets/) — README banner and other artwork

The API surface is summarised in the [root README](../README.md#api-surface);
engineering conventions, build/test commands, and concurrency invariants live
in [`AGENTS.md`](../AGENTS.md).
