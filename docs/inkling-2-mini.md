# Inkling 2 Mini

InferDeck runs Thinking Machines' Inkling 2 Mini in-process through the
`llama_cpp` runtime: text, vision, OpenAI tool calls, and reasoning effort.
The weights are Apache-2.0.

The model is a looped sparse mixture of experts: 26 B parameters, 128 experts
with 8 routed, 3.4 B active per token (about 5.4 B with the loop passes), and
local/global attention with a 1,048,576-token trained context. llama.cpp names
the mainline GGUF architecture `inkling2_loop`.

Status: validated on the R9700 (32 GB, Vulkan) on 2026-11-21. See
[Validation](#validation).

## Runtime pin

Inkling 2 Mini needs llama.cpp `b12614` or later. Older builds do not know the
architecture; builds with ggml-org/llama.cpp#32694 but without #32704 load the
model and print gibberish, because the loop passes run the NVFP4 experts
without their scales.

InferDeck pins `0ea4ffd00`: `b12614` plus ggml-org/llama.cpp#32735, an
approved one-line Vulkan fix that lets `SCALE` take a strided view. Without it
the SiTU activation of every MoE layer runs on the CPU, and the R9700 decodes
at 18 tokens/s instead of 60. Move the pin to the upstream merge once #32735
lands.

## Weights

Use the mainline-layout GGUF files from
[unsloth/Inkling-2-Mini-GGUF](https://huggingface.co/unsloth/Inkling-2-Mini-GGUF).
The files in `Mainline_Support/` end in `.gguf_file`; save them as `.gguf`.
The files at the repository root use a different layout and do not load in
mainline llama.cpp.

| File | Size | SHA-256 |
|---|---|---|
| `Mainline_Support/Inkling-2-Mini-UD-Q4_K_XL.gguf_file` | 17,629,760,736 | `e64276656f072e9b2197a61723667cbdb71da66a716ffd966e572c21088f3cd3` |
| `mmproj-F16.gguf` | 639,348,160 | `3bff75ffeb2d6e0371ba7bd42e30d26a7d98aea681fa44b03b5a20891cfa2c5f` |

`UD-Q4_K_XL` keeps the experts in NVFP4 and everything else in Q8_0.

## Configuration

```yaml
- name: inkling-2-mini
  family: inkling-2
  gguf_path: E:/InferDeck/models/unsloth/Inkling-2-Mini-GGUF/Inkling-2-Mini-UD-Q4_K_XL.gguf
  mmproj_path: E:/InferDeck/models/unsloth/Inkling-2-Mini-GGUF/mmproj-F16.gguf
  n_slots: 4
  min_slots: 4
  vram_required_mb: 27000
  context_size: 131072
  n_batch: 2048
  n_ubatch: 128
  cache_type_k: q4_0
  cache_type_v: q4_0
  has_vision: true
  reasoning:
    supported: true
    efforts: [none, minimal, low, medium, high, xhigh]
    default: high
    none_disables: true
    aliases:
      max: xhigh
  sampling:
    temperature: 1.0
    top_p: 1.0
    top_k: 0
    min_p: 0.0
    repeat_penalty: 1.0
```

- **Sampling** is the model card's: temperature 1.0, top_p 1.0, top_k 0,
  min_p 0.0.
- **Reasoning.** The chat template reads `reasoning_effort` as a string and
  maps it to a number: none 0, minimal 0.1, low 0.2, medium 0.7, high 0.9,
  xhigh and max 0.99. With no value it uses 0.9, so `default: high` matches
  the model's own default. List `none` as an effort so a `none` request reaches
  the template as effort 0; otherwise InferDeck drops the variable and the
  template falls back to 0.9. Effort 0 is not a hard switch: the model still
  writes a short reasoning block. Reasoning arrives in `reasoning_content`.
- **`n_ubatch: 128` is required for long prompts.** The model adds a relative
  position bias to global attention. llama.cpp materialises that bias as an
  f32 tensor of heads x context x ubatch, so it grows with both. Prefill
  collapses once context times ubatch passes a limit between 17 M (fine) and
  33.5 M (collapsed): at a 64K-token depth, `n_ubatch: 512` prefills at 29
  tokens/s with the CPU busy, 256 at 420, and 128 at 395. The cause beyond the
  tensor size was not isolated. 128 keeps 131,072-token slots at the known-good
  size. It costs about a quarter of the prefill
  speed on short prompts (985 against 1,328 tokens/s at 2K). Raise it only
  with shorter slots: 256 is safe up to 65,536. ggml-org/llama.cpp#32740
  (open) moves the bias into flash attention and removes this limit.
- **KV cache.** Only the 8 global attention passes (4 global layers, 2 of them
  inside the 3-pass loop) keep the full context; the sliding-window layers
  keep 1,024 tokens. q8_0 costs about 17 KB per token and q4_0 about 9 KB.
  Four 131,072-token slots measured 29.6 GB of VRAM with q8_0 and 25.7 GB
  with q4_0. KV type changes speed by under 5%.
- **1,048,576 tokens** does not fit in 32 GB with this pin.
- **Speculative decoding** is not available. The official MTP head is only in
  the safetensors release, and the DSpark drafter needs the unmerged
  ggml-org/llama.cpp#32218.

## Validation

Measured on the R9700 with the configuration above unless noted.

| Check | Result |
|---|---|
| Coherence | Correct, fluent answers; no gibberish |
| Reasoning | `reasoning_content` split; every effort from `none` to `max` accepted |
| Tool calls | Single and parallel calls, tool-result turn, `parallel_tool_calls: false` honoured; `Testing/mini-ralph.mjs` passes with serial calls |
| Vision | Described a test image correctly (animal, object, colours) |
| Long context | Found a needle at 30% depth of a 129,071-token prompt (q4_0 KV), 368 s; VRAM flat at 25.8 GB |
| GPU health | No new watchdog or bugcheck reports during the session |

Speed on the R9700 from a streaming benchmark (512 output tokens, reasoning at
each model's default, so decode includes reasoning tokens). Flint and Qwen3.8-27B ran their live entries
(Flint with DFlash2, Qwen3.8-27B with MTP) on the same build.

| Scenario | Inkling 2 Mini | Flint | Qwen3.8-27B |
|---|---|---|---|
| Short prompt, decode | 58.6 t/s | 122.1 t/s | 41.9 t/s |
| 8K prompt: prefill / decode | 797 / 56.5 t/s | 3,277 / 117.0 t/s | 788 / 46.3 t/s |
| 4 x 8K concurrent: wall / mean decode | 63.1 s / 25.6 t/s | 17.9 s / 57.3 t/s | 79.4 s / 16.8 t/s |
| 38K prompt: prefill / decode | 609 / 47.2 t/s | 1,982 / 84.5 t/s | 603 / 41.4 t/s |
| 4 x 38K concurrent: wall / mean decode | 292.0 s / 17.5 t/s | 83.6 s / 46.2 t/s | 322.9 s / 14.4 t/s |
| Code prompt (38K), decode | 47.3 t/s | 92.2 t/s | 38.1 t/s |
| Peak VRAM | 25.8 GB | 31.4 GB | 28.2 GB |

Inkling 2 Mini is faster than Qwen3.8-27B everywhere and needs less memory,
but on this runtime it is about half Flint's speed and a third of its prefill:
the loop passes run 48 layer passes per token, global attention carries the
bias tensor described above, and there is no speculative decoding yet.

llama-bench, same pin, 4 K prompt, flash attention on: `n_ubatch` 512 gives
1,298 prefill tokens/s, 1,024 gives 1,215, 2,048 gives 1,077 and 4,096 gives
879. Decode is 60 tokens/s at an empty context and 47 at 32K. q8_0, q4_0 and
f16 KV are within 5% of each other.

Quality spot checks (single runs, temperature 1): Inkling solved an
under-constrained seating puzzle by listing both valid answers, where Flint
ran out of its 6,000-token budget; both got an arithmetic-series question
right; on a two-function Python task with asserts, Flint and Inkling at `low`
passed, Inkling at `medium` wrote one wrong test case, and Inkling at `high`
spent the whole 6,000-token budget reasoning. Give coding clients a large
`max_tokens` or a lower effort.
