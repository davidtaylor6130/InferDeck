# InferDeck dashboard and optimization work log

Date: 2026-07-26

This file is the running implementation record for the dashboard/catalogue work
and the later model auto-optimization system. It is written so the completed
sections can be reused for GitHub issues, a pull request description, and
version notes.

## Scope and delivery order

1. Finish the GUI, catalogue, local-model inventory, usage, ROI, chart, and
   diagnostics changes.
2. Test, build, visually verify, and deploy that GUI/backend release to the
   live `C:\InferDeck` service.
3. Design and implement the model auto-optimization system as a separate,
   safety-gated phase.

## User-facing requirements

- Recommend credible Hugging Face models instead of sorting by newest upload.
- Rank catalogue results by adoption and show multimodal, recommendation, and
  VRAM-fit information.
- Show every configured model and relevant model artifact already present
  under the live model library, including files not downloaded by InferDeck.
- Keep model downloads/removal safe: only store-managed artifacts may be
  removed by the dashboard.
- Make usage headline values follow the selected 24-hour, week, month, year,
  or all-time range.
- Keep ROI and break-even progress global on Home only.
- Use line charts. Home combined usage is one line per AI service type, not
  prompt/output token lines.
- Give all-time charts useful resolution when there are fewer than 12 monthly
  buckets by using finer persisted buckets and limiting visual density.
- Keep configuration recovery and advanced diagnostics collapsed by default.
- Expose safe model-profile controls clearly. KV-cache quantization is a
  runtime/context-memory setting; seed remains an advanced reproducibility
  control and should default to random.
- The later optimizer must benchmark in the background, refuse to disturb an
  active workload, and recommend a quality-first balance of quality, speed,
  concurrency, and memory headroom.

## Previously completed in this workstream

This section records completed work that predates the current catalogue/usage
pass so this file remains a complete issue, PR, and release-note source.

### Home-LAN and Tailscale access

- Restored the home-lab network contract by binding the gateway to `0.0.0.0`
  instead of a loopback-only or Tailscale-specific address.
- Kept CORS open for the trusted home-lab deployment.
- Added a configuration regression test for LAN binding so future changes
  cannot silently make the dashboard local-only again.
- Verified the live service from:
  - LAN: `192.168.0.168`;
  - Tailscale: `100.95.44.9`;
  - loopback: `127.0.0.1`.
- The solution remains network-agnostic: there is no hard-coded Tailscale
  allow-list or interface binding.

### Persistent SQL usage and token accounting

- Anchored observability storage to
  `C:/InferDeck/data/stats.db`, independent of the service account and current
  working directory.
- Restored lifetime aggregation across service/gateway restarts rather than
  resetting statistics per process instance.
- Persisted and surfaced request count, prompt tokens, completion tokens,
  total tokens, average tokens/second, peak tokens/second, and estimated hosted
  API-equivalent value.
- Added hourly, daily, monthly, and lifetime SQL aggregation used by the
  dashboard.
- At the v0.5.1 live verification point, the recovered database contained
  6,925 requests and 262,794,334 total tokens.

### Native dictation backends

- Replaced the Windows-SAPI-only direction with configurable native speech
  runtimes suitable for Open WebUI-compatible OpenAI audio endpoints.
- Added/integrated the CPU-oriented speech profiles:
  - `parakeet-tdt-0.6b-v3` for speech-to-text;
  - `supertonic-3` for text-to-speech.
- Kept dictation as a server API capability. No microphone, recording,
  playback, or voice-chat client was added to the administration dashboard.
- Added speech usage fields to the SQL ledger:
  - input audio duration for transcription;
  - input character count for speech synthesis.
- Added hosted-API comparison pricing for speech workloads so dictation can
  contribute to total ROI.
- Added native runtime tests for Whisper, sherpa ASR, sherpa TTS, and Windows
  SAPI compatibility/fallback code paths.

### Dashboard information architecture

- Added a global Home page with aggregate status, LLM and Dictation summaries,
  live hardware information, combined usage, and recent activity.
- Split LLM and Dictation into their own Operate, Models, Usage, and
  Diagnostics sections.
- Kept the dashboard as a headless-server administration surface rather than
  implementing end-user AI interactions in it.
- Reworked Operate into a compact runtime table with load/unload actions and a
  Model Details modal instead of repeated model cards.
- Added model-profile editing for slots, minimum slots, context, VRAM budget,
  GPU layers, and sampling values.
- Moved model discovery/download management to Models and removed load history
  from that page.
- Added section-specific LLM and Dictation usage filtering so speech models no
  longer pollute LLM statistics.
- Added model/store download controls, resumable jobs, validated artifact
  metadata, size/checksum enforcement, and safe store-owned removal.
- Added the application version in the lower-left navigation area.

### Stable and active configuration profiles

- Preserved `gateway.yml` as the stable recovery baseline.
- Added a separately saved active profile for dashboard changes.
- Added validation, revision/conflict checks, secret masking/restoration, and
  atomic profile writes.
- Added per-model baseline restore and a complete active-profile reset.
- Added startup fallback: if the active profile cannot load, InferDeck falls
  back to the stable baseline and reports the reason.

### Headless hot apply

- Removed the manual machine/service restart requirement when saving an active
  profile.
- Saving or resetting the active profile now schedules an in-process gateway
  reload, allowing the LocalSystem NSSM service to remain installed and
  headless.
- Added UI polling that waits for the requested revision to become the running
  revision and reports success/failure after the gateway reconnects.
- Live smoke testing confirmed:
  - the active revision became the running revision;
  - gateway uptime reset as expected during in-process reload;
  - NSSM remained running;
  - persisted logs and the SQL usage database were retained.

### v0.5.1 build, regression suite, and live cutover

- Built and deployed v0.5.1 to the live `C:\InferDeck` service.
- Passed 101/101 focused C++ tests.
- Passed 28/28 dashboard tests.
- Verified the active Qwen 27B profile with four configured slots.
- Verified LAN, Tailscale, health, models, configuration revision, persistent
  database totals, and automatic active-profile apply against the live service.
- Created the pre-cutover backup:
  `C:\InferDeck\backups\v051-precutover-20260726-014545`.

## Findings

- The catalogue backend used Hugging Face `sort=lastModified`, which promoted
  newly uploaded, barely used repositories.
- The LLM range selector rebuilt the graph, but its headline estimated API
  cost still used lifetime usage.
- Dictation usage had no range selector, used a bar chart, and duplicated the
  global ROI/break-even controls.
