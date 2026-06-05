# InferDeck Parity Harness

Records responses from `llama-server.exe` (the reference) and replays the
same prompts against `inferdeck-gateway` (the candidate), then scores each
candidate response against the baseline.

## Quick start

```powershell
# 1. Start llama-server with the model you want to baseline
llama-server.exe -m C:\Inferdeck\models\qwen3.6-27b\model.gguf -c 65536 --port 8080

# 2. Record baseline
pwsh -File tests/parity/record_baseline.ps1 -Model qwen3.6-27b

# 3. Start inferdeck-gateway (with same model registered)
build\bin\Debug\inferdeck-gateway.exe -c config\gateway.yml

# 4. Run parity
pwsh -File tests\parity\run.ps1 -BaselinePath tests\parity\baselines\qwen3.6-27b.jsonl
```

## Output

`run.ps1` writes a JSON report with per-prompt scores and an overall score.
Exit code `0` if every prompt meets its `min_score`, `1` otherwise.

The default `min_score` per prompt reflects realistic expectations:

- Factual / math: 0.80 (one-token answers are easy to match)
- Coding: 0.50 (the keyword "int add" or "bool is_even" is enough)
- Safety / summarization: 0.30-0.50 (open-ended)

## How scoring works

`ParityComparator` (in `include/parity/compare.hpp`) computes a token-level
LCS (longest common subsequence) similarity in `[0, 1]`. Both inputs are
normalized first:

1. `<think>...</think>` blocks stripped (Qwen3's chain-of-thought output)
2. Whitespace collapsed
3. Optionally lowercased

The score is `lcs_tokens / max(baseline_tokens, candidate_tokens)`.

## CI gate

`0.95` overall parity is the bar. Per-prompt scores are reported; the overall
score is the mean. Add to your CI:

```yaml
- pwsh -File tests\parity\run.ps1 -BaselinePath tests\parity\baselines\qwen3.6-27b.jsonl -OutputPath parity.json
```

## Files

- `prompts.json` — the fixed prompt set (6 prompts: 1 fact, 1 math, 2 code, 1 safety, 1 summarize)
- `record_baseline.ps1` — capture responses from llama-server
- `run.ps1` — replay against inferdeck-gateway + score
- `compare.hpp/.cpp` — C++ LCS comparator (unit-tested by `parity_tests`)
- `prompts.hpp/.cpp` — built-in prompt set (C++ side, used by C++ tests)
- `runner.hpp/.cpp` — orchestrates runner callbacks and produces a `ParityRun`
