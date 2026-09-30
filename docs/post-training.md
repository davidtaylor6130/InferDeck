# Managed GGUF quantisation

InferDeck can create a lower-precision GGUF model through llama.cpp without a
subprocess. This is quantisation only. Fine-tuning and LoRA adapter training are
not available because the vendored backend does not expose a supported adapter
training and save API.

## Requirements

The source model must:

- be installed and registered through InferDeck's model store;
- use the `llama_cpp` runtime;
- be one regular `.gguf` file inside the configured model-store root; and
- be unloaded when the job starts.

InferDeck accepts one quantisation job at a time. At admission, active requests,
queued requests, and model swaps must all be zero. The output model name must be
new and use the same safe-name rules as other managed models. The request cannot
provide source, staging, or destination paths.

Supported output formats are `Q4_K_M`, `Q5_K_M`, `Q6_K`, and `Q8_0`. The
`threads` value may be 0 through 64. Zero leaves thread selection to llama.cpp.
Requantisation is disabled.

## Capability discovery

```http
GET /api/inferdeck/v1/post-training/capabilities
```

The response states the supported formats, CPU compute resource, concurrency
limit, non-cancellable lifecycle, managed-source requirement, new-lease block,
and unavailable fine-tuning boundary.

## Start a job

This is a control-plane write operation. Remote callers need the configured
control credential and allowed origin. A managed client API key does not grant
access.

```http
POST /api/inferdeck/v1/post-training/quantizations
Content-Type: application/json

{
  "sourceModel": "source-model-f16",
  "outputModel": "source-model-q4-k-m",
  "quantization": "Q4_K_M",
  "threads": 8
}
```

A successful request returns HTTP 202:

```json
{
  "id": 12,
  "state": "queued",
  "cancellable": false
}
```

Unknown fields and invalid types are rejected. A missing source returns 404.
An existing output, active job, busy server, loaded source, or other maintenance
owner returns 409.

## Inspect jobs

```http
GET /api/inferdeck/v1/post-training/quantizations
```

Jobs move through `queued`, `quantizing`, then `installed` or `failed`. The
listing includes the source and output model names, selected format, thread
count, error text, output byte count, and SHA-256 digest. `outputPath` remains
empty until installation succeeds.

The final artifact is first written to a server-derived `.partial` path. It is
hashed, moved without overwrite, added to the model-store manifest, and
registered under the requested output model name. The source artifact is never
replaced.

llama.cpp does not provide progress or cancellation callbacks for this call.
InferDeck therefore does not expose fake progress or cancellation. Graceful
shutdown waits for the worker to finish.

## Scheduling and background leases

An active job owns InferDeck's CPU maintenance resource. New CPU-backed model
work and configuration changes using that resource are rejected until the job
finishes. GPU-backed inference remains eligible after admission.

When no background lease is already active,
`GET /api/inferdeck/v1/background/availability` reports `available: false` with
reason `maintenance` while quantisation runs. It also returns
`suggestedReportBackAtUnixMs` and `Retry-After`. A new lease request receives
HTTP 409 for the same reason. The maintenance reservation is released on both
successful and failed jobs.

## Real-model verification

Build `route_tests.exe` in Release first. The verifier downloads and SHA-256
checks a pinned official 15M F32 GGUF fixture when no model path is supplied.

Prepare the fixture without invoking llama.cpp:

```powershell
powershell -File Testing/Test-PostTrainingQuantization.ps1 `
  -RouteTests build/bin/Release/route_tests.exe `
  -PrepareOnly
```

Run the real Q8_0 conversion:

```powershell
powershell -File Testing/Test-PostTrainingQuantization.ps1 `
  -RouteTests build/bin/Release/route_tests.exe `
  -Output C:\tmp\inferdeck-quantization-validation.gguf
```

The verifier checks the source hash, native test result, GGUF signature, output
size, and output SHA-256. Conversion is CPU work, but the linked llama.cpp
backend can enumerate Vulkan devices during initialisation. For strict GPU
isolation on a single-machine installation, stop the existing InferDeck service
and confirm its process and listener are gone before the real conversion, then
restore and verify the service afterward. `-PrepareOnly` does not invoke the
quantizer.
