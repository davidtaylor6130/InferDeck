# Job Queue Architecture

Jobs are the fundamental unit of work in the r9700-ai-gateway.
The queue provides priority-based scheduling with GPU resource locking.

## Job Lifecycle

```
QUEUED → LEASED → RUNNING → SUCCEEDED
                    ↓
                 FAILED → (retry) QUEUED
                    ↓
                 DEAD_LETTER
```

## Job Types

- `llm_chat`: LLM chat conversation
- `llm_generate`: Text generation
- `llm_embed`: Embedding generation
- `rag`: RAG pipeline execution
- `llama_index`: Indexing/retreival
- `comfyui`: ComfyUI workflow
- `image_generation`: Image generation
- `audio_processing`: Audio processing
- `custom`: Custom job type
- `maintenance`: System maintenance

## Job Record Schema

```typescript
interface JobRecord {
  // Identity
  id: UUID;
  type: JobType;
  resourceClass: ResourceClass;
  status: JobStatus;
  
  // Scheduling
  priority: number;
  retryCount: number;
  maxRetries: number;
  
  // Execution tracking
  createdAt: Date;
  updatedAt: Date;
}

interface JobContext {
  payload: Record<string, unknown>;
  resourceClass: ResourceClass;
  status: JobStatus;
  priority: number;
}
```

## Queue Processing Flow

1. Job created → queued in QueueStore
2. Worker polls QueueStore.getNextJob()
3. get queueStore.dequeue() removes it
4. Worker acquires resources
5. ResourceLockManager.acquire() locks GPU if needed
6. Worker executes job
7. Worker updates queueStore.remove() when done
8. Worker reports success/error via callback

## GPU Locking Rules

- Only one GPU-heavy job at a time by default
- GPU-heavy types: gpu_llm, gpu_image, gpu_audio
- Non-GPU jobs can run concurrently
- Gaming mode prioritizes GPU for gaming
- Interactive LLM jobs get priority in gaming mode

## Persistence

Jobs are stored in SQLite:
- jobs table: full job records
- job_events table: event history
- job_artifacts: results and payloads
- TTL cleanup: old jobs removed automatically
