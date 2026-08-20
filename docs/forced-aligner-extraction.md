# Forced-aligner extraction decision and inventory

- Status: Inventory and history preservation complete; destructive gate not granted
- Date: 2026-08-20
- Overhaul phase: 1
- Tracking: #109
- Core baseline: `d1760c7f9fcad0940faa66944c75294a69b2fd39`
- Feature version: `1.0.11`

## Decision

The feature will leave InferDeck Core and become **Qwen Alignment Companion**,
owned by `@davidtaylor6130` and released independently from InferDeck.

The companion is not an OpenAI API implementation. Its canonical endpoint will
be a product-specific `POST /alignments`; the current
`POST /v1/audio/alignments` path is a legacy migration surface and must not be
described as OpenAI-compatible. InferDeck will not install, launch, supervise,
proxy, authenticate, schedule, observe, or release the companion.

The companion defaults off, has its own repository or archive, semantic version,
dependency lock, security policy, service definition, client migration guide,
and release artifacts. Its failure or absence cannot change InferDeck startup,
health, model residency, or OpenAI endpoint behavior.

## Tracked source inventory

The feature currently owns 21 tracked files. SHA-256 values describe the Phase 1
inventory snapshot and allow a later archive to be checked before extraction.

| Path | Purpose | SHA-256 |
|---|---|---|
| `apps/forced-aligner/.gitignore` | Python build exclusions | `DBADE045E06CA2D822A0BB78A6C20FFC789468528B6F03ACE29BB97C987904A2` |
| `apps/forced-aligner/Dockerfile` | ROCm container and port 11436 | `97B31431577E27C6A55DE31B047226DC209A4740BB5356F4688CA851468035D7` |
| `apps/forced-aligner/README.md` | Sidecar API and deployment guide | `922D022FDD637412B1EDDE435D00D5B67FB63F000C0984D3877337FA6B1C0774` |
| `apps/forced-aligner/pyproject.toml` | Python package and entry point | `9976A04B9C8DD57C88947B141D669E3F6EC22C8E866E01EDEE6D6008EB82979B` |
| `apps/forced-aligner/requirements.lock` | Python, PyTorch, and ROCm dependency lock | `CF632CA161DF5721FB4CF8917BE8A99C43E4D9DFF6AAB34FBC3C9B8864F7B709` |
| `apps/forced-aligner/src/inferdeck_forced_aligner/__init__.py` | Package version | `5ABAF4212DF43F25DCD2E0CEF8DA2B25A97B6756BDFE28F38F172D64F7C22A3E` |
| `apps/forced-aligner/src/inferdeck_forced_aligner/app.py` | FastAPI routes, auth, and request lifecycle | `00AAA578EF50892C7934D7972DA3294144D2A7765884D9798BD6445B26E51197` |
| `apps/forced-aligner/src/inferdeck_forced_aligner/audio.py` | Upload handling and FFmpeg subprocess | `7E1F543CDBA39C4B4FB1BCB62935E7C53A48E3DBB9D5106256CEB38FE396DC52` |
| `apps/forced-aligner/src/inferdeck_forced_aligner/backend.py` | Qwen/PyTorch alignment runtime | `3FBA0E82C0F77ACE2068F6740AECBE75BE4842D36D723394FC20ECA9617A888A` |
| `apps/forced-aligner/src/inferdeck_forced_aligner/config.py` | Environment and token-file settings | `0945CB58CF2F0A807065E47594543F7C35E4F8FD165BEB34F9508F7252DB9EAF` |
| `apps/forced-aligner/src/inferdeck_forced_aligner/errors.py` | Companion error type | `AC6D301D3772B2E524CAD2904F432DAAC5032557B76F38C01879E3EC403F69E3` |
| `apps/forced-aligner/src/inferdeck_forced_aligner/main.py` | Uvicorn entry point | `888CD81F2087C378277C45ED4DECAD4DD66DF1267BC2DC8F4D8F974577FA534B` |
| `apps/forced-aligner/src/inferdeck_forced_aligner/words.py` | Word aggregation and validation | `D94D9790DDAAA97FCCC0813113C287A8CCA9562726FC47C25EF5C73FB8489F02` |
| `apps/forced-aligner/tests/test_service.py` | Python service tests | `745C9306872F84F886D352D27CDC727CB50CC954F4FC1160C6419A0E3E5379B1` |
| `config/services/forced-aligner.yml` | Sidecar listener, model, network, and supervision config | `D9B2791AEB24A64241F16F2FCDD7E947A0B1CAAE5F16F0DBE80FB403ED32CA72` |
| `scripts/windows/Enable-ForcedAlignerRemote.ps1` | Remote binding, install, firewall, and startup | `CE90461A693C295414C4C0127A1519D3BF3D05B9B11F062E0B458C39EF63ACC1` |
| `scripts/windows/Install-ForcedAlignerService.ps1` | NSSM service installation | `CE325840806E4DEA2D555EF5A3C5D34905DAE8633FE23CDE7E1259685C35C590` |
| `scripts/windows/Start-ForcedAligner-Watchdog.vbs` | Hidden watchdog launcher | `DD14B7A666A84D10FA31CE244808B2E379FDB1D575D736B2C024AF1BF20E99D2` |
| `scripts/windows/Start-ForcedAligner.ps1` | Python process launcher | `369C3E198A77A1AC42043F490985DA7DD0989F661FC2B1AC7EC4F793C0B51EF2` |
| `scripts/windows/Test-ForcedAlignerRealAudio.ps1` | Gateway-to-companion real-audio test | `CE7BBFE01C91B9CCC122BB162604161B699231870BFCC4819BF84AF893709C03` |
| `scripts/windows/Watch-ForcedAligner.ps1` | Per-minute process watchdog | `CB657A8BB7ADF204DEA807D255C7A67C5A737398FE3D1873501868D860B74FB3` |

