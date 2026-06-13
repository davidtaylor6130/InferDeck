# InferDeck — Deployment Guide

InferDeck deploys as a **user-level Windows scheduled task**, not a service:
the gateway needs a logged-on session for GPU access, and the host doubles as
a desktop machine. The deployed copy lives outside the repo (default
`C:\InferDeck`) so rebuilds never touch the running install.

```
C:\InferDeck\
├── bin\              gateway exe + llama.cpp DLLs (copied from build\bin\Release\)
├── config\           gateway.yml (paths rewritten to absolute)
├── logs\             startup-task.out.log / .err.log, install results
├── run\              startup lock file
└── Start-InferDeck.ps1
```

## Install / update

```powershell
# Build first (see README), then:
powershell -ExecutionPolicy Bypass -File ops\Install-InferDeck-LogonStartup.ps1
```

The install script:

1. Copies `build\bin\Release\*` to `C:\InferDeck\bin\`, the startup script,
   and `config\gateway.yml` (rewriting `~/.inferdeck/` paths to absolute).
2. Registers the **"InferDeck Gateway Logon"** scheduled task (at-logon
   trigger, no execution time limit, auto-restart ×3).
3. Disables any competing Ollama startup task.
4. Starts the task and verifies `/api/health` before reporting success.

> **Note:** the `ops\` scripts are written for this machine — they hardcode
> the repo path and user profile. Adjust `$root`/`$repo` at the top of each
> script for your own setup.

## Startup script behaviour

`Start-InferDeck.ps1` is idempotent and safe to fire from multiple triggers
(logon task plus a periodic watchdog task):

- Exits immediately if the gateway already answers `/api/health` healthy.
- Uses a PID lock file (`run\inferdeck-startup.lock`) so concurrent triggers
  don't double-start.
- Stops competing inference servers (`ollama`, `llama-server`) before
  launching.
- Starts the gateway hidden with stdout/stderr redirected to `logs\`, then
  polls health for up to 20 s before reporting failure.

## Watchdog

Register `Start-InferDeck.ps1` on a repeating timer (e.g. every 15 minutes)
as a second task. Because the script is a no-op while the gateway is healthy,
this gives crash recovery for free. `ops\Repair-InferDeck-Startup.ps1`
re-registers the tasks if they're lost.

## Verify

```powershell
Invoke-RestMethod http://127.0.0.1:11434/v1/health   # { ok: true, ... }
Invoke-RestMethod http://127.0.0.1:11434/v1/models
# Dashboard: http://<host>:11434/
```