- Home combined usage used stacked monthly bars and was limited to the last 12
  monthly rows.
- The Downloaded Models panel only read the InferDeck store manifest. The live
  manifest is empty even though `C:\InferDeck\models` contains the configured
  LLM, STT, and TTS model libraries.
- Live inventory at audit time: 10 configured models, 25 GGUF/ONNX/BIN
  artifacts, AMD Radeon AI PRO R9700 with approximately 32 GB VRAM, and no
  active or queued requests.

## Changes completed

### Catalogue and local inventory backend

- Changed Hugging Face search ordering from newest-first to downloads-first,
  with likes as the tie-breaker.
- Added catalogue `recommended` and `hasVision` metadata.
- Corrected ONNX speech artifact classification so ONNX ASR can use the
  sherpa-onnx runtime and retain the transcription modality.
- Added a read-only local library inventory that merges:
  - models registered in the active InferDeck profile;
  - InferDeck store-managed models;
  - compatible unregistered files found beneath the configured model-library
    root.
- Local inventory reports configuration/management status, aggregate size,
  quantization, artifact count, and multimodal projection presence. External
  files remain read-only.

### Catalogue and local inventory GUI

- Replaced newest-upload discovery with quality-oriented categories for:
  - recommended 20–40B LLMs;
  - multimodal LLMs;
  - coding LLMs;
  - efficient LLMs;
  - recommended STT;
  - Whisper GGUF;
  - Parakeet ONNX;
  - recommended TTS;
  - Kokoro ONNX.
- Added a Recommended Only filter. LLM discovery requires meaningful adoption
  by default; speech uses a lower threshold because its compatible ecosystem is
  smaller.
- Added download/like counts, recommendation badges, and multimodal icons.
- Added artifact-level hardware-fit badges against the detected 32 GB VRAM
  device, with explicit warning that context and parallel KV cache require
  additional headroom.
- Renamed Downloaded Models to Models on This Server and now show:
  - active-profile models;
  - store-managed downloads;
  - compatible unconfigured artifacts already on disk.
- Only store-managed artifacts expose a Remove action. Existing external
  libraries are visibly read-only.
- Speech catalogue inspection distinguishes single-file Whisper models from
  multi-artifact sherpa-onnx repositories. Individual sherpa ONNX downloads
  are disabled and labelled `Bundle required` so the UI cannot create an
  incomplete, non-runnable STT or TTS installation.
- Isolated gateway proof detected 12 real model groups beneath
  `C:\InferDeck\models`: 10 LLM groups and two speech groups, including five
  multimodal/projection groups.

### Usage, economics, and charting GUI

- LLM headline total, prompt, output, and estimated API cost now all follow the
  selected 24-hour, week, month, year, or all-time range.
- Removed LLM ROI remaining and break-even controls from the LLM Usage page.
- Added the same time-range control to Dictation Usage.
- Dictation headline requests, successes, failures, audio duration, synthesized
  characters, and API-equivalent value now follow the selected range.
- Removed Dictation ROI remaining and break-even controls from the Dictation
  Usage page.
- Replaced Dictation monthly bars with a line graph.
- Replaced Home combined stacked bars with one line per AI service:
  - LLM requests;
  - Dictation requests.
- Added shared range controls to Home combined usage.
- Added common time buckets so service lines align even when one service has no
  activity in a period.
- Added adaptive all-time resolution:
  - fewer than 12 recorded months uses persisted daily detail;
  - daily detail is resampled to at most 24 useful points;
  - 12 or more months uses monthly buckets;
  - all-time headline totals remain complete rather than using a truncated
    recent window.
- Kept portfolio break-even progress and ROI remaining on Home only.
- Moved the global break-even target into a collapsed Home Portfolio
  Assumptions section.

### Model settings and diagnostics GUI

- Added shared KV-cache key/value precision controls to Model Details:
  Q4, Q8, and F16.
- Clearly separated KV-cache/context quantization from GGUF model-file
  quantization.
- Added prompt batch, physical batch, and flash-attention controls.
- Documented Q8 as quality-first, Q4 as memory-headroom oriented, and F16 as
  maximum precision.
- Kept seed as a per-request control with random default. A fixed seed will be
  used internally by the later optimizer for reproducible comparisons rather
  than making normal responses deterministic.
- Collapsed Configuration Recovery by default and retained the confirmation
  gate for resetting active changes.
- Collapsed Dictation gateway logs under Advanced Dictation Diagnostics.

### v0.5.2 version

- Bumped gateway and dashboard version strings to v0.5.2 for this GUI release.

## Changes in progress

- A measured in-process quality benchmark remains a separate maintenance-mode
  design because testing reload-only settings requires model reloads. The
  dashboard must not mislabel synthetic or heuristic scoring as measured.
- A repository-level sherpa-onnx bundle installer is intentionally deferred;
  it must download, checksum, and atomically register every required runtime
  artifact instead of treating an arbitrary ONNX file as a complete model.

## Tests and live proof

- Dashboard: 29/29 tests passed.
- C++/integration: 109/109 tests passed. The sandboxed aggregate run initially
  denied one Foundation temp-path cleanup; the exact test passed when rerun
  outside the restricted sandbox.
- Release build completed for the dashboard and full C++ solution.
- Focused route/observability run: 36/36 passed.
- Isolated loopback gateway smoke:
  - model library entries: 12;
  - configured entries in the minimal smoke profile: 2;
  - additional on-disk groups: 10;
  - multimodal/projection groups: 5;
  - LLM groups: 10;
  - Dictation groups: 2;
  - adaptive daily all-time mode: enabled.
- The temporary loopback sidecar and its test database/log files were removed.
- In-app browser visual automation is currently unavailable because the
  browser-control runtime rejects its own request metadata before setup
  (`codex/sandbox-state-meta: missing field sandboxPolicy`). No standalone
  browser fallback was used.
- Live Hugging Face API proof:
  - Qwen3.6 GGUF returned 30 LLM results in descending download order;
  - `ASR ONNX` returned 35 transcription repositories;
  - `TTS ONNX` returned 25 speech-synthesis repositories;
  - Whisper, Parakeet, and Kokoro presets returned 44, 41, and 34 results.
- Deployed gateway/backend v0.5.2 to the LocalSystem NSSM service using the
  backup `C:\InferDeck\backups\v052-precutover-20260726-022254`.
- Applied the final catalogue asset correction without restarting the service
  and created backup
  `C:\InferDeck\backups\v052-static-precutover-20260726-022825`.