Transitional references in `overhall_plan.md` and ADR 0001 describe the removal
decision and remain valid after extraction. `docs/v2-cleanup-report.md` contains
historical ROCm context and must be rewritten as history rather than current Core
capability during the documentation truth pass.

## Runtime and dependency inventory

The service is a Python 3.12 FastAPI/Uvicorn process. It loads
`Qwen/Qwen3-ForcedAligner-0.6B` at revision
`c7cbfc2048c462b0d63a45797104fc9db3ad62b7` through `qwen-asr`, PyTorch, and the
Windows ROCm 7.2.1 packages. Audio normalization calls the imageio FFmpeg binary
through `subprocess.run`.

The service owns these surfaces:

- unauthenticated `GET /health`;
- `OPTIONS /v1/audio/alignments`;
- bearer-authenticated multipart `POST /v1/audio/alignments`;
- listener `192.168.0.168:11436`;
- remote client assumption `192.168.0.172`;
- single-request `asyncio.Semaphore` admission;
- independent logs, PID files, model cache, firewall rule, service installer,
  launcher, and watchdog.

The locked environment contains 106 direct and transitive Python packages. The
container path uses `rocm/pytorch:rocm7.2.1_ubuntu24.04_py3.12_pytorch_release_2.9.1`
without an immutable image digest and is not part of the native Core build.

## Live snapshot

Read-only inspection on 2026-08-20 confirmed:

| Item | Evidence |
|---|---|
| Listener | `192.168.0.168:11436`, owned by Python PID 21516 |
| Health | `ok=true`, model loaded, device `rocm` |
| Wrapper | `inferdeck-forced-aligner.exe`, PID 22496 |
| Watchdog | PowerShell PID 22000 |
| Runtime tree | `C:\InferDeck\runtime\forced-aligner`, 56,345 files, 5,027 directories, 7,264,440,493 bytes |
| Model cache | pinned snapshot, 20 files, 1,840,072,459 bytes |
| Bearer token | `C:\InferDeck\config\forced-aligner-token.txt`, 43 bytes; content not read |
| Local sample | `C:\InferDeck\runtime\forced-aligner\sample.wav`, 173,736 bytes |
| Logs | `forced-aligner.log` and `forced-aligner-error.log` contain current operational history |
| Repository artifacts | seven untracked pytest token fixtures plus `.tmp-aligner-regression` audio and JSON results |
| Windows service | no `InferDeckForcedAligner` service or registry key installed |
| Scheduled tasks | no task action or name references the forced aligner |
| Firewall | enabled inbound TCP 11436 rule, local `192.168.0.168`, remote `192.168.0.172` |
| Token ACL | explicit read access for `SYSTEM` and `DEV-PC-16-CORE\david`; content not read |

