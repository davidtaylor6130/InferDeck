# Background availability and leases

InferDeck exposes a managed-key API for low-priority applications that should
run only after interactive work has stopped. The API reports current
availability and grants one global, time-limited lease.

A lease coordinates background clients. It does not block interactive InferDeck
requests. Give background clients a lower managed-key priority so foreground
requests remain ahead in the inference queue.

## Configure the quiet period

For a source checkout, edit:

```text
config/gateway.yml
```

For the standard installed runtime, the corresponding file is:

```text
C:\InferDeck\config\gateway.yml
```

Set the number of inactive seconds required before a new lease can be granted:

```yaml
gateway:
  background_idle_after_seconds: 900
```

The accepted range is 60 to 86400 seconds. The default is 900 seconds.
InferDeck counts from the most recent request, model swap, or gateway start. It
also refuses a new lease while requests are active or queued, a swap is active,
maintenance is using compute, or request history is unavailable.

Restart or reload the gateway after changing the configuration.

## Create a client key

Create a managed key as described in [Managed API keys](api-keys.md). Save the
one-time plaintext value in the background application's secret storage. A
priority such as `-40` keeps the client behind normal interactive work.

The examples below use PowerShell:

```powershell
$leaseKey = "idk_REPLACE_WITH_THE_ONE_TIME_KEY"
$baseUrl = "http://127.0.0.1:11434"
```

Only managed API keys can call these routes. The legacy OpenAI bearer token, the
control token, dashboard cookies, and unauthenticated loopback requests are not
accepted.

## Check availability

```powershell
curl.exe "$baseUrl/api/inferdeck/v1/background/availability" `
  -H "Authorization: Bearer $leaseKey"
```

An idle response contains `"available": true` and `"reason": "idle"`. A busy
response remains HTTP 200 and contains `"available": false`, a reason, a
`Retry-After` header, and `suggestedReportBackAtUnixMs`.

If another client holds the lease, the response contains the expiry and a
jittered suggested report-back time. It does not expose the holder's key ID,
name, or lease ID.

## Acquire a lease

```powershell
curl.exe -X POST "$baseUrl/api/inferdeck/v1/background/lease" `
  -H "Authorization: Bearer $leaseKey" `
  -H "Content-Type: application/json" `
  -d '{"durationSeconds":3600}'
```

`durationSeconds` is optional. It defaults to 3600 seconds and accepts 60 to
43200 seconds.

A new lease returns HTTP 201 and a lease ID. Repeating the request with the same
managed key returns HTTP 200 with `"status": "existing"`, the same ID, and the
same expiry. This makes acquisition safe to retry after a lost response.

If another key already owns the lease, InferDeck returns HTTP 409 with:

```json
{
  "reason": "lease_active",
  "expiresAtUnixMs": 1800000060000,
  "suggestedReportBackAtUnixMs": 1800000072345
}
```

The response also includes `Retry-After`. Clients should wait until the
suggested report-back time instead of repeatedly polling.

## Renew a lease

Replace `$leaseId` with the ID returned during acquisition:

```powershell
$leaseId = "REPLACE_WITH_LEASE_ID"
curl.exe -X PATCH "$baseUrl/api/inferdeck/v1/background/lease/$leaseId" `
  -H "Authorization: Bearer $leaseKey" `
  -H "Content-Type: application/json" `
  -d '{"durationSeconds":3600}'
```

Only the owning managed key can renew the lease. Renewal sets a new expiry from
the current time. An expired, unknown, or non-owned lease returns HTTP 404.

## Release a lease

```powershell
curl.exe -X DELETE "$baseUrl/api/inferdeck/v1/background/lease/$leaseId" `
  -H "Authorization: Bearer $leaseKey"
```

Release is idempotent and returns HTTP 204. Revoking the owning managed API key
also releases its lease in the same database transaction. Active leases persist
across gateway restarts and expire automatically.

Time-of-day rules, such as work hours, lunch breaks, or early-morning windows,
belong in the background application. The InferDeck API answers the narrower
question: is the gateway currently quiet and unleased?
