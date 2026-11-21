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

## Media defaults

`default_model` selects the chat model. Optional `default_models` entries select
the model used when an image, music, or video generation request omits `model`:

```yaml
default_models:
  image: stable-diffusion-xl-1.0-fp16
  music: ace-step-v1.5-xl-sft-bf16
  video: ltx-2.3
```

An explicit request model takes precedence. The image and video pages initially
select their configured default when its runtime is available. These settings
do not download models or add runtime support. Without a media default, image
and video retain the existing `default_model` fallback; music requires `model`.

## Speculative decoding

A `llama_cpp` text model may set `speculative`:

```yaml
speculative:
  type: dflash2            # none | mtp | dflash | dflash2
  draft_model: E:/InferDeck/models/z-lab/Qwen3.8-27B-DFlash2-GGUF/Qwen3.8-27B-DFlash2-Q8_0.gguf
  draft_tokens: 3          # mtp: 1-4 (default 2); dflash/dflash2: 1-16 (default 3; above 4 is slower on Vulkan, see below)
  p_min: 0.0               # 0-1; DFlash2 truncates the draft at a lower selector probability
  max_active_requests: 1   # speculation runs only while this many requests or fewer are runnable
```

- `mtp` drafts from the target GGUF's own MTP head; `draft_model` is rejected.
- `dflash` and `dflash2` load a separate block-diffusion drafter trained for
  the target. `draft_model` is required and must exist. The runtime clamps
  `draft_tokens` to the drafter's trained block size (block 8 drafts at most 7).
  `dflash` accepts any DFlash-family drafter; `dflash2` requires a DFlash2
  candidate selector.
- The runtime contract must declare speculative support, and DFlash is accepted
  only for `runtime: llama_cpp`.
- Both modes share the adaptive window: at more than `max_active_requests`
  runnable requests the model decodes without drafting, and a request that lost
  its draft cache resynchronises before drafting again. Image requests always
  bypass speculation.
- With `context_pool_auto`, the drafter's file size is reserved from the fitted
  pool; its KV cache is covered by the allocation retry.

Measured on an AMD Radeon AI PRO R9700 (Vulkan), Qwen3.8-27B UD-IQ4_XS, one
slot, 48K context, greedy, 512 output tokens:

| Mode | Short essay | ~38K-token essay | Short code | ~38K-token code |
|---|---|---|---|---|
| plain | 35.7 t/s | 32.7 t/s | 35.9 t/s | 32.9 t/s |
| `mtp`, 2 tokens | 57.5 t/s | 44.9 t/s | 71.8 t/s | 55.9 t/s |
| `dflash2`, 3 tokens | 55.8 t/s | 46.6 t/s | 76.1 t/s | 61.1 t/s |
| `dflash2`, 7 tokens | 30.7 t/s | 24.2 t/s | 52.9 t/s | 41.5 t/s |

On this GPU a target verification batch of five or more tokens costs about
twice a four-token batch, so `draft_tokens: 3` is fastest here even though the
drafter's upstream recommendation is 7. Measure before raising it.

## Co-residency and alias fallbacks

When a request needs a model that does not fit beside the resident ones, the
coordinator frees memory in this order: idle context reclamation, calibrated
slot reduction, then eviction. Context reclamation applies to a resident
llama.cpp model with `kv_unified: true`, `context_pool_auto: true` and
`context_pool_size: 0`. It rebuilds that model's KV pool smaller without
unloading its weights, down to one request's `context_size`, so the incoming
model can load beside it. `n_slots` is unchanged; the slots share the smaller
pool and wait for each other when their combined demand exceeds it. The rebuild
clears that model's prompt cache. It only runs while the model is idle.

The model that gave up context is the lender; the model admitted into that space
is its borrower. When a request for the lender needs more context than its
reduced pool holds, the coordinator evicts its idle borrowers and rebuilds the
pool at full size, again without reloading weights. If a borrower is still
running, the lender's request waits and retries when that work ends. A model
marked `residency: always` or `eviction_eligible: false` is never evicted for a
restore. Models without `context_pool_auto` keep the previous behaviour and are
evicted when the incoming model does not fit.

`model_aliases[].fallbacks` is an optional ordered list of concrete models:

```yaml
model_aliases:
  - name: everyday
    target: qwen3.5-4b
    fallbacks: [qwen3.8-27b]
    required_context_size: 16384
```

A request for the alias uses `target` when it is resident and ready. Otherwise
it uses the first listed fallback that is resident and ready. If none is, it
loads `target`. Every fallback must be a distinct registered model that
satisfies the alias's `required_context_size` and `required_capabilities`.
A model named by an alias fallback cannot be unregistered. Aliases without
`fallbacks` behave as before.

The choice is made once, when the request is resolved. If the chosen model is
unloaded before the request is admitted, `auto_swap` loads that model again;
the request does not switch candidates. Fallback compatibility is checked when
the alias is created or retargeted, not on every request.

A restore is refused while the lender has active requests or any borrower is
active, draining or resizing; the request waits and retries without evicting
anything. A lender with continuous traffic therefore restores only in a gap
between its requests, and the waiting request gives up after the model's
`request_queue_timeout_seconds`. A borrower that cannot be evicted makes the restore fail with an
out-of-memory error, so the lender keeps its reduced pool and its cache.

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

`auth.api_keys_db` selects the separate managed-key database. Managed key
plaintext is returned only by the create endpoint and is never written to YAML
or returned by the configuration API. See [Managed API keys](api-keys.md).

`gateway.background_idle_after_seconds` sets the quiet period required before a
managed client can acquire the global background lease. It accepts 60 to 86400
seconds and defaults to 900. Lease records use the managed-key database and
survive gateway restarts. See
[Background availability and leases](background-leases.md).

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
