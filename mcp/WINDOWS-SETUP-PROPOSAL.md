# Windows MCP companion setup proposal

This proposal installs a separate automatic LocalSystem NSSM service named
`InferDeckMcp`. It runs `node.exe` against this checkout's `mcp/src/index.mjs`,
uses the gateway only through `http://127.0.0.1:11434`, and does not modify,
stop, restart, or reconfigure the existing `InferDeck` service.

The installer is intentionally not run as part of repository work. Review the
script and choose the final interface before activation.

## Review validation

From the repository root, with Node.js 22+, NSSM, and an existing `InferDeck`
service installed:

```powershell
& .\mcp\test\Test-InferDeckMcpSetup.ps1
```

This starts no service and creates no firewall rule. It checks required tokens,
LAN allowlist requirements, token length, and occupied-port refusal.

## Proposed activation command

Run in an elevated PowerShell session only after review. Do not put real tokens
in repository files or committed scripts. Omitting a token parameter prompts
for it as a secure PowerShell input.

```powershell
& .\mcp\scripts\Install-InferDeckMcp.ps1 -Install `
  -BindHost '192.168.0.10' `
  -AllowedHosts 'machine-a','machine-b','machine-c' `
  -AllowedOrigins 'machine-a','machine-b','machine-c' `
  -Port 11436
```

The script refuses to use an occupied TCP or UDP port and never stops the
owner. If port `11436` is used, select another explicit port and update the
three clients' MCP URL. The service is installed as Automatic but is left
stopped. Starting it is a separate owner action.

NSSM captures stdout/stderr under the protected runtime `logs\\` directory and rotates each log at 10 MiB. The persisted NSSM environment contains three explicitly supplied values:
`INFERDECK_API_KEY`, `INFERDECK_CONTROL_TOKEN`, and `MCP_BEARER_TOKEN`. The
gateway token is never discovered or copied automatically. The MCP bearer is a
shared trust-domain token, not per-machine or per-user authorization. Keep the
gateway loopback-only and put any broader network exposure behind an approved
TLS boundary. This proposal does not create firewall rules, TLS certificates,
reverse-proxy configuration, or client configuration files.

The NSSM parameter key is protected for LocalSystem and Administrators only;
ordinary users are not granted access. Verify the resulting service registry
values and the three clients' secure environment configuration before a later
activation task starts the service.
