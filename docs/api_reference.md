# InferDeck Gateway API Reference

## Overview

The InferDeck Gateway provides a strict OpenAI-compatible API for local LLM inference. All endpoints return JSON responses following OpenAI's schema.

## Base URL

```
http://localhost:8080    # HTTP
https://localhost:8080   # HTTPS (default)
```

## Endpoints

### `/v1/chat/completions`

Create a chat completion.

**POST** `/v1/chat/completions`

#### Request Body

```json
{
  "model": "default",
  "messages": [
    {"role": "system", "content": "You are a helpful assistant."},
    {"role": "user", "content": "Hello!"}
  ],
  "max_tokens": 256,
  "temperature": 0.7,
  "top_p": 0.9,
  "stream": false
}
```

#### Response (200 OK)

```json
{
  "id": "chatcmpl-123",
  "object": "chat.completion",
  "created": 1234567890,
  "model": "default",
  "choices": [
    {
      "index": 0,
      "message": {
        "role": "assistant",
        "content": "Hello! How can I help you?"
      },
      "finish_reason": "stop"
    }
  ],
  "usage": {
    "prompt_tokens": 10,
    "completion_tokens": 5,
    "total_tokens": 15
  }
}
```

#### Streaming Response

When `stream: true`, returns SSE format:

```
data: {"id":"chatcmpl-123","object":"chat.completion.chunk","created":1234567890,"model":"default","choices":[{"index":0,"delta":{"role":"assistant"},"finish_reason":null}]}

data: {"id":"chatcmpl-123","object":"chat.completion.chunk","created":1234567890,"model":"default","choices":[{"index":0,"delta":{"content":"Hello"},"finish_reason":null}]}

data: [DONE]
```

### `/v1/completions`

Create a text completion.

**POST** `/v1/completions`

#### Request Body

```json
{
  "model": "default",
  "prompt": "Once upon a time",
  "max_tokens": 100,
  "temperature": 0.7
}
```

#### Response (200 OK)

```json
{
  "id": "cmpl-123",
  "object": "text_completion",
  "created": 1234567890,
  "model": "default",
  "choices": [
    {
      "text": ", there was a cat...",
      "index": 0,
      "finish_reason": "stop"
    }
  ],
  "usage": {
    "prompt_tokens": 5,
    "completion_tokens": 10,
    "total_tokens": 15
  }
}
```

### `/v1/models`

List available models.

**GET** `/v1/models`

#### Response (200 OK)

```json
{
  "object": "list",
  "data": [
    {
      "id": "llama-2-7b.Q4_K_M.gguf",
      "object": "model",
      "created": 1234567890,
      "owned_by": "inferdeck"
    }
  ]
}
```

### `/v1/embeddings`

Create embeddings.

**POST** `/v1/embeddings`

#### Request Body

```json
{
  "model": "default",
  "input": "Hello, world!"
}
```

#### Response (200 OK)

```json
{
  "object": "list",
  "data": [
    {
      "object": "embedding",
      "index": 0,
      "embedding": [0.1, 0.2, 0.3, ...]
    }
  ],
  "model": "default",
  "usage": {
    "prompt_tokens": 3,
    "total_tokens": 3
  }
}
```

### `/v1/health`

Health check endpoint.

**GET** `/v1/health`

#### Response (200 OK)

```json
{
  "status": "healthy",
  "version": "0.1.0",
  "uptime": 3600
}
```

### `/inferdeck/metrics`

Get inference metrics.

**GET** `/inferdeck/metrics`

#### Response (200 OK)

```json
{
  "counters": {
    "inferdeck.requests.total": 100,
    "inferdeck.requests.success": 95
  },
  "gauges": {
    "gpu.vram_used": 8589934592,
    "queue.pending": 2
  },
  "histograms": {
    "inferdeck.latency_ms": {
      "min": 10.5,
      "max": 500.2,
      "avg": 45.3,
      "count": 100,
      "sum": 4530.0
    }
  }
}
```

### `/inferdeck/status`

Get inference engine status.

**GET** `/inferdeck/status`

#### Response (200 OK)

```json
{
  "initialized": true,
  "model": "llama-2-7b.Q4_K_M.gguf",
  "gpu": {
    "device_name": "AMD Radeon RX 6800",
    "vram_total": 16106127360,
    "vram_used": 8589934592
  },
  "quantization": "Q4_K_M",
  "precision": "mixed"
}
```

## Error Responses

All errors follow OpenAI schema:

```json
{
  "error": {
    "message": "Error description",
    "type": "invalid_request_error",
    "param": null,
    "code": null
  }
}
```

### Error Codes

| Code | Type | Description |
|------|------|-------------|
| 400 | `invalid_request_error` | Bad request, missing fields |
| 404 | `not_found_error` | Endpoint not found |
| 500 | `server_error` | Internal server error |
| 503 | `service_unavailable` | Model not loaded |
