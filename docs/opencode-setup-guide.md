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

## Troubleshooting

| Symptom | Fix |
|---|---|
| `connect ECONNREFUSED` | Gateway not running — start InferDeck first |
| `context length exceeded` | Reduce context or output limit |
| `reasoning_content` missing | Don't pass `--reasoning-format` to the engine (default is correct) |

## Reference

- [Full diagnostics](qwen36-opencode-diagnostics.md)
- [Smoke test script](../scripts/test-qwen36-response.ps1)