- Final live proof:
  - binary: `inferdeck-gateway 0.5.2`;
  - service: `SERVICE_RUNNING`;
  - loopback, LAN `192.168.0.168`, and Tailscale `100.95.44.9`: healthy;
  - SQL database: `C:/InferDeck/data/stats.db`;
  - retained totals: 6,925 requests and 262,794,334 tokens;
  - active/queued requests at verification: 0/0;
  - registered models: 10;
  - all-time daily usage detail: enabled;
  - active dashboard asset: `index-BA1teWAO.js`;
  - final asset contains STT/TTS discovery presets, local-server inventory,
  and the sherpa bundle safety guard.

## Quality-first profile optimization

### Implemented

- Added an in-process profile recommendation engine to `libs/optimize`.
- Added a safety-gated `POST /api/optimize/profile` dashboard endpoint.
- The endpoint refuses analysis when:
  - active requests are non-zero;
  - queued requests are non-zero;
  - a model swap is active;
  - GPU telemetry is unavailable;
  - GPU utilization exceeds 20 percent.
- Candidate inputs use:
  - the actual GGUF artifact size;
  - multimodal projection size only when vision is enabled;
  - detected total VRAM;
  - configured VRAM budget;
  - context size explicitly per slot;
  - configured and minimum slots;
  - KV key/value precision;
  - prompt and physical batch sizes;
  - persisted output throughput when available.
- Candidates preserve a 12 percent VRAM safety reserve and score:
  - quality: 60 percent;
  - speed: 15 percent;
  - parallelism: 15 percent;
  - memory headroom: 10 percent.
- Model-file quantization is not treated as a runtime toggle. Changing Q4,
  Q8, or another GGUF quant requires selecting a different downloaded
  artifact.
- Normal request seeds remain random. The profile analyser does not edit seed.
- Added Quality-first Profile Analysis to LLM Model Details.
- Analysis is asynchronous from the browser but lightweight and in-process.
  It never starts a subprocess, loads a model, runs inference, reloads the
  gateway, or mutates configuration.
- Results are explicitly labelled as estimates rather than measured quality
  benchmarks.
- Stage Recommendation updates only the open YAML draft. The user must still
  review and explicitly save the validated active profile before hot apply.

### One-click usability follow-up

- Added a visible `Auto-optimize values` action to every LLM row in Operate.
- One click now:
  - opens that model's Model Details dialog;
  - calls the safety-gated profile analyser;
  - compares the safe candidates;
  - stages context per slot, slots, KV key/value precision, prompt batch,
    physical batch, and flash attention in the active-profile draft.
- The same action remains available inside Model Details for rerunning the
  analysis after manual changes.
- Saving remains a separate explicit action so optimization cannot silently
  reload a headless production service.
- Added a pure YAML-staging regression proving every recommended field is
  written to the selected model and shared gateway settings.
- Dashboard regression count after this follow-up: 31/31.
- The production bundle contains the visible action. In-app browser automation
  remains unavailable because its runtime rejects its own session metadata
  before connecting; no standalone browser fallback was used.

### Optimization tests and proof

- Added four optimizer regression tests covering:
  - a fitting quality-first recommendation;
  - per-slot context remaining separate from slot count;
  - invalid profile rejection;
  - demotion of candidates that breach the VRAM reserve.
- Added dashboard-route regression coverage for:
  - a valid quality-first recommendation;
  - rejection while GPU utilization is above the safety threshold.
- Added dashboard API coverage proving the optimiser posts explicit per-slot
  and runtime fields without applying configuration.
- Focused optimizer plus route suite: 28/28 passed.
- Dashboard suite after optimizer UI/API integration: 30/30 passed.
- Complete C++ unit/integration suite: 113/113 passed.
- Isolated loopback v0.5.3 API proof on port 11436 used:
  - real Qwen3.6 27B GGUF size: 16,038 MB;
  - detected GPU capacity: 32,022 MB;
  - requested target: 100,000 context tokens per slot and four slots;
  - recommendation: 100,000 per slot, four slots, Q4/Q8 KV,
    2,048/2,048 batches;
  - estimated VRAM: 24,000 MB;
  - estimated reserve: 8,022 MB;
  - quality score: 97.7 percent;
  - overall score: 98.3 percent;
  - candidates compared: eight;
  - loaded models during analysis: zero.
- The isolated API returned 404 for an unknown model. The sidecar and every
  temporary config, log, state, and database artifact were removed.
- A final route regression passed after excluding disabled vision projections
  from the resident-size estimate. Dashboard remained 30/30.

### Live v0.5.3 cutover state

- Release binary:
  `C:\Users\david\Documents\GitHub\InferDeck\build\bin\Release\inferdeck-gateway.exe`.
- Release static bundle:
  `index-DJvAaKNm.js` and `index-D68mq8yi.css`.
- Guarded deployment helper:
  `C:\tmp\deploy-inferdeck-v053.ps1`.
- Preflight before attempted cutover:
  - running requests: zero;
  - queued requests: zero;
  - swap active: false;
  - loaded models: zero;
  - GPU utilization: approximately 1.3 percent;
  - VRAM used: approximately 804 MB.
- Two attempts to launch the helper with `Start-Process -Verb RunAs` waited for
  UAC and timed out before the helper created a backup or result marker.
- Both post-attempt audits confirmed:
  - no v0.5.3 backup was created;
  - no deployment helper remained running;
  - the live binary remained v0.5.2;
  - NSSM remained running;
  - `/v1/health` remained healthy;
  - the persistent database remained
    `C:/InferDeck/data/stats.db` with 6,925 requests.
- No workaround was used to bypass Windows service permissions. At that point,
  accepting the UAC prompt was the only remaining cutover action.
- After the one-click follow-up, another guarded launch was attempted while
  active/queued requests were zero, swap was idle, and GPU utilization was
  approximately 1.7 percent. One idle model was resident.
- The elevated helper again did not start or publish a result. The audit
  confirmed no backup, no helper process, live v0.5.2 still running, a healthy
  SQL database at `C:/InferDeck/data/stats.db`, and 6,926 persisted requests.
