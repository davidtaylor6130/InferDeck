# API Overview

## Base URL

- **HTTPS**: `https://localhost:8080` (default)
- **HTTP**: `http://localhost:8080`

## Authentication

Currently no authentication required. Rate limiting available via `api.rate_limit` in gateway.yml.

## Content Types

- Requests: `application/json`
- Responses: `application/json` (except SSE streams: `text/event-stream`)

## Rate Limits

Configured via `api.rate_limit` in gateway.yml (requests per minute). Default: 100.

## CORS

Configured via `api.cors_origins` in gateway.yml. Default: `"*"` (all origins).

## Schema Compliance

All endpoints follow OpenAI API schema exactly. This includes:
- Response structure and field names
- Error response format
- Streaming format (SSE)
- Token counting in usage fields

## Versioning

API version is included in response headers and response bodies:

```
X-InferDeck-Version: 0.1.0
```

## Error Format

All errors use the OpenAI error format:

```json
{
  "error": {
    "message": "Human-readable error description",
    "type": "error_type",
    "param": "parameter_name",
    "code": "error_code"
  }
}
```

### Error Types

| Type | HTTP Code | Description |
|------|-----------|-------------|
| invalid_request_error | 400 | Malformed request |
| not_found_error | 404 | Resource not found |
| server_error | 500 | Internal error |
| service_unavailable | 503 | Model not loaded |

## Streaming

When `stream: true` is set in the request, the response is sent as Server-Sent Events (SSE):

```
HTTP/1.1 200 OK
Content-Type: text/event-stream
Cache-Control: no-cache
Connection: keep-alive
```

Each chunk follows the OpenAI streaming format:

```
data: {"id":"chatcmpl-123","object":"chat.completion.chunk","created":1700000000,"model":"default","choices":[{"index":0,"delta":{"content":"Hello"},"finish_reason":null}]}

data: [DONE]
```

## Metrics

Real-time metrics available at `/inferdeck/metrics`:

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
