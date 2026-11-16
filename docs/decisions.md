# Decisions

`POST /v1/decisions` answers typed questions about an input with calibrated
probabilities instead of generated text. It follows the request and response
shape of the OpenAI Decisions API, so a client written for that API can point
at InferDeck.

It is served by decision models. InferDeck ships one runtime for them,
`jev_style`, which runs Jev-Style GGUF models in-process on llama.cpp.

## Request

```json
{
  "model": "jev-style-0.8b",
  "input": "I was charged twice and I want my money back.",
  "questions": [
    {"type": "predicate", "name": "refund", "instructions": "The customer is asking for a refund."},
    {"type": "choice", "name": "team", "instructions": "Which team should handle this?",
     "choices": [{"value": "billing", "description": "payments and refunds"},
                 {"value": "technical"}]},
    {"type": "score", "name": "anger", "instructions": "How upset is the customer?",
     "levels": [{"label": "calm"}, {"label": "annoyed"}, {"label": "very upset"}]}
  ]
}
```

| Field | Rule |
|---|---|
| `model` | A model that declares the `decisions` capability, or an alias for one. |
| `input` | A string, or an array of messages (`role`, `content`). Message content is a string or `input_text` / `input_json` parts. Each message becomes one `role: content` line. |
| `questions` | 1 to 400. Answers come back in the same order. |
| `predicate` | `instructions` only. |
| `choice` | `choices`: 2 to 255, each with a unique string or boolean `value` and an optional `description`. |
| `score` | `levels`: 2 to 10, lowest first, each with a `label` and an optional `description`. |
| `name` | Optional, echoed in the answer. |
| `prompt_cache_key`, `prompt_cache_options`, `safety_identifier` | Accepted and ignored. |

Any other field is rejected with `unsupported_parameter`.

## Response

```json
{
  "model": "jev-style-0.8b",
  "answers": [
    {"type": "predicate", "name": "refund", "probability": 0.97},
    {"type": "choice", "name": "team", "choice": "billing", "confidence": 0.95,
     "probabilities": [{"value": "billing", "probability": 0.97},
                       {"value": "technical", "probability": 0.03}]},
    {"type": "score", "name": "anger", "score": 1.5, "confidence": 0.26,
     "probabilities": [{"value": 0, "label": "calm", "probability": 0.02},
                       {"value": 1, "label": "annoyed", "probability": 0.46},
                       {"value": 2, "label": "very upset", "probability": 0.52}]}
  ],
  "usage": {"input_tokens": 410, "input_tokens_details": {"cached_tokens": 0, "cache_write_tokens": 0},
            "output_tokens": 0, "output_tokens_details": {"reasoning_tokens": 0}, "total_tokens": 410}
}
```

- `probability` is the chance the statement is true.
- `score` is the probability-weighted average of the level indices.
- Choice `confidence` is `(k * p_max - 1) / (k - 1)` for `k` choices. Score
  `confidence` is 1 minus the probability spread around the most likely level,
  relative to an even spread.

## Differences from OpenAI

- Images, files, and tool-call items in `input` or `instructions` are rejected
  with `invalid_input`. The supported models are text only.
- There are no refusal answers.
- Nothing is cached, so the cached token counts are always 0.
- Limits are the model's, not OpenAI's. 0.8B v3: the whole rendered input
  must fit in 25,600 tokens and one question with its options in 2,048.
  2B v4: the whole rendered input must fit in 32,768 tokens, with no separate
  question limit. Nothing is truncated; an input over a limit fails with
  `context_length_exceeded`.
- The route was added on 2026-11-12 from the OpenAI Decisions reference. It is
  not covered by the pinned SDK contract suites in
  [openai-compatibility.md](openai-compatibility.md), whose SDK versions
  predate the API.

## Model setup

Two models are supported (Apache-2.0). For each, download one GGUF and
`readout_config.json` into the same folder.