- The final interactive elevation was accepted and the guarded helper completed
  successfully:
  - rollback backup:
    `C:\InferDeck\backups\v053-precutover-20260726-103735`;
  - live gateway version: `0.5.3`;
  - live binary SHA-256:
    `C7E435BD17D92024E105F3312B609AE1104E2AED571611202A54B0559051D19B`;
  - live executable and static `index.html` hashes match the release artifacts;
  - the stale dashboard bundle was removed and the live assets are
    `index-DJvAaKNm.js` plus `index-D68mq8yi.css`;
  - `/v1/health` returned HTTP 200 with the retained
    `C:/InferDeck/data/stats.db` ledger and 6,949 requests;
  - the live optimiser returned HTTP 200 for Qwen3.6 27B with 100,000 context
    per slot, four slots, Q4/Q8 KV cache, 2,048/2,048 batches, and a fitting
    24,000 MB estimate;
  - `0.0.0.0:11434` is listening;
  - both `192.168.0.168:11434` and Tailscale
    `100.95.44.9:11434` returned HTTP 200 for the health API and dashboard.

## GitHub issue coverage

Every substantive product workstream in this log is now represented by a
formatted GitHub issue. Build, deployment, rollback, and live-probe entries are
validation evidence for those product issues and PR #63 rather than separate
features.

| Workstream | GitHub issue | PR #63 relationship |
| --- | --- | --- |
| Home-LAN and Tailscale-routed access | #64 | Closes |
| Stable lifetime statistics and token ledger | #65 | Closes |
| Native in-process TTS | #54 | Closes |
| Native in-process STT | #55 | Closes |
| Dashboard service split, time-ranged costs, line charts, and global ROI | #15, #17, #66 | Closes |
| GUI configuration editor | #58 | Closes |
| Active-profile hot apply and stable fallback | #67 | Closes |
| Shared in-process runtime architecture | #52 | Closes |
| Cross-modality scheduling and cancellation | #59 | Closes |
| Embeddings and Responses APIs | #60, #61 | Closes |
| Model capability and residency discovery | #62 | Closes |
| Resource-aware residency and capacity planning | #56 | Partially addresses |
| Hardware-aware model store and external inventory | #57 | Partially addresses |
| Atomic sherpa-onnx bundle installation | #70 | Follow-up; remains open |
| In-process image runtime/API | #53 | Partially addresses |
| Safety-gated measured profile optimizer | #68 | Closes |
| Adaptive in-process MTP and generation-only TPS | #69 | Closes |
| Canonical OpenCode model inventory | #71 | Closes |
| Measured optimization provenance in Operate | #72 | Closes |

## Pull request summary

InferDeck now behaves as a headless home-lab AI control plane: it is reachable
across the LAN and Tailscale, retains SQL usage across restarts, separates LLM
and Dictation administration, supports native neural speech backends, and can
apply validated dashboard profiles without a manual machine/service restart.
The current follow-up completes hardware-aware model discovery, reconciles the
existing on-disk model library, corrects time-ranged economics and charting,
and adds a safety-gated quality-first profile analyser that stages explicit
recommendations without pretending estimates are measured benchmarks.

## Version notes

### v0.5.1

- Restored LAN/Tailscale dashboard and API access.
- Restored restart-persistent SQL usage accounting.
- Added LLM/Dictation/Home dashboard separation and global ROI reporting.
- Added native neural dictation runtime configuration and speech accounting.
- Added validated active profiles with stable-baseline recovery.
- Added automatic headless profile apply with no manual service or hardware
  restart.
- Expanded automated C++ and dashboard regression coverage.

### v0.5.2

- Ranked Hugging Face discovery by adoption with quality-oriented catalogue
  presets.
- Added recommendation, multimodal, and hardware-fit indicators.
- Reconciled active, managed, and externally downloaded models on disk.
- Made LLM and Dictation headline economics follow the selected time range.
- Added adaptive all-time chart resolution with complete totals.
- Replaced service usage bars with line charts and added one line per AI
  service to Home.
- Moved all ROI/break-even controls to Home.
- Added KV-cache precision and shared runtime-memory controls.
- Collapsed recovery and advanced Dictation diagnostics for safety.
- Prevented incomplete single-file sherpa-onnx installs from the catalogue.
- Deployed and verified the release on loopback, LAN, and Tailscale while
  retaining the existing SQL usage ledger.

### v0.5.3

- Added safety-gated, quality-first model profile analysis after the v0.5.2
  GUI baseline.
- Added scored context-per-slot, slot, KV precision, batch, parallelism, and
  VRAM-headroom candidates.
- Added review-and-stage workflow in Model Details with no automatic mutation.
- Added optimizer, route, API, and full-suite regression coverage.
- Kept measured quality benchmarking separate and truthfully labelled as a
  future maintenance-mode feature.

v0.5.3 was deployed and verified live on loopback, LAN, and Tailscale with the
existing SQL usage ledger retained.

### v0.6.0

- Replaced the estimate-only Auto-optimize action with an asynchronous,
  maintenance-mode mini benchmark that actually loads and runs the selected
  llama.cpp model.
- Added fixed-seed arithmetic, ordering-logic, and long-record retrieval
  quality probes, a dedicated 128-word sustained single-slot generation, and
  four concurrent 96-word throughput generations.
- Measures each candidate's model load time, average output tokens per second,
  aggregate parallel tokens per second, average time to first token, actual
  peak VRAM, and quality result.
- Scores measured candidates with a quality-first balance:
  60 percent measured quality, 15 percent sequential speed, 15 percent
  concurrent throughput, and 10 percent VRAM headroom.
- Requires a measured 12 percent VRAM reserve before a candidate can win.
- Blocks new LLM requests, swaps, manual model changes, and profile changes
  during the benchmark so the results are isolated.
- Added live progress, cancellation, per-candidate measured results, and
  explicit review-and-save controls to Model Details.
- Restores the exact previously resident model set after success, cancellation,
  or failure. Benchmark probes bypass usage accounting and therefore do not
  contaminate normal request, token, cost, or ROI statistics.
- Preserves the original estimator only as the bounded candidate generator;
  the final recommendation is selected exclusively from real model runs.
- An early real isolated functional smoke against the installed
  Qwen2.5-Coder-3B Q4_K_M GGUF completed before the dedicated sustained-speed
  probe was added:
  - quality: two of three probes, 66.7 percent;
  - short-answer end-to-end output rate: 61.03 tokens per second;
  - short-answer parallel rate: 56.95 tokens per second;
  - average first-token latency: 60.74 ms;
  - load time: 2,117.34 ms;
  - actual peak VRAM: 3,651.15 MB;
  - arithmetic returned 704 instead of 714 while the logic and retrieval
    probes returned Cara and saffron correctly, proving the measured quality
    gate can expose a failure that configuration heuristics cannot;
  - these two early speed figures are retained as execution evidence only and
    are not comparable to the corrected sustained-throughput measurement.
