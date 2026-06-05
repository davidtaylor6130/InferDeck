# AGENTS.md — InferDeck 2.0

**For:** Future AI agents and developers working on this codebase.
**Read this first.** It saves you from re-deriving what the previous team learned.

## What is InferDeck?

A local AI gateway. Single Windows `.exe` that runs LLMs (currently
Qwen3.6-27B and Qwen3-Coder-Next) in-process via llama.cpp, exposes
an OpenAI-compatible HTTP API on `:11434/v1/`, and serves a React
dashboard on `:11434/`.

**Hard constraint:** Everything in-process. NO subprocess. NO
`llama-server.exe` proxy. NO orphan processes. This is non-negotiable
because the user is on Windows and orphan `llama-server.exe` processes
have been a pain point.

## Project Structure

```
apps/gateway-service/         HTTP routes (Layer 8)
apps/benchmark-runner/        Optuna optimization harness (Layer 9)
apps/model-tester/            Swap exerciser (P3 dev tool)
apps/hardware-adlx-helper/    ADLX wrapper for GPU telemetry — REUSE THIS
                              in libs/observability/, do not rewrite

libs/foundation/              asio, logging, JSON (Layer 1)
libs/messaging/               std::variant content, role enum (Layer 2)
libs/sampling/                common_sampler_init wrapper (Layer 3)
libs/model/                   ModelRegistry + BackendCoordinator (Layer 4)
libs/engine/                  Per-model slot pool (Layer 5)
libs/scheduler/               LCP-match + queue (Layer 6)
libs/observability/           ADLX + EMA stats + SQLite (Layer 7)
libs/llama_cpp_wrapper/       Legacy — being replaced by new layers
libs/core/                    Legacy — being replaced
libs/third_party/llama.cpp    Layer 0, Vulkan build

config/gateway.yml            Active config (only this one is read)
config/sampler-profiles/      Per-model sampler configs
config/bench-search-spaces.yaml

tests/parity/                 CI gate: parity with raw llama-server
tests/stress/                 4h session, swap cycles
tests/integration/            HTTP end-to-end
```

## Build Commands

```bash
# Configure (one-time)
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang-cl \
      -DCMAKE_CXX_COMPILER=clang-cl \
      -DCMAKE_TOOLCHAIN_FILE=cmake/clang-cl-msvc.txt

# Build everything
cmake --build build --config Release -j

# Build specific layer
cmake --build build --target inferdeck-gateway
cmake --build build --target inferdeck-bench

# ASan build
cmake -S . -B build-asan -G "Visual Studio 17 2022" -A x64 \
      -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_C_FLAGS="-fsanitize=address" \
      -DCMAKE_CXX_FLAGS="-fsanitize=address"
cmake --build build-asan --config Debug -j
```

## Test Commands

```bash
# Unit tests (fast, every commit)
ctest --test-dir build --output-on-failure -L unit

# Integration tests (medium, every commit, includes Tier A + Tier B once P3 lands)
ctest --test-dir build --output-on-failure -L integration

# All non-e2e (unit + integration)
ctest --test-dir build --output-on-failure -LE e2e

# Real-model end-to-end (slow, pre-release, needs test GGUF)
bash tests/integration/run.sh e2e

# All tiers including e2e
bash tests/integration/run.sh all

# Parity tests (CI gate, slow)
bash tests/parity/run.sh

# Swap stress test
bash tests/stress/swap_cycle.sh 20

# Real-hardware validation (4h session)
bash tests/stress/four_hour_session.sh
```

### Test Labels

| Label         | Speed     | When          | Requires                |
|---------------|-----------|---------------|-------------------------|
| `unit`        | fast      | every commit  | nothing                 |
| `integration` | medium    | every commit  | nothing (Tier A + B)    |
| `e2e`         | slow      | pre-release   | test GGUF in `C:/Inferdeck/models/` |

### Integration Test Fixtures

Real opencode / openwebui / Anthropic payloads live in `tests/fixtures/`.
Each fixture is a JSON file representing one realistic request body from a
client you actually use (opencode, openwebui) or from the Anthropic Messages
API. Tier A tests parse each fixture, round-trip through `messaging`, and
diff against the original.

Tier C uses the Qwen2.5-Coder-3B-Instruct Q4_K_M GGUF at
`C:/Inferdeck/models/Qwen/Qwen2.5-Coder-3B-Instruct-GGUF/qwen2.5-coder-3b-instruct-q4_k_m.gguf`
(~2 GB, small enough for CI). Override with `INFERDECK_TEST_MODEL` env var.

## Code Conventions

- **C++23.** Use `co_await`, `std::expected`, `std::span`, `std::variant`,
  `std::visit`. No raw `new`/`delete`, use `std::unique_ptr`/`std::shared_ptr`.
- **asio awaitables** for all I/O. No callback hell.
- **Structured logging**: `LOG_INFO("model_loaded", "name"_a=name, "vram_mb"_a=vram)`.
  Never `std::cout`.
