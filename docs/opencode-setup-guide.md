# OpenCode + InferDeck Setup Guide

## Architecture

```
OpenCode → InferDeck Gateway (port 11434)
              ↓
         BackendCoordinator and native runtimes
```

The gateway runs llama.cpp inference directly in its own process on port 11434.
No external llama-server binary is needed.

## Quick Start

1. **Start InferDeck** — run the gateway (it loads the model and listens on port 11434)
2. **Verify the API** — `curl http://127.0.0.1:11434/v1/models`
3. **Run OpenCode** from the repo root — the `opencode.json` auto-detects the gateway

```bash
cd C:\Users\david\Documents\GitHub\InferDeck
opencode
```

## Use InferDeck model aliases

InferDeck publishes concrete models and stable aliases through `GET /v1/models`.
The checked-in `opencode.json` defines one OpenAI-compatible provider and keeps
the default selections stable:

```json
{
  "model": "inferdeck/Normal",
  "small_model": "inferdeck/n8n-model"
}
```

`Normal`, `Pro`, and `n8n-model` are resolved by InferDeck. Changing an alias
target on the server does not require changing these OpenCode model names.
Keep unrelated OpenCode plugins, MCP servers, and settings in `opencode.json`.

## Provider

The provider uses `provider.inferdeck.options.baseURL` as its `/v1` data-plane
base.

| Provider | Endpoint | Use Case |
|---|---|---|
| `inferdeck/Normal` | configured by `options.baseURL` | Stable normal-work alias |
| `inferdeck/Pro` | configured by `options.baseURL` | Stable demanding-work alias |
| `inferdeck/n8n-model` | configured by `options.baseURL` | Stable automation alias |

## Context Limits

`GET /v1/models` is authoritative for the server-side context and output
limits. OpenCode also keeps model metadata in `opencode.json`, so those values
must not exceed the alias target's advertised limits. Update the local metadata
when a target's capabilities change; the alias name itself remains stable.

For large repository tasks, keep discovery and implementation as separate
turns when the complete working set would exceed the advertised context.
## Reasoning Effort

InferDeck accepts `reasoning_effort` on Chat Completions and
`reasoning.effort` on Responses. Supported values are advertised per model by
`GET /v1/models`; unsupported values return HTTP 400.

The bundled `qwen3.8-27b` profile supports `low`, `medium`, `xhigh`, and `none`.
Its embedded template treats `high` as an alias for `xhigh`, uses `xhigh` by
default, and uses `none` to disable reasoning.

Add the model to the InferDeck provider in `opencode.json` with explicit
variants so OpenCode exposes them through `/variants` and `variant_cycle`:

```json
{
  "qwen3.8-27b": {
    "name": "qwen3.8-27b",
    "reasoning": true,
    "limit": { "context": 100000, "output": 16384 },
    "modalities": {
      "input": ["text", "image"],
      "output": ["text"]
    },
    "variants": {
      "low": { "reasoningEffort": "low" },
      "medium": { "reasoningEffort": "medium" },
      "xhigh": { "reasoningEffort": "xhigh" },
      "off": { "reasoningEffort": "none" }
    }
  }
}
```

`reasoning_effort` is supported on strict OpenAI routes. The derivative
`chat_template_kwargs.reasoning_effort` form is available only when the
default-off OpenAI-derivative profile is enabled and its `/compat` base is used.

## Troubleshooting

| Symptom | Fix |
|---|---|
| `connect ECONNREFUSED` | Gateway not running — start InferDeck first |
| `context length exceeded` | Reduce context or output limit |
| `reasoning_content` missing | Use OpenAI Responses reasoning events, or explicitly enable and target the derivative compatibility profile |

## Reference

- [Strict OpenAI compatibility](openai-compatibility.md)
- [Architecture](architecture.md)
- [Streaming tool-call harness](../Testing/mini-ralph.mjs)