- A complete three-candidate run against the installed 16.82 GB
  Qwen3.6-27B Q4_K_M GGUF and the AMD Radeon AI PRO R9700 selected:
  - 100,000 context tokens per slot;
  - four slots;
  - Q4/Q4 KV cache;
  - 2,048/2,048 batch and micro-batch sizes;
  - three of three fixed quality probes passed;
  - sustained single-slot output: 30.42 tokens per second;
  - four-slot aggregate throughput: 48.70 tokens per second;
  - average time to first token: 355.59 ms;
  - model load time: 7,660.70 ms;
  - actual peak VRAM: 27,357.95 MB;
  - actual reserve: 4,663.71 MB;
  - measured overall score: 95.78 percent.
- The competing Q4/Q8 profile was marginally faster at 30.52 output tokens
  per second but peaked at 30,492.53 MB. Q8/Q8 peaked at 28,813.48 MB. Both
  were rejected because they left less than the required 12 percent VRAM
  reserve.
- A follow-up stock `llama-bench` run isolated gateway/request overhead for the
  same installed Qwen3.6-27B Q4_K_M GGUF:
  - llama.cpp b9276, Vulkan, AMD Radeon AI PRO R9700;
  - Q4/Q4 KV cache, flash attention on, all 27B layers on the GPU;
  - 2,048/2,048 batch and micro-batch;
  - five 256-token generation samples;
  - sustained result: 32.62 +/- 0.04 tokens per second;
  - 64-token prompt processing: 526.96 +/- 2.99 tokens per second.
- The 32.62 tok/s stock result versus InferDeck's 30.42 tok/s end-to-end
  result shows that ordinary one-token decoding has only about seven percent
  headroom left. A 40-50 tok/s target therefore requires speculative/MTP
  decoding or a materially smaller/lower-quality quant, not another batch-size
  adjustment.
- The installed standalone llama.cpp b9276 runtime advertises native
  `draft-mtp` support. The currently installed non-MTP GGUF does not include
  the required next-token prediction layer, and InferDeck's custom
  continuous-batch scheduler does not yet expose llama.cpp speculative
  decoding. The MTP-specific Q4_K_M replacement is approximately 17.5 GB and
  must be benchmarked for real acceptance rate, long-session correctness, and
  its additional draft-context VRAM before it can be recommended.
- The first 27B pass revealed that short quality answers made first-token
  latency dominate the apparent speed. The speed path was corrected to use
  dedicated sustained generations, the candidate notes were switched from
  estimated to actual VRAM reserve, and the complete 27B benchmark was rerun.
- The measured runner used a temporary database and a localhost-only sidecar
  on port 11435. It restored an empty prior residency, was stopped after the
  result was collected, and left the live v0.5.3 service idle and healthy with
  its production ledger unchanged at 6,949 requests and 262,846,499 tokens.
- Validation completed before live cutover:
  - four focused optimizer/benchmark route cases passed with 44 assertions;
  - all 32 dashboard tests passed;
  - the production dashboard bundle built successfully;
  - all 113 C++ unit and integration tests passed under normal Windows
    filesystem permissions.
- The required in-app browser validation was attempted with the bundled
  browser client. Its Node runtime rejected initialization before a browser
  session could be created with:
  `codex/sandbox-state-meta: missing field sandboxPolicy`.
  No visual-browser pass is claimed; the dashboard result is supported by the
  32 automated tests and successful production bundle instead.
- A guarded v0.6.0 live deployment helper was prepared at
  `C:\tmp\deploy-inferdeck-v060.ps1`, with automatic binary/static backup,
  hash verification, health probing, and rollback.
- Live cutover preflight was clean: zero active requests, zero queued requests,
  no swap, no resident model, approximately 1.77 percent GPU utilization,
  874 MB used of 32,022 MB VRAM, and the production SQL ledger intact.
- The managed command session cannot stop the LocalSystem NSSM service:
  Windows returned `OpenService(): Access is denied`. Direct and Explorer-shell
  elevation launchers did not produce an elevated helper result in this
  session. No UAC boundary was bypassed.
- The failed non-elevated attempt stopped before replacing any live artifact.
  It left a recoverable snapshot at
  `C:\InferDeck\backups\v060-precutover-20260726-112743`.
- Post-attempt verification confirmed that production remains deliberately
  unchanged on v0.5.3: the service is running and healthy, the database remains
  `C:/InferDeck/data/stats.db`, lifetime usage remains 6,949 requests and
  262,846,499 tokens, the GPU returned to approximately 874 MB idle use, and no
  model is resident.

### 2026-07-26 direct v0.5.3 cutover audit

- Executed `C:\tmp\deploy-inferdeck-v053.ps1` directly through the managed
  elevated-command path after confirming zero active requests, zero queued
  requests, no swap, and an idle resident model.
- Windows Service Control Manager rejected both NSSM stop requests with
  `OpenService(): Access is denied`; the command session has a medium-integrity
  token and the LocalSystem service grants stop/start rights only to SYSTEM and
  Administrators.
- The guarded helper created
  `C:\InferDeck\backups\v053-precutover-20260726-103300`, reported
  `status=failed`, and completed its rollback path.
- Post-attempt verification confirmed:
  - the `InferDeck` NSSM service is still running;
  - the live executable and dashboard hashes are unchanged from v0.5.2;
  - `/v1/health` is healthy;
  - the persistent database remains `C:/InferDeck/data/stats.db`;
  - the lifetime request count remains available at 6,949.
- No partial v0.5.3 installation was left live and no security boundary was
  bypassed.
- A subsequent interactive elevation was accepted. The same guarded helper
  completed successfully, deployed the matching v0.5.3 gateway and dashboard,
  and passed the complete post-cutover verification recorded above.

### 2026-07-26 Qwen3.6-27B MTP download and measured benchmark

- Resolved the required speculative-decoding artifact to the public
  `unsloth/Qwen3.6-27B-MTP-GGUF` repository rather than substituting another
  model or quant.
- The live C: volume had only 1.51 GB free, so the model was downloaded
  resumably to the NVMe-backed E: model store. The existing non-MTP model in
  `C:\InferDeck\models` was not modified.
- Final artifact:
  - path:
    `E:\InferDeck\models\unsloth\Qwen3.6-27B-MTP-GGUF\Qwen3.6-27B-Q4_K_M.gguf`;
  - exact size: 17,106,773,120 bytes;
  - SHA-256:
    `A7CBD3ECC0E3F9B333EDEE61AE66BC87ED713C5D49587A8355814722ED329E0F`;
  - the completed file was promoted from its partial-download name only after
    both size and SHA-256 matched the upstream Hugging Face metadata.
