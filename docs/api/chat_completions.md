# Chat Completions API

## Overview

Create chat completions using the loaded model. Supports both streaming and non-streaming modes.

## Endpoint

```
POST /v1/chat/completions
```

## Request

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

### Parameters

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| model | string | "default" | Model identifier |
| messages | array | required | Chat message history |
| max_tokens | int | 256 | Maximum tokens to generate |
| temperature | float | 0.7 | Sampling temperature (0.0-2.0) |
| top_p | float | 0.9 | Nucleus sampling parameter |
| stream | bool | false | Enable SSE streaming |

### Message Roles

| Role | Description |
|------|-------------|
| system | System prompt (optional, typically first) |
| user | User message |
| assistant | Assistant response |
| tool | Tool call result |

## Response (Non-Streaming)

```json
{
  "id": "chatcmpl-abc123",
  "object": "chat.completion",
  "created": 1700000000,
  "model": "default",
  "choices": [
    {
      "index": 0,
      "message": {"role": "assistant", "content": "Hello! How can I help you?"},
      "finish_reason": "stop"
    }
  ],
  "usage": {
    "prompt_tokens": 15,
    "completion_tokens": 10,
    "total_tokens": 25
  }
}
```

## Response (Streaming)

```
HTTP/1.1 200 OK
Content-Type: text/event-stream
Cache-Control: no-cache
Connection: keep-alive

data: {"id":"chatcmpl-abc123","object":"chat.completion.chunk","created":1700000000,"model":"default","choices":[{"index":0,"delta":{"role":"assistant","content":""},"finish_reason":null}]}

data: {"id":"chatcmpl-abc123","object":"chat.completion.chunk","created":1700000000,"model":"default","choices":[{"index":0,"delta":{"content":"Hello"},"finish_reason":null}]}

data: {"id":"chatcmpl-abc123","object":"chat.completion.chunk","created":1700000000,"model":"default","choices":[{"index":0,"delta":{"content":"!"},"finish_reason":null}]}

data: [DONE]
```

## Error Responses

### 400 Bad Request

```json
{
  "error": {
    "message": "Missing 'messages' field",
    "type": "invalid_request_error",
    "param": "messages",
    "code": "missing_field"
  }
}
```

### 503 Service Unavailable

```json
{
  "error": {
    "message": "Model not loaded",
    "type": "service_unavailable",
    "param": null,
    "code": "model_not_loaded"
  }
}
```