The PID values are a point-in-time snapshot and are not migration identifiers.
An elevated read-only snapshot found no installed NSSM service and no scheduled
task for the companion. The active watchdog was therefore not proven reboot
persistent. The tracked NSSM installer and VBS launcher remain deployment
possibilities, not descriptions of current machine state. Repeat the privileged
snapshot immediately before live migration because this state can drift.

## Data classification and preservation

### Reproducible

- tracked application, tests, configuration, Dockerfile, and scripts;
- Python runtime and virtual environment rebuilt from the lock and companion
  release instructions;
- pinned Hugging Face model snapshot, subject to upstream availability and
  companion redistribution terms;
- PID and temporary request directories.

### Preserve outside Git

- the bearer token, followed by rotation when the companion moves;
- `sample.wav` until its provenance and reuse value are confirmed;
- service and error logs for a bounded diagnostic-retention period;
- `.tmp-aligner-regression` audio and JSON evidence until the owner classifies
  whether it contains user speech;
- the current firewall definition and evidence that no NSSM service or scheduled
  task was installed at the inventory snapshot;
- the editor-host configuration that currently points
  `AI_FORCED_ALIGNER_URL` at port 11436.

The untracked pytest token files contain test fixtures, not the live bearer token,
but remain user-owned worktree data and are not modified by this migration.

## History preservation

The complete feature history is the ordered commit chain:

1. `9c9d028d5d764ebfedaf6aafa9c39984a501079e` - initial service;
2. `3dd479582f5e3aace09a95ca23470b61f7be127a` - remote-access security;
3. `1af6b5e462daa93846f5c9bb090029a38a2cff8e` - safe reinstall behavior;
4. `d1760c7f9fcad0940faa66944c75294a69b2fd39` - long-audio hardening and version 1.0.11.

Annotated tag `forced-aligner-v1.0.11` is published at
`d1760c7f9fcad0940faa66944c75294a69b2fd39`. Its remote tag object is
`c877c4c74ba4624ca09376f12d236e7ab853be80`. A future companion repository
imports the tagged tree with history rather than copying only the latest files.
The tag target and this manifest must also be verified from a fresh clone before
the destructive gate is exercised.

## Ordered migration

1. Publish and verify the preservation tag.
2. From an elevated read-only session, export the exact service, task, firewall,
   and ACL state without exposing the bearer token.
3. Classify and preserve the local sample, logs, and regression audio/results.
4. Create the independent companion release source and change its canonical API
   from `/v1/audio/alignments` to `/alignments` with a documented compatibility
   window.
5. Migrate the editor client to the companion release and product-specific URL.
6. Obtain explicit owner approval naming every tracked path to remove.
7. Remove the 21 tracked feature files from Core and update Core documentation.
8. Add an architecture-policy test that rejects secondary inference listeners,
   Python services, subprocess/proxy launch paths, and feature watchdogs in Core.
9. Prove the native Core build and tests have no Python, PyTorch, ROCm, FFmpeg,
   FastAPI, Uvicorn, Docker, NSSM-sidecar, or watchdog dependency.
10. Obtain separate live-deployment approval before stopping or changing the
    currently running companion, its credentials, network rule, or boot state.

## Rollback

Source extraction rolls back from tag `forced-aligner-v1.0.11` and the manifest
above. Companion migration rolls back using its independently versioned release,
configuration export, encrypted credential backup, and client URL backup.

Core rollback never launches the companion. Live rollback must restore the
companion separately and must not reintroduce it into the InferDeck executable,
OpenAI route table, release artifacts, or watchdog contract.

## Approval gates

The inventory and preservation design are non-destructive. They do not authorize:

- moving or deleting the 21 tracked files;
- deleting or reading the live bearer token;
- deleting local audio, logs, caches, environments, or test artifacts;
- stopping PIDs 21516, 22496, or 22000;
- changing the listener, firewall, service, scheduled tasks, or editor client;
- publishing a new companion repository or deploying a companion release;
- changing the live InferDeck installation.

Each action requires the explicit gate identified by the overhaul plan.