- Benchmarked the downloaded model with the installed llama.cpp b9276 Vulkan
  runtime on the AMD Radeon AI PRO R9700 through an isolated localhost sidecar
  on port 11435. The live InferDeck gateway remained on port 11434, idle, and
  its production SQL ledger remained unchanged at 6,949 requests and
  262,846,499 tokens.
- Corrected an exploratory benchmark before accepting its results:
  `--kv-unified` changes the relationship between total and per-sequence
  context. The authoritative matrix therefore used separated KV geometry with
  400,000 total context tokens, four slots, and 100,000 context tokens per
  slot.
- All authoritative profiles used Q4/Q4 target KV, flash attention, all model
  layers on the GPU, a 2,048-token batch, fixed seed 42, real model inference,
  three exact-answer quality probes, two sustained single-request workloads,
  and four concurrent prose generations.
- Best safe MTP profile:
  - speculative method: `draft-mtp`;
  - draft depth: two tokens;
  - draft probability floor: zero;
  - micro-batch: 512;
  - quality: three of three probes passed;
  - repeated deterministic workload: 41.40 output tokens per second;
  - technical-prose workload: 47.71 output tokens per second;
  - sustained single-request average: 44.56 output tokens per second;
  - four-request aggregate while MTP remained enabled: 46.69 output tokens
    per second;
  - draft acceptance: 41.15 percent on the repeat workload, 55.37 percent on
    prose, and 51.99 percent across the parallel workload;
  - model load time: 8,702.19 ms;
  - actual peak VRAM: 27,354.36 MB;
  - actual reserve: 4,667.34 MB, or 14.58 percent.
- The 512-token micro-batch is material: the same depth-two MTP profile with a
  2,048-token micro-batch peaked at 30,765.82 MB and left only 3.92 percent
  reserve. It was rejected despite reaching a similar 43.97 single-request
  average.
- Draft depth one averaged 41.98 output tokens per second for a single request,
  only 42.53 aggregate under four-way load, and left 5.80 percent reserve.
  Draft depth three was slower than depth two and left still less headroom.
- Matching non-speculative control with the same downloaded MTP GGUF and
  512-token micro-batch:
  - quality: three of three probes passed;
  - sustained single-request average: 32.41 output tokens per second;
  - four-request aggregate: 79.42 output tokens per second;
  - model load time: 8,152.85 ms;
  - actual peak VRAM: 25,046.34 MB;
  - actual reserve: 6,975.36 MB, or 21.78 percent.
- The measured result supports a concurrency-aware implementation:
  depth-two MTP is beneficial for one active request, while ordinary
  continuous batching is substantially faster once multiple requests are
  active. InferDeck does not yet expose llama.cpp speculative decoding, and
  the installed b9276 server compiles out per-request speculative-parameter
  changes, so a dynamic in-process switch has not yet been implemented or
  claimed as measured.
- No live profile, service binary, YAML, model registry, or resident production
  model was changed by this download and benchmark. All benchmark
  `llama-server.exe` processes were stopped after their exact runs.

### 2026-07-26 native adaptive MTP inside InferDeck

- Added MTP directly to InferDeck's in-process llama.cpp wrapper. The gateway
  does not start, proxy, or depend on `llama-server.exe`.
- Extended per-model configuration and validation with:
  - `n_batch` and `n_ubatch`;
  - target KV cache types;
  - speculative type `mtp`;
  - draft depth;
  - draft probability floor;
  - maximum active requests allowed to use MTP.
- Registered the verified Qwen3.6-27B MTP artifact under the existing
  `qwen3.6-27b` API model ID so clients do not need to change model names.
- Implemented the measured concurrency policy:
  - one active request uses depth-two MTP;
  - two or more active requests use ordinary continuous batching;
  - target hidden-state extraction and all draft-context decoding are disabled
    in the multi-request path;
  - a request that enters the multi-request path stays non-speculative until it
    finishes;
  - the next single request clears and rebuilds the paired target/draft cache
    before MTP resumes.
- Added target/draft micro-batch separation. The selected target context uses a
  2,048-token micro-batch for four-slot throughput while the MTP draft context
  is capped at 512 to avoid allocating a second oversized compute graph.
- Corrected InferDeck TPS accounting:
  - total request duration remains the latency figure;
  - tokens per second now uses measured output-generation duration and excludes
    prompt-prefill time;
  - the automatic benchmark uses the same generation-only definition for
    single and aggregate parallel throughput;
  - request events and gateway logs expose generation duration separately.
- Updated the dashboard model-details editor with per-model KV, batch, and
  adaptive-MTP controls. Benchmark results now explicitly label single and
  parallel generation throughput.
- Updated the automatic optimiser's measured VRAM gate from a percentage to a
  2,048 MB minimum reserve. This permits the measured 27B four-slot
  performance profile while retaining approximately 2.23 GB of usable VRAM
  headroom on the R9700.
- Selected repository profile:
  - four slots, 100,000 context tokens per slot;
  - Q4/Q4 KV cache;
  - target batch/micro-batch 2,048/2,048;
  - MTP draft context micro-batch capped internally at 512;
  - draft depth two, probability floor zero;
  - MTP active-request limit one;
  - measured VRAM requirement 29,791 MB.
- Authoritative isolated InferDeck benchmark on port 11435:
  - real in-process gateway runtime and HTTP API;
  - quality: three of three exact-answer probes passed;
  - sustained single generation probes: 39.36 and 42.06 tokens per second,
    averaging 40.71 tokens per second;
  - four-slot aggregate generation throughput: approximately 51.45 tokens per
    second;
  - four-slot aggregate end-to-end throughput including prefill and HTTP:
    47.55 tokens per second;
  - peak VRAM: 29,790.39 MB;
  - model load time: 8,603.27 ms.
- The four-slot-to-single transition was tested in the same process:
  - InferDeck logged `scheduler_mtp_resync`;
  - the following 128-token single request reached 62.71 generation tokens per
    second and 55.53 end-to-end tokens per second;
  - 86 of 86 drafted tokens were accepted;
  - no stale-cache or quality failure occurred.
- Rejected measured alternatives:
  - target micro-batch 1,024 used 28,571.86 MB but reached only 35.87
    end-to-end aggregate tokens per second;
  - target micro-batch 1,536 used 29,067.62 MB but reached only 29.23
    end-to-end aggregate tokens per second;
  - giving both target and draft contexts a 2,048 micro-batch used
    30,754 MB, so the split context configuration is materially safer.
