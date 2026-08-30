# InferDeck Windows deployment

InferDeck deploys as one native executable plus a matched static dashboard.
The live installation is outside the repository at `C:\InferDeck`.

## Authoritative boot target

The production boot mechanism is the automatic LocalSystem NSSM service
`InferDeck`. Verify it before every activation:

```powershell
Get-CimInstance Win32_Service -Filter "Name='InferDeck'"
Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\InferDeck\Parameters'
```

Expected registry values are `Application=C:\InferDeck\inferdeck-gateway.exe`,
`AppDirectory=C:\InferDeck`, and `AppParameters=-c config\gateway.yml`.
Legacy InferDeck startup, logon, and watchdog tasks and the old
`InferDeckGateway` service are disabled. Do not deploy through them. Reconfirm
this state because boot configuration can drift independently of files.

## Matched artifacts

Build both artifacts from one source revision:

```powershell
cmake --build build --target inferdeck-gateway --config Release --parallel
pnpm --filter dashboard build
```

Activate `build\bin\Release\inferdeck-gateway.exe` as
`C:\InferDeck\inferdeck-gateway.exe` and
`apps\inferdeck-gateway\static\` as `C:\InferDeck\static\`. The gateway
resolves `static` relative to its executable. Never activate only one
artifact. Remove stale hashed bundles from the staged static directory before
the directory swap.

## Prove build identity

Every development and release executable reports its semantic version, full Git
revision, and dirty state:

```powershell
& .\build\bin\Release\inferdeck-gateway.exe --version
```

A release candidate must report the expected 40-character revision and
`dirty=false`. Source archives without `.git` metadata must configure with
`-DINFERDECK_BUILD_REVISION=<full-sha>`. After activation,
`GET /api/inferdeck/v1/health` must report the same `version`, `build_revision`,
and `build_dirty=false`. Stop verification if the CLI, health response, release
manifest, and intended source revision do not agree.

## Backup and activation

Explicit owner authorization is required before writing `C:\InferDeck` or
restarting the service.

1. Capture the source commit, artifact hashes, service registry values, running
   service and child PIDs, configuration revision, and live endpoint behavior.
2. Back up the active executable, complete static directory,
   `config\gateway.yml`, StatsDb with WAL and SHM files, managed-model
   manifests, and service registry values into one timestamped rollback set.
3. Validate staged configuration and open a copy of StatsDb with the new build.
4. Stop only the verified `InferDeck` service and confirm its NSSM parent and
   gateway child exited.
5. Replace the executable and complete static directory, then apply validated
   configuration and state migrations.
6. Start `InferDeck` and verify the service PID, gateway child, executable
   hash, configuration revision, and static asset hashes.

An on-disk replacement is not a deployment until the restarted process and
live listener prove those exact artifacts are active.

## Secure LAN and credentials

The OpenAI bearer token authenticates only `/v1`. Rotate it through the
versioned configuration transaction, verify the new token, and prove the old
token returns 401.

Remote administration is separately opt-in. Enable
`control.allow_remote`, use a distinct control token with at least 32
cookie-safe ASCII characters, configure exact HTTP or HTTPS origins, and carry
traffic over an encrypted overlay because the listener has no TLS. Forwarded
headers and reverse proxies never inherit loopback authority. After rotation,
prove unauthenticated, data-token, wrong-origin, and stale-token control
requests fail. Remote dashboards exchange the control token for an HTTP-only,
same-site session cookie; loopback dashboard access remains passwordless.

## Verification and rollback

Verify the strict route manifest; official Python and JavaScript SDK clients;
Chat and Responses stream and non-stream; linked media formats; control
security; request correlation through logs, StatsDb, SSE, metrics, and the
dashboard; and the absence of Python, FFmpeg, `llama-server`, proxy, or other
runtime child processes. Verify reboot persistence through NSSM separately from
the immediate restart.

Rollback is a matched-pair operation. Stop the same verified service, restore
the executable, complete static directory, configuration, StatsDb, and
manifests from one rollback set, restart, and repeat health, model, dashboard,
request, and boot-target probes. If a file is locked or a target differs, stop
and request owner direction.