- **JSON**: `nlohmann::json` or `simdjson`. Pick one and stick with it.
- **No exceptions for control flow.** Use `std::expected<T, Error>`.
- **Headers**: `#pragma once`, order: related -> <system> -> <std> -> third-party -> local.
- **Naming**: PascalCase types, camelCase functions/variables, `k_` prefix
  for constants, `_t` suffix for type aliases.
- **File layout**: `libs/{layer}/include/{layer}/{file}.hpp`,
  `libs/{layer}/src/{file}.cpp`, `libs/{layer}/tests/test_{file}.cpp`.

## Architecture Quick Reference

When a request hits `/v1/chat/completions`:

1. **Layer 8** parses OAI body, extracts `model:`, `messages:`, `tools:`
2. **Layer 2** converts to internal message representation
3. **Layer 5** Scheduler: calls `BackendCoordinator::AcquireSlot(model_name)`
4. **Layer 4** BackendCoordinator: ensures model loaded, returns free slot
5. **Layer 6** Engine: tokenizes prompt, checks LCP against slot's cache
6. **Layer 3** Sampling: builds sampler chain from model's profile
7. **llama.cpp** does the inference
8. **Layer 7** Observability: tracks t/s, tokens, swaps
9. **Layer 8** streams SSE back to client

When swap is requested:

1. User clicks dashboard or `POST /v1/swap/to/qwen3-coder-next`
2. **Layer 4** BackendCoordinator: drains active requests (30s timeout)
3. **Layer 4** unloads current model (destroys contexts, frees VRAM)
4. **Layer 4** reads new GGUF from disk
5. **Layer 4** initializes llama_model + n_parallel contexts
6. **Layer 7** logs swap event to SQLite
7. WebSocket broadcasts `ready`

## Critical Bugs to Avoid (Current Code)

These are the 4 bugs being fixed in P0-P6. **Do not reintroduce them** if
working in the legacy `LlamaEngine.cpp` or `ChatCompletions.cpp` files.

| File:line | Bug |
|---|---|
| `libs/llama_cpp_wrapper/src/LlamaEngine.cpp:444-554` | `reuse_cache` uses message count, not LCP. **Fix:** use `server_prompt_cache::load` algorithm. |
| `libs/llama_cpp_wrapper/src/LlamaEngine.cpp:170-199` | Hand-rolled sampler chain fights Qwen3 defaults. **Fix:** use `common_sampler_init` from `libs/third_party/llama.cpp/common/sampling.cpp:187`. |
| `apps/gateway-service/src/.../ChatCompletions.cpp:2175-2188` | O(n²) streaming sanitizer. **Fix:** cursor-based, only scan new tokens. |
| `libs/llama_cpp_wrapper/src/LlamaEngine.cpp:126` | `add_bos=true` hardcoded. **Fix:** use `llama_vocab_get_add_bos`. |

## Don'ts

- **NO subprocess.** Don't call `llama-server.exe`. Don't `system()`.
  Don't `popen()`. The user is on Windows and orphan processes are a problem.
- **NO proxy design.** Don't make InferDeck a thin wrapper around
  `llama-server.exe`. Use llama.cpp library directly.
- **NO MMProj load on demand.** mmproj stays loaded permanently for Qwen3.6-27B
  (vision is a stated use case).
- **NO auto-swap on OAI request.** Returns 503 with hint, user must click
  dashboard. The user explicitly chose this simpler design.
- **NO MTP for v1.** Conflicts with n_parallel>1. Revisit in P2 future.
- **NO comments unless asked.** Code style per system prompt.

## Common Tasks

### Add a new model

1. Download GGUF to `C:/Users/david/Documents/00_Models/{name}-GGUF/`
2. Add entry to `config/gateway.yml`:
   ```yaml
   model_registry:
     - name: "new-model"
       gguf_path: "C:/.../new-model-Q4_K_M.gguf"
       mmproj_path: null  # or "C:/.../mmproj-BF16.gguf" for vision
       family: "qwen3.6"
       n_slots: 2
       vram_required_mb: 22000
   ```
3. Add sampler profile to `config/sampler-profiles/{name}-coding.yaml`
4. Restart InferDeck — new model appears in `/v1/models`
5. Run parity test: `bash tests/parity/run.sh new-model`
6. (Optional) Run optimization: click "Run Optimization" in dashboard

### Run an optimization

```bash
# CLI
./build/Release/inferdeck-bench run \
  --model qwen3.6-27b \
  --profile coding \
  --suite humaneval,mbpp,bfcl \
  --trials 30 \
  --output ~/.inferdeck/optimization.db

# Dashboard
# Click "Run Optimization" -> select model + profile -> background job
# Results in ~/.inferdeck/optimization.db, view in dashboard
```

### Test a swap manually