- Added permanent regression coverage for:
  - repository MTP profile parsing;
  - invalid MTP depth and concurrency windows;
  - invalid per-model batch geometry;
  - persistent low-concurrency MTP eligibility;
  - generation-only TPS calculation.
- Validation:
  - complete Release build succeeded;
  - all 115 C++ unit and integration tests passed;
  - all 32 dashboard tests passed;
  - TypeScript checking and the production dashboard build passed;
  - isolated benchmark requests used only the temporary benchmark database and
    did not alter the production token ledger.
- This section records checkout and isolated-runtime completion. Live
  `C:\InferDeck` deployment is recorded separately after guarded service
  cutover and live health/performance verification.

### 2026-07-26 adaptive MTP live cutover and production verification

- Performed an idle-service cutover of the matching Release gateway,
  production dashboard bundle, and base YAML into `C:\InferDeck`.
- The guarded deployment:
  - required zero active requests, zero queued requests, no swap, and no
    resident model;
  - backed up the previous executable, dashboard, and base YAML under
    `C:\InferDeck\backups\v060-precutover-20260726-142432`;
  - verified the deployed executable and YAML by SHA-256;
  - restarted the LocalSystem NSSM service through Windows elevation;
  - required a successful `/v1/health` response and would have restored all
    three backed-up artifacts on failure.
- Deployed hashes:
  - `inferdeck-gateway.exe`:
    `5C03AC4EB1EAE457C9313EF97F47CC350C849924522EA594C5C9F68522D1247C`;
  - `config/gateway.yml`:
    `8E9A5E32022942BBCFE5318C2507D3CEE1324AA434E13721C7DE76085D7B9C47`.
- Preserved the existing active profile instead of replacing it with the base
  YAML. Merged only the Qwen3.6-27B MTP artifact, four-slot minimum, Q4/Q4 KV,
  batch geometry, speculative controls, and measured VRAM requirement. Other
  live overrides, including slot-count differences for other models, remain
  intact.
- Saving the active profile triggered InferDeck's automatic headless reload.
  No hardware restart or manual service restart was required for the profile
  change. Active and running revision both became
  `c2383d0165e31267`.
- Production data continuity before performance verification:
  - database: `C:/InferDeck/data/stats.db`;
  - lifetime requests: 6,949;
  - lifetime tokens: 262,846,499.
- InferDeck's live measured-optimisation endpoint completed a real one-candidate
  trial:
  - quality: three of three probes passed;
  - selected trial: 32,768 context per slot, four slots, Q4/Q8 KV;
  - single generation throughput: 38.08 tokens per second;
  - four-request aggregate generation throughput: 51.52 tokens per second;
  - actual peak VRAM: 25,166.25 MB;
  - the trial restored the previous empty residency and did not write
    artificial benchmark traffic into the production usage ledger.
- The measured optimiser trial above was not applied because it reduced the
  explicit 100,000-token-per-slot requirement. The active profile remained
  four slots with 100,000 context tokens per slot and Q4/Q4 KV.
- Exact active-profile verification through the normal InferDeck
  `/v1/chat/completions` API:
  - gateway load logs confirmed 400,000 shared context tokens, four slots,
    100,000 context tokens per slot, target batch/micro-batch 2,048/2,048,
    Q4/Q4 KV, MTP depth two, and a one-active-request MTP limit;
  - a 256-token single request reached 50.16 generation tokens per second and
    43.55 end-to-end tokens per second;
  - it accepted 143 of 224 drafted tokens, or 63.84 percent;
  - four simultaneous 256-token requests completed 1,024 output tokens at
    51.24 aggregate end-to-end tokens per second;
  - the next 128-token single request logged `scheduler_mtp_resync`, reached
    57.15 generation tokens per second and 51.04 end-to-end tokens per second,
    and accepted 78 of 98 drafted tokens.
- The exact live results meet the requested performance envelope directly
  through InferDeck: approximately 40 to 50+ tokens per second for a single
  request and 50 to 60 aggregate tokens per second across four concurrent
  requests.
- All six normal API verification requests were intentionally retained in the
  production ledger:
  - lifetime requests increased exactly from 6,949 to 6,955;
  - lifetime prompt tokens increased by 196;
  - lifetime completion tokens increased by 1,408;
  - lifetime tokens increased exactly from 262,846,499 to 262,848,103.
- Unloaded Qwen3.6-27B after verification. Final service state:
  - healthy;
  - zero running and zero queued requests;
  - no resident model;
  - 30,997 MB available from the 32,021 MB InferDeck VRAM budget;
  - active and running YAML revisions match;
  - production SQL database remains `C:/InferDeck/data/stats.db`.

### 2026-07-26 main-integration review and OpenCode model inventory

- Audited the complete `main...alpha-v2` history before publication:
  - `main` and `origin/main` are at the v0.4.0 merge commit `798b2ef`;
  - `main` is the merge base and `alpha-v2` is not behind it, so no synthetic
    merge-from-main commit is required;
  - the connected GitHub repository had no existing open pull request for
    `alpha-v2`;
  - local `alpha-v2` contained ten additional commits beyond the previously
    published `origin/alpha-v2`.
- Reviewed 155 changed paths covering the runtime registry, resource-aware
  scheduling, Responses/embeddings/media APIs, model store, active
  configuration editor, headless dashboard, measured profile optimiser, and
  adaptive MTP runtime.
- Corrected merge-readiness findings:
  - removed trailing whitespace caught by `git diff --check`;
  - added the v0.6.0 changelog covering the complete alpha scope and live
    verification;
  - aligned `inferdeck-bench --version` with gateway/dashboard v0.6.0;
  - replaced the obsolete agent rule that prohibited MTP with the implemented
    adaptive single-request/concurrent-request invariant;
  - corrected the project OpenCode catalog from 40,960 to 100,000 context
    tokens for `qwen3.6-27b`;
  - added the installed `qwen3-coder-next` 262,144-context model to the project
    OpenCode catalog;
  - normalized project OpenCode model keys and defaults to InferDeck's
    canonical bare IDs, removing duplicate `:latest` entries from the merged
    `opencode models inferdeck` catalog.
- Public-tree secret-pattern review found only the deliberately fake
  `sk-inferdeck-local-dev-token-do-not-use-in-prod` fixture already used by
  authentication tests; no real private key or provider token was found in the
  alpha delta.
- Merge gate:
  - complete Release build passed;
  - all 115 C++ unit/integration tests passed;
  - all 32 dashboard tests passed;
  - TypeScript checking and production Vite build passed;
  - `inferdeck-bench --version` returned `0.6.0`;
  - `git diff --check` passed after the whitespace correction.
