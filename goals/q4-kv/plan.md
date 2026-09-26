# Q4 KV for Qwen3.8 27B

## Objective

Serve Qwen3.8 27B through the actual Windows InferDeck Radiance runtime with an INT4/Q4 KV cache, four usable 106496-token slots, coherent tool/reasoning output, and representative two-agent tool-loop time similar to the current BF16/R4D production profile. Use 10% above the 133.684 s BF16 median (147.052 s) as the initial screening limit, then judge repeat runs and output lengths before promotion. Measure complete client elapsed time, cache reuse, PP, TPS, and VRAM. Keep the BF16/R4D production path recoverable. Q4 Radiance vision is optional; preserve the existing Vulkan vision path.

## Constraints

- Use real InferDeck API requests for promotion evidence. Synthetic kernel checks are diagnostic only.
- Preserve the current production executable/configuration until a candidate passes real-model quality, latency, and recovery checks.
- One isolated staged candidate on port 11435; restore production after each GPU window.
- No context reduction, output truncation, or changed reasoning/sampling to claim speed.
- Do not deploy or commit private untracked kernel files as if they were reproducible source.

## Ordered work

1. Preserve and build the exact INT4 prefill/decode source and Python overlay used by the staged candidate; verify the staged imported file hashes match tracked source.
2. Keep the alpha64 real-app four-short-request and 64K+2K results as evidence, including the worker's false failure from a fixed warm-prompt-token expectation. Repeat with the corrected cache check after any performance change.
3. Attribute the measured two-agent delay and try one bounded PP/scheduling improvement at a time. Compare total client elapsed, each request, cache counts, output lengths, PP, TPS, and VRAM with the BF16/R4D evidence. Do not promote a slower profile as default.
4. Verify four concurrent ~100K requests, repeated tool/reasoning quality, and original-model recovery on a source-reproducible staged build. Make tested local commits and document build/deployment/rollback. Deploy only a passing candidate.

## Baseline and failed candidate

- BF16/R4D frozen two-agent 64K+2K fixture: three-run total-time median 133.684 s. Evidence: `.UAM/AINotes/r4d-timer-free-three-rep.json`.
- Existing prefill-only INT4 alpha58: one real InferDeck run 184.150 s, both exact tool tasks passed and warm cache reused, but 37.75% slower than that historical median. Evidence: `.UAM/AINotes/q4kv-64k2k-20260923T231009248Z-worker.json`, `.UAM/AINotes/r4d-int4-alpha58-audit.json`.
- Private split-K INT4 decode kernel passed one-layer GPU parity and a 64K synthetic microbenchmark; this does not establish application speed. Evidence: `.UAM/AINotes/int4-decode-window-20260923T233430845Z.json`.

The active orchestration goal is the Q4 four-slot objective. The broader runtime-PP goal remains separate and unfinished.
