# OpenCode + InferDeck Setup Guide

## Architecture

```
OpenCode → InferDeck Gateway (port 11434)
              ↓
         LlamaEngine (in-process inference)
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

## Provider

| Provider | Endpoint | Use Case |
|---|---|---|
| `inferdeck/qwen36` | `http://127.0.0.1:11434/v1` | Primary — in-process gateway |

## Context Limits

| Setting | Value | Reason |
|---|---|---|
| `context` | 65536 | Stable on 4GB VRAM + 19.7GB model |
| `output` | 8192 | Prevents premature truncation |
| Max safe | ~98k | PC crashes beyond this (memory pressure) |

For large tasks (full repo reviews), use the **two-phase workflow**:

### Phase 1: Explore and summarize
```bash
opencode run "Read src/main.ts, src/config.ts, src/utils.ts.
Write arch-findings.md summarizing the architecture."
```

### Phase 2: Analyze and write
```bash
opencode run "Read arch-findings.md.
Create optimization-plan.md with actionable recommendations."
```

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