- Environment-only retries recorded for reproducibility:
  - the first MSBuild invocation inherited duplicate Windows `Path` and `PATH`
    keys and failed with `MSB6001`; the normalized child environment build
    passed;
  - the sandbox denied weak canonicalization of the foundation test's own
    temporary directory and Vite workspace traversal; the identical tests
    passed outside that filesystem sandbox.
- OpenCode inventory:
  - installed OpenCode version: 1.18.3;
  - provider: `inferdeck` through `@ai-sdk/openai-compatible`;
  - LAN API base: `http://192.168.0.168:11434/v1`;
  - global default and small model are both `inferdeck/qwen3.6-27b`, avoiding a
    model swap for lightweight title work when using the dense 27B profile;
  - the InferDeck project config overrides those defaults with
    `qwen3-coder-30b-a3b` as primary and `qwen2.5-coder-3b` as small model;
  - automatic compaction and pruning are enabled with a 16,000-token reserve.
- Live model-selection facts captured for OpenCode optimisation:
  - `qwen3-coder-30b-a3b`: 262,144 context, 20,000 MB admission estimate, one
    slot; best balanced primary coding model and able to co-reside with the
    2,500 MB small coder;
  - `qwen3.6-27b`: four 100,000-token slots, 29,791 MB, adaptive MTP; measured
    quality-first dense option at 50.16 single generation tokens/s and 51.24
    aggregate end-to-end tokens/s;
  - `qwen3.6-35b-a3b`: four live slots, 100,096 context, 24,000 MB; the
    historically dominant general/agent model by local usage;
  - `qwen3-coder-next`: 262,144 context, 29,000 MB, one slot; high-context
    coding option but too close to the VRAM ceiling to pair safely with another
    substantial model without fresh admission testing;
  - `qwen2.5-coder-3b`: 32,768 context, 2,500 MB, four live slots; intended for
    titles, summaries, and cheap helper tasks when it can co-reside;
  - `gpt-oss-20b`: 32,768 context, 13,000 MB, eight slots; fast parallel helper
    option, but its shorter context makes it unsuitable as the only long-agent
    primary;
  - `gemma-4-26b-a4b`: four 100,000-token slots, 13,000 MB; efficient
    general/review alternative, currently advertised as text-only;
  - `gemma-4-31b`: 65,536 context, 29,000 MB, one slot and only one historical
    production request; not recommended as a default without more quality and
    performance evidence.
- Published the reviewed local history to `origin/alpha-v2` and opened draft
  pull request
  `https://github.com/davidtaylor6130/InferDeck/pull/63` against `main`.
  GitHub reports the PR conflict-free and mergeable. The repository did not
  attach a pull-request workflow run to the head commit, so the complete local
  build/test gate above is the recorded verification evidence.

### 2026-07-26 measured optimization provenance and GitHub issue audit

- Added optional per-model optimization provenance to the profile data:
  measured state, measurement date, quality pass count, single-request
  generation throughput, and parallel throughput.
- Marked the Qwen3.6-27B profile with the already completed live measurements:
  three of three quality probes, 50.16 single-request generation tokens per
  second, and 51.24 four-request aggregate end-to-end tokens per second.
- Carried the metadata through model discovery and dashboard status without a
  model-name conditional.
- The LLM Operate page now:
  - shows a `Measured optimized` badge when profile data reports a measured
    optimization;
  - uses a green `Auto-optimize` button for measured profiles;
  - keeps the normal blue action for profiles without measured provenance;
  - removes `with benchmark` from the button label while retaining the detailed
    measured-benchmark explanation inside the dialog.
- Added configuration and dashboard regression coverage for the data-driven
  marker and button state.
- Validation:
  - 32 of 32 dashboard tests passed;
  - dashboard TypeScript checking and production Vite build passed;
  - configuration tests passed with 70 assertions across nine test cases;
  - gateway route tests passed with 743 assertions across 66 test cases;
  - the affected Release gateway, configuration tests, and route tests rebuilt
    successfully after normalizing the duplicate Windows `Path`/`PATH` child
    build environment.
- Completed a full GitHub issue audit for this work log:
  - reused existing #15, #17, and #52 through #62 where their scope matched;
  - created #64 through #72 for previously untracked workstreams;
  - reserved #70 as the explicit uncompleted sherpa-onnx bundle follow-up;
  - prepared PR #63 to distinguish issues it closes from work it only partially
    addresses or leaves as a follow-up.
- A final integration review caught and fixed the dashboard API normalizer
  dropping optimization metadata from `/v1/models`. The API regression now
  proves the complete measured record reaches `ModelInfo`, not only the
  gateway JSON or a fixture rendered directly by the page test.
- Pushed the final optimization commits to `origin/alpha-v2` and updated PR #63
  with explicit closing links for the completed issues, partial links for
  #53/#56/#57, and #70 as the unclosed follow-up.
- Deployed the matching Release gateway and dashboard to live
  `C:\InferDeck` through the idle-service rollback guard:
  - the first managed command remained medium-integrity and Windows rejected
    service control before replacing any artifact;
  - the normal Windows UAC path then completed successfully;
  - rollback snapshot:
    `C:\InferDeck\backups\v060-precutover-20260726-162800`;
  - deployed gateway SHA-256:
    `AA4B491ACE8AFCB8E4CBEF960F0DD413532EBB577F8DF7144DFB25D867D6937C`;
  - the deployed and Release-build hashes match.
- Updated only the live Qwen3.6-27B optimization data through InferDeck's own
  active-profile API. The automatic headless reload completed with matching
  active and running revision `3eee3284b566a8db`.
- Live discovery now reports:
  - optimization status `measured`;
  - measurement date `2026-07-26`;
  - quality three of three;
  - single throughput 50.16 tokens per second;
  - parallel throughput 51.24 tokens per second.
- Live dashboard asset verification returned HTTP 200 and confirmed:
  - `Measured optimized` is present;
  - `Auto-optimize` is present;
  - the obsolete `Auto-optimize with benchmark` label is absent;
  - the green success style is included.
- The in-app browser controller could not initialize because its managed
  sandbox metadata was unavailable. This was an environment-only inspection
  limitation; the live API, served index, exact production asset, automated
  render test, and production build all passed.
- Final live state remained healthy and idle with zero running requests, zero
  queued requests, no swap, no resident model, 6,986 persisted requests, and
  the same `C:/InferDeck/data/stats.db` production ledger.
