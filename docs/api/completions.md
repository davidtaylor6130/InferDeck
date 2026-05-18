# Completions API

## Overview

Create text completions from a prompt. Supports both streaming and non-streaming modes.

## Endpoint

```
POST /v1/completions
```

## Request

```json
{
  "model": "default",
  "prompt": "Once upon a time",
  "max_tokens": 100,
  "temperature": 0.7,
  "top_p": 0.9,
  "stream": false
}
```

### Parameters

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| model | string | "default" | Model identifier |
| prompt | string | required | Text to complete |
| max_tokens | int | 256 | Maximum tokens to generate |
| temperature | float | 0.7 | Sampling temperature (0.0-2.0) |
| top_p | float | 0.9 | Nucleus sampling parameter |
| stream | bool | false | Enable SSE streaming |

## Response (Non-Streaming)

```json
{
  "id": "cmpl-abc123",
  "object": "text_completion",
  "created": 1700000000,
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

## Response (Streaming)

```
HTTP/1.1 200 OK
Content-Type: text/event-stream
Cache-Control: no-cache
Connection: keep-alive

data: {"id":"cmpl-abc123","object":"text_completion.chunk","created":1700000000,"model":"default","choices":[{"index":0,"delta":{"content":"Hello"},"finish_reason":null}]}

data: [DONE]
```

## Error Responses

### 400 Bad Request

```json
{
  "error": {
    "message": "Missing 'prompt' field",
    "type": "invalid_request_error",
    "param": "prompt",
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
