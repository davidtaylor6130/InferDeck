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

- Live v0.5.3 cutover and post-cutover proof for the profile analyser. The
  release is built and staged, but Windows UAC has not launched the elevated
  deployment helper yet.
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
  `index-HVe9AxzV.js` and `index-D68mq8yi.css`.
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
- No workaround was used to bypass Windows service permissions. Accepting the
  UAC prompt is the only remaining cutover action.

## GitHub issue candidates

- Preserve LAN-wide binding as an explicit home-lab compatibility contract.
- Make statistics storage path stable across Windows service accounts and
  restarts.
- Add native neural STT/TTS runtimes and OpenAI-compatible audio accounting.
- Split dashboard navigation by AI service and responsibility.
- Add safe active-profile hot apply with stable-baseline fallback.
- Make model catalogue recommendations adoption- and hardware-aware.
- Reconcile externally downloaded model libraries in the Models UI.
- Add an atomic multi-artifact sherpa-onnx repository bundle installer.
- Make all usage and cost summaries respect the selected time range.
- Keep portfolio ROI global and standardize line charts across AI services.
- Add a safety-gated background model profile optimizer.
- Design an explicit maintenance-mode measured benchmark that persists across
  reload-only trials and restores the original active profile on failure.

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

Deployment and live verification are pending.
The live v0.5.2 service remains healthy until the v0.5.3 UAC prompt is accepted.
