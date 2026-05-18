# Models API

## Overview

List available models for inference. Returns the currently loaded model.

## Endpoint

```
GET /v1/models
```

## Request

No request body required.

## Response

```json
{
  "object": "list",
  "data": [
    {
      "id": "llama-2-7b.Q4_K_M.gguf",
      "object": "model",
      "created": 1700000000,
      "owned_by": "inferdeck"
    }
  ]
}
```

### Model Fields

| Field | Type | Description |
|-------|------|-------------|
| id | string | Model identifier (filename or alias) |
| object | string | Always "model" |
| created | int | Unix timestamp when model was loaded |
| owned_by | string | Always "inferdeck" |

## Error Responses

### 500 Internal Server Error

```json
{
  "error": {
    "message": "No models loaded",
    "type": "server_error",
    "param": null,
    "code": "no_models"
  }
}
```
