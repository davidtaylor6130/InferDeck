# OpenCode Mac Verification

Use this from the Mac that runs OpenCode. It verifies the config/provider path, proves Open WebUI remains healthy, runs the file-read OpenCode repro, and checks InferDeck dashboard state.

## Expected Config

The Mac OpenCode config should match the shape in `docs/opencode-mac-inferdeck.example.json`:

- provider key: `inferdeck`
- `baseURL`: `http://ai.homelab.com:11434/v1`
- top-level `model`: `inferdeck/qwen3.6-35b-a3b:latest`
- top-level `small_model`: `inferdeck/qwen3.6-35b-a3b:latest`
- keep the existing long output limit: `65536`

## One Command

Run from a local repo/site directory on the Mac:

```bash
EXPECTED_BASE_URL=http://ai.homelab.com:11434/v1 \
DASHBOARD_BASE_URL=http://ai.homelab.com:8080 \
MODEL=inferdeck/qwen3.6-35b-a3b:latest \
MAX_OPENCODE_SECONDS=240 \
/path/to/InferDeck/scripts/smoke-opencode-mac.sh
```

## What A Pass Proves

- OpenCode config exists and targets InferDeck.
- `ai.homelab.com` resolves and port `11434` is reachable.
- `/v1/models` works.
- tiny `/v1/chat/completions` works.
- `/api/chat` Open WebUI control works before, during, and after OpenCode.
- OpenCode sends a request that InferDeck sees within 2 seconds.
- The final OpenCode job is terminal: succeeded or failed, not hidden/running.
- Dashboard queue is clean after the run.
- Dashboard reports exactly one running managed llama service.

## How To Read Failures

- `provider/baseURL is not targeting InferDeck`: OpenCode is using the wrong config/provider.
- `no top-level model set`: OpenCode may be using a session/default model instead of the intended InferDeck model.
- `no request sent to InferDeck`: OpenCode stalled locally before the LLM call.
- `OpenCode did not finish after InferDeck accepted a request`: gateway/backend accepted work, but OpenCode did not complete the local loop.
- `dashboard OpenCode evidence failed`: InferDeck did not record the request as a terminal OpenCode job.
- `dashboard backend check failed`: queue/backend state is not clean or the dashboard does not report exactly one running llama service.

