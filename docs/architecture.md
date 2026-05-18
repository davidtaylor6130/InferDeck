# InferDeck Gateway Architecture

## Overview

InferDeck Gateway is a production-grade C++ 23 application that provides a strict OpenAI-compatible API for local LLM inference. It bridges the gap between `llama.cpp` and applications expecting the OpenAI API format.

## Component Diagram

```
┌─────────────────────────────────────────────────┐
│                React Dashboard                   │
│   (apps/dashboard – unchanged, updated API      │
│    layer to /v1/... and /inferdeck/...)         │
└──────────────────────┬──────────────────────────┘
                       │ HTTPS + SSE / WebSocket
                       ▼
┌─────────────────────────────────────────────────┐
│              Gateway Service (.exe)              │
│                                                  │
│  ┌──────────┐  ┌──────────┐  ┌───────────────┐  │
│  │  HTTP/S   │  │  SSE     │  │  Config       │  │
│  │ Server    │  │ Stream   │  │  (YAML)       │  │
│  │(cpp-httpl│  │ Handler  │  │  (spdlog)     │  │
│  └──────────┘  └──────────┘  └───────────────┘  │
│                                                  │
│  ┌───────────────────────────────────────────┐   │
│  │           Job Queue & Worker Pool           │   │
│  │    (priority queue → LlamaEngine)          │   │
│  └───────────────────────────────────────────┘   │
│                                                  │
│  ┌───────────────────────────────────────────┐   │
│  │            LlamaEngine (C++)               │   │
│  │   ┌─────────────┐   ┌──────────────────┐  │   │
│  │   │ Vulkan GPU   │   │ Mixed Precision  │  │   │
│  │   │ Backend      │   │ Auto‑detect      │  │   │
│  │   └─────────────┘   └──────────────────┘  │   │
│  └───────────────────────────────────────────┘   │
└─────────────────────────────────────────────────┘
                       │
                       ▼
              GGUF Model File (local)
```

## Module Structure

### libs/core
Shared utilities used by all modules:
- **Logger**: spdlog singleton wrapper with thread-safe initialization
- **Metrics**: In-memory counters, gauges, and histograms for `/inferdeck/metrics`
- **Config**: YAML configuration loader with validation and defaults

### libs/llama_cpp_wrapper
Inference engine abstraction:
- **GGUFParser**: Reads GGUF header to detect quantization (Q4/Q8/F16/F32)
- **VulkanDevice**: Vulkan GPU enumeration, VRAM info, compute capability detection
- **LlamaEngine**: Model loading, inference parameters, mixed precision handling

### apps/gateway-service
HTTP server and route handlers:
- **Server**: cpp-httplib SSL/HTTP wrapper with graceful shutdown
- **ConfigLoader**: YAML parsing for gateway.yml
- **Routes**: OpenAI-compatible endpoint handlers
  - `/v1/chat/completions` (streaming + non-streaming)
  - `/v1/completions` (streaming + non-streaming)
  - `/v1/models`, `/v1/embeddings`, `/v1/health`
  - `/inferdeck/metrics`, `/inferdeck/status`

## Data Flow

1. Request arrives at Server via cpp-httplib
2. Route handler validates and parses the request body (nlohmann::json)
3. ChatCompletions handler converts messages to internal format
4. LlamaEngine::Predict() runs inference with Vulkan GPU acceleration
5. Result formatted as OpenAI-compatible JSON
6. Response streamed back via cpp-httplib

## Configuration

The gateway reads `config/gateway.yml` at startup:

```yaml
server:
  host: "0.0.0.0"
  port: 8080
  tls:
    enabled: true
    cert_file: "certs/server.crt"
    key_file: "certs/server.key"

model:
  path: "models/llama-2-7b.Q4_K_M.gguf"
  precision: "auto"
  n_gpu_layers: -1
  context_size: 4096

gpu:
  device_id: 0

queue:
  worker_threads: 4
  max_queue_size: 100

logging:
  level: "info"
  file: "logs/gateway.log"
```

## Precision Mapping

| GGUF Quantization | GPU Compute | Action |
|-------------------|-------------|--------|
| Q4_0, Q4_K_M | FP16 | Quantized forward pass |
| Q8_0 | FP16 | Quantized forward pass |
| F16 | FP16 | Direct forward pass |
| F32 | FP16 | Down-converted to FP16 |

Override via `model.precision` in gateway.yml: `auto`, `f32`, `f16`, `q4_0`, `q4_k`, `q8_0`.

## Error Handling

All errors follow OpenAI schema:

```json
{
  "error": {
    "message": "Model not found",
    "type": "invalid_request_error",
    "param": "model",
    "code": "model_not_found"
  }
}
```

## Security

- TLS 1.2+ with self-signed certs (generated during build)
- CORS configured via `api.cors_origins` in gateway.yml
- Request validation before inference to prevent malformed input
- Graceful shutdown via signal handlers (SIGINT, SIGTERM)

## Build System

- CMake 3.27+ with C++23 standard
- vcpkg for dependency management
- Self-signed TLS certs generated during build
- Coverage with gcov/lcov (CI future)

## Future Phases (Post-V1)

- V2: Multi-model support, dynamic model switching
- V3: Windows Service integration
- V4: WebSocket protocol (alternative to SSE)
- V5: Distributed inference across multiple GPUs
- V6: Plugin system for custom backends
- V7: WebGPU backend for browser inference
