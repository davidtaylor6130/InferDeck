# InferDeck MCP

MCP access to InferDeck discovery and optional shared operator-scoped media tools. The adapter does not write gateway configuration.

## Install

```powershell
Set-Location mcp
pnpm install --ignore-workspace
pnpm test
```

## Environment

Keep secrets in environment variables. Server variables belong to the MCP process. Client variables belong to OpenCode or its host process.

MCP server:

```powershell
$env:INFERDECK_URL = 'http://127.0.0.1:11434'
$env:INFERDECK_API_KEY = '...'
$env:INFERDECK_CONTROL_TOKEN = '...'
$env:MCP_BEARER_TOKEN = '...'
$env:MCP_ENABLE_MEDIA = 'true'
$env:MCP_GENERATION_TIMEOUT_MS = '300000'
$env:MCP_BIND_HOST = '127.0.0.1'
$env:MCP_PORT = '11436'
$env:MCP_ALLOWED_HOSTS = 'localhost,127.0.0.1,[::1]'
$env:MCP_ALLOWED_ORIGINS = 'localhost,127.0.0.1,[::1]'
pnpm start
```

`MCP_BEARER_TOKEN` is the shared token required for every MCP request. `INFERDECK_CONTROL_TOKEN` is needed when authenticated remote control-read routes are used. `MCP_ENABLE_MEDIA=true` enables media tools. `MCP_GATEWAY_TIMEOUT_MS` defaults to 30000 ms for discovery and ordinary gateway calls. `MCP_GENERATION_TIMEOUT_MS` defaults to 300000 ms and must be an integer from 1 to 1800000; it controls synchronous media generation calls.

The server defaults to loopback binding and loopback host/origin allowlists. For three-machine access, set `MCP_BIND_HOST` to the intended interface and provide explicit `MCP_ALLOWED_HOSTS` and `MCP_ALLOWED_ORIGINS`. Prefer a TLS reverse proxy in front of the MCP server. Keep the bearer token scoped to this MCP service and never put it in prompts or repository files.

OpenCode/client:

```powershell
$env:INFERDECK_PUBLIC_URL = 'https://inferdeck.example'
$env:INFERDECK_API_KEY = '...'
$env:INFERDECK_MCP_PUBLIC_URL = 'https://inferdeck.example/mcp'
$env:INFERDECK_MCP_TOKEN = '...'
$env:SEARXNG_BASE_URL = 'https://search.example'
```

`INFERDECK_MCP_TOKEN` must equal the server's `MCP_BEARER_TOKEN`. `INFERDECK_PUBLIC_URL` is the gateway origin; generated configs append `/v1`. `INFERDECK_MCP_PUBLIC_URL` is the endpoint OpenCode reaches. `INFERDECK_API_KEY` is used for the OpenAI-compatible gateway provider. Do not commit real values.

## OpenCode config

`GrabOpenCodeConfig` discovers `GET /api/inferdeck/v1/models` and generates model entries from returned metadata. It uses `context_size` and `has_vision`, excludes models without chat capabilities, and selects defaults only for exact chat-capable aliases `normal` and `small-model`.

Output budgets default to 16384 for normal models and 8192 for `utility`, `compact-coder`, and `small-model`. Each emitted budget is capped at `floor(context_size / 2)` to leave prompt room. The tool warns when OpenCode versions differ on the custom compaction and tool-output fields.

The generated config uses direct `mcp.NAME` entries for InferDeck and searxng only. It sets `experimental.mcp_timeout` to 300000 ms for MCP requests and uses a matching per-server fetch timeout. It preserves the requested compaction and tool-output settings without writing any file.

## Media tools

Media uses shared operator scope and bearer-token authentication. With `MCP_ENABLE_MEDIA=true`, tools are `generate_image`, `generate_music`, `synthesize_speech`, `generate_video`, `list_media_jobs`, `get_media_output`, and `cancel_media_job`. Video calls check live model capabilities before generation; without a configured video model they return an availability error. See [LTX-2.3.md](LTX-2.3.md) for setup.

Generation is synchronous. Set `MCP_GENERATION_TIMEOUT_MS` high enough for the operation and keep the OpenCode/client call timeout aligned. Music accepts up to 600 seconds of requested duration. Stored output retrieval is capped at 25 MiB.

