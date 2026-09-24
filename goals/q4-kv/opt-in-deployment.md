# Qwen3.8 Radiance Q4 opt-in candidate

Status: **prepared, not deployed**. The current production service remains `0.9.0-alpha-55` on its existing active profile. This is a local experimental candidate, not a public release.

## Candidate and evidence

The candidate package is `build/private-releases/alpha68-q4-active-optin/`. Its manifest pins the executable, active-profile YAML, embedded Python profile, INT4 overlay, both native DLLs and static assets by SHA-256. The executable reports `0.9.0-alpha-68 revision=2332e133f82a28c3b016afa16cc919e89e378f0c dirty=false`; executable SHA-256 is `77c8bab84495e82d70ac672383c0723439df602984b190f703e9b1eb131befd1`. The candidate active YAML SHA-256 is `f41e94924b2b56bd02d72fd0224fb8bb2aa16bd7595e0e40fac4c731f6ba68f5`.

Configure with `cmake -S . -B build -DINFERDECK_LOCAL_BUILD_NUMBER=68`, then build `inferdeck-gateway` in Release. `--version` and the repository's C++ unit/integration suite passed on the committed source: 148/148 tests. The Python profile suite previously passed 31/31. The alpha68 executable has **not** yet served a real model; alpha67, with the same runtime code and Python/native artifacts but a different build identity, supplied the real-app evidence in `progress.md`.

Alpha67's frozen two-agent 64K+2K tool-loop times were 137.930 and 136.871 seconds versus the BF16/R4D 133.684-second median. Its four simultaneous cold inputs each contained 100194 tokens and returned HTTP 200, but two answers reached the explicitly requested 256-token output cap. A separate 100K tool continuation completed normally with exact `read_file` and `BUILD_TAG` checks, 100320 cached tokens, and 99.074 seconds from first send to final answer. These results support an opt-in trial, not a claim of higher raw PP or complete long-term stability.

## Configuration boundary

Production currently uses `C:\InferDeck\config\gateway.active.yml`, **not** the base `gateway.yml`. The active profile has a different default model, aliases and model set. The package's `config/gateway.active.optin.yml` preserves every existing active-profile byte and inserts one model before `model_aliases:`. YAML structural comparison confirms only one additional registry entry. Its new ID is `qwen3.8-27b-radiance-q4`, with four fixed 106496-token slots, `r4d_int4`, `int4_per_token_head` KV and text-only capability. The existing one-slot BF16 `qwen3.8-27b-radiance`, default, aliases and Vulkan `qwen3.8-27b` with `mmproj_path`/vision remain unchanged. Select Q4 explicitly by its new ID.

The first package built from `gateway.yml` was invalidated as `INVALIDATED_BASE_CONFIG_NOT_ACTIVE`. Do not deploy `build/private-releases/alpha68-q4-optin/` or `alpha67-prefill2048/deployment/gateway.optin.yml`.

Run `powershell.exe -NoProfile -ExecutionPolicy Bypass -File .UAM/AINotes/Deploy-Alpha68Q4OptIn.ps1` for the **read-only** preflight. It passed as `PREFLIGHT_OK alpha68-q4-optin-20260924T022938735Z`, with evidence in `.UAM/AINotes/alpha68-q4-optin-20260924T022938735Z.json`; live hashes and the original loaded model remained unchanged. Activation is a separate `-Execute -RootReviewed` mode after production authorization and final review.

## Activation and rollback

Before activation, confirm the current live active/base config and executable hashes still match `manifest.json`, the `InferDeck` NSSM target is `C:\InferDeck\inferdeck-gateway.exe -c config\gateway.yml`, the active profile is in use, the service has no active/queued requests, and every package artifact matches its manifest. Back up the live executable, active config, Python profile and any files at the new overlay/DLL destinations under `C:\InferDeck\backups\` before stopping the service. Keep the base config untouched. The package's static assets and all 24 root DLLs matched production by SHA-256 when prepared; verify this again and leave them unchanged if they still match.

After stopping only `InferDeck`, copy the alpha68 executable, candidate active YAML, Python profile, INT4 overlay and two hash-pinned DLLs to their `C:\InferDeck` destinations. Start the service and verify the reported alpha68 revision, active profile, model inventory and a real Q4 chat request. Swap back to the pre-deployment model and confirm it serves. If any step fails, stop the service, restore the backed-up executable/active YAML/Python profile, restart, and verify the original model. Retain the backup and raw validation record. Do not change existing aliases or switch the default to Q4.

The local deploy helper backs up the six touched destinations and starts an independent hidden watchdog before stopping the service. Adversarial review found and we fixed a hash-check race, a watchdog deadline shorter than the longest validation path, a missed stop-timeout rollback, unbounded rollback paths, and a watchdog activation-before-start case. The reviewed helper now checks live and backup hashes again immediately before activation, restricts rollback to the six destinations, and uses parent-process monitoring plus an 1800-second ceiling. These safeguards have passed parser validation and read-only preflight; live rollback has not been exercised on alpha68.

The candidate still references the existing local Python/ROCm/vLLM/Radiance environment and model artifact under `build/runtime-radiance-probe`; those large dependencies are **not** bundled. Do not clear or move that tree while Radiance is configured. LocalSystem access and the final alpha68 service-switch path remain to be validated live. Upstream source authorship/licensing for the native INT4 copy remains unresolved, so do not publish this package or source as a release.

The original goal's repeated 20-cycle/100-mixed-request reliability gates and complete four-agent 100K answers are still open. Keep the candidate opt-in and the proven BF16/Vulkan paths available while those are assessed.
