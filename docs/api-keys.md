# Managed API keys

InferDeck can create separate bearer keys for API clients. Each key has a name
and a queue priority from `-100` to `100`. Higher values run first when requests
are waiting; queue age still prevents a low-priority client from waiting
forever.

Managed keys authenticate OpenAI data-plane routes such as
`/v1/chat/completions`, `/v1/responses`, embeddings, images, and audio. They also
authenticate the [background availability and lease API](background-leases.md).
They do not grant dashboard or control-plane access.

## Storage

Set the credential database path in `gateway.yml`:

```yaml
auth:
  required: false
  token: ""
  api_keys_db: "C:/InferDeck/data/api-keys.db"
```

InferDeck stores a SHA-256 hash and non-secret metadata. It never stores the
plaintext key. Backing up the database preserves active keys, but it cannot
recover their plaintext values.

## Create a key

Key management uses the existing control-plane security rules. Direct loopback
requests work by default. Remote administration requires an authenticated
dashboard session or control token.

```powershell
curl.exe -X POST http://127.0.0.1:11434/api/inferdeck/v1/api-keys `
  -H "Content-Type: application/json" `
  -d '{"name":"overnight jobs","priority":-40}'
```

The `201` response includes the plaintext `key` once. Copy it immediately and
store it as a secret. Later list responses contain only its ID, display prefix,
name, priority, timestamps, and revocation state.

## Manage keys

```text
GET    /api/inferdeck/v1/api-keys
PATCH  /api/inferdeck/v1/api-keys/:id
DELETE /api/inferdeck/v1/api-keys/:id
```

`PATCH` accepts `name`, `priority`, or both. `DELETE` revokes the key
immediately and is safe to repeat. Revoked records remain visible in the list
for administration and cannot authenticate.

## Use a key

```powershell
curl.exe http://127.0.0.1:11434/v1/models `
  -H "Authorization: Bearer idk_REPLACE_WITH_THE_CREATED_KEY"
```

For a managed key, the stored priority is authoritative. A client cannot raise
it with a request-body `priority` field. The legacy configured `auth.token`
continues to work and keeps its existing request-priority behaviour.
