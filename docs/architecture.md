# System Architecture

## Overview

InferDeck is a production-ready local AI gateway designed for single-GPU machines.
It provides intelligent GPU scheduling, mode switching, and API abstraction for local AI workloads.

## Architecture Diagram

```
┌─────────────┐
│  Browser    │
│  Dashboard  │
└──────┬──────┘
       │ HTTP
┌──────▼──────┐
│   Fastify   │
│   Server    │
└──────┬──────┘
       │
┌──────▼──────┐     ┌──────────────┐
│ Gateway     │────▶│ llama.cpp    │
│ Proxy       │     │ Backend      │
└──────┬──────┘     └──────────────┘
       │
┌──────▼──────┐     ┌──────────────┐
│ Scheduler   │────▶│ GPU Resource │
│ & Queue     │     │ Manager      │
└──────┬──────┘     └──────────────┘
       │
┌──────▼──────┐
│ SQLite DB   │
└─────────────┘
```

## Key Components

### Gateway Service
- Fastify HTTP server with middleware stack
- Handles both dashboard serving and API proxying
- Provides llama.cpp API compatibility and OpenAI API proxy
- Database-backed job queue with SQLite

### Scheduler Module
- Priority-based job scheduling
- GPU resource locking and conflict prevention
- Mode-aware scheduling (AI/Gaming/Maintenance)
- Interactive hold-and-run for responsive LLM chat

### Resource Lock Manager
- GPU resource allocation and deallocation
- Concurrent job prevention (one GPU-heavy job at a time)
- Gaming mode GPU reservation
- Lease-based resource management

### Dashboard
- React SPA with Tailwind CSS
- Real-time health monitoring
- Job queue management
- Mode switching UI

## Data Flow

1. HTTP request arrives at Gateway Proxy port
2. Auth middleware validates API key (if enabled)
3. Route handler processes request:
   - Dashboard requests: served from static files
   - API requests: routed to appropriate handler
   - Proxy requests: forwarded to llama.cpp backend
4. Scheduler determines if job can run immediately
5. Resource locks prevent GPU contention
6. Results returned to client or streamed back

## Configuration Flow

```
gateway.local.yaml
    ↓
loadConfig() → GatewayConfig
    ↓
AppContext (config, queueStore, lockManager)
    ↓
startServer() → Fastify app
```

## Mode Switching

### AI Mode
- Default mode
- GPU jobs scheduled normally
- Interactive LLM chat available

### Gaming Mode
- GPU reserved for gaming
- Interactive LLM can run if GPU free
- Background jobs paused

### Maintenance Mode
- All jobs paused
- GPU available for maintenance tasks
- Interactive LLM disabled

## Security Model

- API key required (configurable)
- CORS for LAN access
- SQLite with WAL mode for concurrency
- Environment variable-based secrets (never in config)
