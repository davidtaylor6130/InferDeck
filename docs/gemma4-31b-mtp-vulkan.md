# Gemma 4 31B MTP on Vulkan

## Current decision

Keep MTP disabled for `gemma-4-31b` on the AMD Vulkan deployment.

Gemma 4 uses a separate assistant GGUF rather than Qwen 3.6's embedded MTP
layers. Unsloth publishes the 515 MB `mtp-gemma-4-31B-it.gguf`, and upstream
llama.cpp added the corresponding `gemma4-assistant` architecture in build
b9549. InferDeck's current llama.cpp revision is b9276 and its adaptive MTP
wrapper only initializes the target GGUF as both target and draft.

Updating the wrapper alone is not sufficient on this machine. The upstream
Gemma 4 31B external-drafter path currently crashes during Vulkan context
initialization when the assistant context shares the target model's KV tensors.
The upstream Vulkan report is still open:

https://github.com/ggml-org/llama.cpp/issues/24492

A later cross-backend report also reproduces failures with the Unsloth drafter:

https://github.com/ggml-org/llama.cpp/issues/25873

Do not move the llama.cpp submodule or install the drafter in the live profile
until the Vulkan issue is fixed and verified on the Radeon AI PRO R9700.

## Measured baseline

The installed `gemma-4-31B-it-UD-Q4_K_XL.gguf` is 18,822,970,304 bytes and
generates about 27-28 tokens per second. The R9700 has 640 GB/s memory bandwidth.
Reading the quantized weights once per generated token already implies roughly
508-527 GB/s, so ordinary one-token decoding is close to the device's practical
bandwidth ceiling. Lowering the configured context did not materially improve
decode throughput in earlier runs.

Qwen 3.6 27B reaches roughly 50-60 tokens per second because its accepted MTP
drafts amortize a target-model pass across multiple output tokens. It is not an
apples-to-apples dense-model baseline for Gemma without a working Gemma drafter.

## Revisit checklist

1. Confirm the upstream Vulkan shared-KV issue is fixed.
2. Update llama.cpp on a review branch and prove the existing Qwen adaptive-MTP
   tests and throughput have not regressed.
3. Add a distinct draft GGUF path to `ModelInfo`, `LlamaCppConfig`, and the YAML
   `speculative` map.
4. Load the assistant model in-process, create its context with `ctx_other` set
   to the target context, and preserve the current adaptive concurrency rules.
5. Benchmark draft acceptance, output quality, VRAM use, and single-request
   throughput before enabling it in `config/gateway.yml`.