| Model | Repository | Revision |
|---|---|---|
| 0.8B v3 | `chaoliangUNSW/Jev-Style-0.8B-Decision-v3-GGUF` | |
| 2B v4 | `chaoliangUNSW/Jev-Style-2B-Decision-v4-GGUF` | `9111d5d232d21d52593c4c10f1e8cfa6561669f6` |

```yaml
model_registry:
  - name: jev-style-0.8b
    family: jev-style
    runtime: jev_style
    modality: decision
    capabilities: [decisions]
    gguf_path: "F:/InferDeck/models/decision/jev-style-0.8b-v3/Jev-Style-0.8B-Decision-v3-Q8_0.gguf"
    n_slots: 1
    vram_required_mb: 2000
  - name: jev-style-2b
    family: jev-style
    runtime: jev_style
    modality: decision
    capabilities: [decisions]
    gguf_path: "F:/InferDeck/models/decision/jev-style-2b-v4/Jev-Style-2B-Decision-v4-Q8_0.gguf"
    n_slots: 1
    vram_required_mb: 3000   # estimate, not measured
```

`readout_config.json` is read from beside the GGUF. Set the `readout_config`
artifact to use another path. Its `format`, `template`, and `readout` fields
choose the layout below; any other template fails the load. The file also
names the answer tokens and the calibration temperatures. Loading fails if the
model's tokenizer does not produce those tokens, and, for 2B v4, if the GGUF's
`general.name` differs from the config's `model_name` (the 0.8B and 2B models
share a tokenizer, so a mismatched GGUF would otherwise load and answer
wrongly).

## How it works

The model does not generate. 0.8B v3 (`macjev-render-v1`) renders the request
as plain text:

```
State:
<input>

Question [choice]: <instructions>
Options:
- <option 1>
- <option 2>
Judge each option:
<option 1> ->
<option 2> ->
```

One causal forward pass reads the logits at each ` ->`. An option's score is
`logit(" yes") - logit(" no")` there, and the probabilities are
`softmax(scores / T)` with the temperature fitted for that question type and
option count.

2B v4 (`macjev-render-v2-long-options`) drops the "Judge each option" section
and puts the slot on each option line:

```
State:
<input>

Question [choice]: <instructions>
Options:
- <option 1> ->
- <option 2> ->
```

If the question and its option lines exceed 2,048 tokens, it lists the options
as `Option k: <text>` lines, then `Judge each numbered option in the complete
catalogue above:` and one `Option k ->` line per option.

The input is cut into blocks of at most 2,048 tokens: the state, then one
question block (or catalogue blocks and rubric blocks). The model attends
block-causally: each block sees all earlier tokens and all of itself. The
runtime gets this from stock llama.cpp with a `NON_CAUSAL` context,
`n_batch = n_ubatch = 2048`, and one `llama_decode` per block. The state is
decoded once per request into sequence 0. Each question is decoded into
sequence 1 on a copy of it. Probabilities are `softmax(scores / T)`, where `T`
is the fitted temperature for the question type (`choice`, `noul` for
predicates, `score`).

## Measured

Jev-Style 0.8B v3, Q8_0, Vulkan on the R9700, 2026-11-12:

| Request | Time |
|---|---|
| One predicate, 76 input tokens, warm | 16 ms |
| Four questions, 410 input tokens, warm | 64 ms |
| First request after load | 0.8 s |

VRAM use is about 1.5 GB.

## Limits of this implementation

- **Not verified against the reference numerically.** The renderer and readout
  were ported from the model's published reference runtime and the answer
  tokens are checked at load, but the reference itself was not run here, so
  bit-level agreement on arbitrary text is untested.
- One request at a time per model. Each question re-reads the input; shared
  input caching across questions is not implemented.
- **2B v4 is experimental.** Its renderer and block scorer are implemented
  and unit-tested, and a previous review found them consistent with the
  reference source. On 2026-11-16 the model loaded and returned HTTP 200 for a
  set of test requests, but the answers, latency, and VRAM were not recorded
  before that run was cut short, so its accuracy on the GPU is unverified.
  Use 0.8B v3 for anything that matters until that check is repeated.