```bash
# Check current model
curl http://localhost:11434/v1/models | jq

# Trigger swap (background, returns 202)
curl -X POST http://localhost:11434/v1/swap/to/qwen3-coder-next

# Poll status
curl http://localhost:11434/v1/swap/status | jq

# Once ready, test inference
curl -X POST http://localhost:11434/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{"model":"qwen3-coder-next","messages":[{"role":"user","content":"hello"}]}'
```

### Add a new benchmark

1. Implement `Benchmark` interface in
   `apps/benchmark-runner/src/benchmarks/{name}.cpp`:
   ```cpp
   class MyBench : public Benchmark {
     std::vector<Problem> load_problems() override;
     double score(const std::string& output, const Problem& p) override;
   };
   ```
2. Register in `apps/benchmark-runner/src/main.cpp`
3. Add to `config/bench-search-spaces.yaml` if searchable
4. Test: `./inferdeck-bench run --suite my-bench --model qwen3.6-27b`

## Observability Points

- All HTTP requests logged with: timestamp, model, prompt_tokens,
  completion_tokens, duration_ms, t/s, status_code, slot_id
- Swap events: timestamp, from_model, to_model, duration_ms, success, error
- GPU telemetry: every 100ms via ADLX, util%, temp°C, vram_used_mb, power_w
- Lifetime counters: total_tokens_in, total_tokens_out, total_requests,
  total_swaps, uptime_s
- All persisted to `~/.inferdeck/stats.db` (SQLite)

Dashboard pulls from WebSocket `/v1/stats` (live) and `/v1/stats/history`
(aggregated from SQLite).

## Testing Strategy

| What | How | When |
|---|---|---|
| Unit | GoogleTest | Every commit, must pass |
| Integration | custom HTTP tests | Every commit, must pass |
| Parity | `tests/parity/run.sh` | Every commit, must pass per registered model |
| Swap stress | `tests/stress/swap_cycle.sh 20` | Pre-release |
| 4h session | `tests/stress/four_hour_session.sh` | Pre-release |
| Optimization | Optuna 30 trials, results in DB | Manual trigger |

## When Stuck

1. **Parity failing?** Check the bug table above first.
2. **Slow first token?** Probably LCP miss. Check `LlamaEngine.cpp:444`.
3. **Repetition in long context?** Qwen3-A3B YaRN issue. Check context size.
4. **Tool calls failing?** Check grammar sampler init in P6.
5. **Swap stuck?** Check ADLX VRAM. Probably mmproj still loaded.
6. **mmproj not found?** Verify path in `config/gateway.yml` and the GGUF
   actually has vision enabled (Unsloth Dynamic GGUFs do).

## Useful llama.cpp Functions

| Task | Function |
|---|---|
| Load model | `llama_model_load_from_file` |
| Init context | `llama_init_from_model` |
| Tokenize | `llama_tokenize` |
| BOS handling | `llama_vocab_get_add_bos(llama_model_get_vocab(model))` |
| Detokenize | `llama_token_to_piece` |
| Decode batch | `llama_decode` |
| KV cache prefix match | `llama_kv_cache_seq_rm` to trim divergent tail |
| Sampler init | `common_sampler_init` (see `common/sampling.cpp:187`) |
| Grammar lazy | `llama_sampler_init_grammar_lazy_patterns` |
| Tool call JSON | parse from sampler output (don't regex) |
| Multimodal | `mtmd_*` functions in `mtmd.h` |

## Useful Files in llama.cpp to Reference

- `libs/third_party/llama.cpp/tools/server/server-context.cpp` —
  slot pool, LCP cache, prompt eval
- `libs/third_party/llama.cpp/tools/server/server-task.h` —
  `server_prompt_cache` struct
- `libs/third_party/llama.cpp/common/sampling.cpp:187` —
  `common_sampler_init` for sampler chain
- `libs/third_party/llama.cpp/common/chat.cpp` —
  OAI chat template parsing

## Dashboard

React app in `apps/dashboard/`. WebSocket to `/v1/stats` and
`/v1/swap/status`. Build with `npm run build`, output to
`apps/gateway-service/src/static/`.

Panels:
- **Models:** current model, target model, "Switch" button
- **Live stats:** t/s, GPU util, temp, lifetime tokens
- **Optimization:** per-model history, "Run Optimization" button
- **Swap history:** timestamp, from, to, duration, success

## Release Checklist

- [ ] All unit tests pass
- [ ] All integration tests pass
- [ ] Parity >= 0.95 for all registered models
- [ ] Swap stress test passes (20 cycles, no leaks)
- [ ] 4h opencode session completes (zero OOM, zero crash)
- [ ] Optimization applied to coding profile
- [ ] Dashboard shows all stats correctly
- [ ] `PLAN.md` and `AGENTS.md` up to date
- [ ] No comments in code (per system prompt convention)
- [ ] `config/gateway.yml` final form
