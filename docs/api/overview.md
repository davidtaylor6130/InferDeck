# API Reference

## Authentication

Most API endpoints require authentication via the `X-API-Key` header. The key value is read from the environment variable configured in `security.apiKeyEnv` (default: `R9700_GATEWAY_ADMIN_KEY`).

---

## Health Check

**GET** `/health`

Public endpoint. No authentication required.

**Response:**
```json
{
  "status": "healthy",
  "uptime": 3600,
  "version": "1.0.0",
  "timestamp": "2025-01-01T00:00:00.000Z",
  "services": {
    "ollama": "127.0.0.1:11435"
  }
}
```

---

## Status

**GET** `/status`

Public endpoint. No authentication required.

**Response:**
```json
{
  "health": { ... },
  "mode": {
    "mode": "ai",
    "enabledAt": "2025-01-01T00:00:00.000Z",
    "details": {}
  },
  "queue": {
    "totalQueued": 0,
    "totalRunning": 0,
    "totalLeased": 0,
    "totalPaused": 0,
    "totalDeadLetter": 0,
    "totalFailed": 0,
    "gpuLocked": false,
    "lockedBy": null,
    "estimatedWaitMs": 0
  },
  "modeConfig": {
    "mode": "ai",
    "rejectInteractiveLlm": true,
    "pauseBackgroundJobs": true,
    "unloadOllamaModels": true,
    "stopComfyUi": true
  },
  "uptimeMs": 3600000
}
```

---

## Jobs

### List Jobs

**GET** `/jobs`

**Query Params:**
- `page` (int, default: 1)
- `pageSize` (int, default: 50)
- `status` (string, optional)

**Response:**
```json
{
  "jobs": [
    {
      "id": "550e8400-e29b-41d4-a716-446655440000",
      "type": "llm_chat",
      "status": "queued",
      "priority": 70,
      "resourceClass": "gpu_llm",
      "clientName": "opencode",
      "requestPath": "/v1/chat/completions",
      "requestMethod": "POST",
      "payload": {},
      "createdAt": "2025-01-01T00:00:00.000Z",
      "updatedAt": "2025-01-01T00:00:00.000Z"
    }
  ],
  "total": 1,
  "page": 1,
  "pageSize": 50
}
```

### Get Job by ID

**GET** `/jobs/:id`

**Response:** Full `JobRecord` object.

### Create Job

**POST** `/jobs`

**Body:**
```json
{
  "type": "llm_chat",
  "payload": { "messages": [] },
  "priority": 70,
  "resourceClass": "gpu_llm",
  "idempotencyKey": "optional-key"
}
```

**Response:**
```json
{
  "jobId": "550e8400-e29b-41d4-a716-446655440000",
  "status": "queued",
  "position": 0
}
```

### Cancel Job

**POST** `/jobs/:id/cancel`

### Change Priority

**POST** `/jobs/:id/reprioritize`

**Body:** `{"priority": 80}`

---

## Models

### List Models

**GET** `/models`

**Response:**
```json
{
  "models": [
    {
      "name": "llama3",
      "size": 4700000000,
      "digest": "sha256:abc123",
      "details": {
        "parent_model": "",
        "format": "gguf",
        "family": "llama",
        "families": ["llama"],
        "parameter_size": "8B",
        "quantization_level": "Q4_K_M"
      }
    }
  ],
  "backends": {
    "ollama": "http://127.0.0.1:11435"
  }
}
```

### List Running Models

**GET** `/models/running`

**Response:**
```json
{
  "running": [
    {
      "name": "llama3",
      "size": 4700000000,
      "size_vram": 4700000000,
      "ttl": 300
    }
  ]
}
```

### Pull Model

**POST** `/models/pull`

**Body:** `{"name": "llama3"}`

### Load Model

**POST** `/models/load`

**Body:** `{"model": "llama3", "keep_alive": "5m"}`

### Unload Model

**POST** `/models/unload`

**Body:** `{"model": "llama3"}`

### Delete Model

**DELETE** `/models/:name`

---

## Services

### List Services

**GET** `/services`

**Response:**
```json
{
  "services": [
    {
      "id": "gateway",
      "name": "r9700-AI-Gateway",
      "kind": "gateway",
      "status": "running",
      "version": "1.0.0"
    },
    {
      "id": "ollama",
      "name": "Ollama",
      "kind": "ollama",
      "status": "running",
      "baseUrl": "http://127.0.0.1:11435"
    }
  ]
}
```

### Ollama Health

**GET** `/services/ollama/health`

**Response:**
```json
{
  "healthy": true,
  "backend": "http://127.0.0.1:11435",
  "modelCount": 1
}
```

---

## Modes

### Switch Mode

**POST** `/modes/:mode`

**Path:** one of `ai`, `gaming`, `maintenance`

**Response:**
```json
{
  "mode": "ai",
  "message": "Mode switched to AI"
}
```

---

## Metrics

**GET** `/metrics`

**Response:**
```json
{
  "samples": [],
  "summary": {
    "queueLengthHistory": [0, 0, 1],
    "gpuUtilizationAvg": null,
    "requestsLastHour": 0,
    "errorsLastHour": 0,
    "avgJobDuration": null
  }
}
```

---

## SSE Events

**GET** `/events/stream`

Open a Server-Sent Events connection. Events will be pushed in real time.

**Event Types:**
- `connected` — Initial connection confirmation
- `queue:changed` — Queue length changed
- `job:created` — New job submitted
- `job:updated` — Job status changed
- `job:cancelled` — Job was cancelled
- `job:error` — Job failed
- `mode:changed` — Gateway mode switched
- `model:loaded` — Model was loaded
- `model:unloaded` — Model was unloaded
- `service:health` — Service health check
- `hardware:update` — Hardware metrics update
