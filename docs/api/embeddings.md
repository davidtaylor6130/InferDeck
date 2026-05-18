# Embeddings API

## Overview

Create embeddings for input text using the loaded model.

## Endpoint

```
POST /v1/embeddings
```

## Request

```json
{
  "model": "default",
  "input": "Hello, world!"
}
```

### Parameters

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| model | string | "default" | Model identifier |
| input | string or array | required | Text or list of texts to embed |

## Response

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

### Embedding Fields

| Field | Type | Description |
|-------|------|-------------|
| object | string | Always "embedding" |
| index | int | Position in input array |
| embedding | array | Float array of embedding values |

### Usage Fields

| Field | Type | Description |
|-------|------|-------------|
| prompt_tokens | int | Number of tokens in input |
| total_tokens | int | Total tokens processed |

## Error Responses

### 400 Bad Request

```json
{
  "error": {
    "message": "Missing 'input' field",
    "type": "invalid_request_error",
    "param": "input",
    "code": "missing_field"
  }
}
```

### 503 Service Unavailable

```json
{
  "error": {
    "message": "Model does not support embeddings",
    "type": "service_unavailable",
    "param": null,
    "code": "no_embeddings"
  }
}
```
