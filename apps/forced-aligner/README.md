# InferDeck forced-alignment service

This local sidecar serves `Qwen/Qwen3-ForcedAligner-0.6B` on port 11436. It decodes uploads with FFmpeg, preserves the source timeline, serializes inference, and returns model-derived word timestamps and timestamp-token confidence values. It never generates a transcript.

The model snapshot is pinned to `c7cbfc2048c462b0d63a45797104fc9db3ad62b7`. Download it before enabling offline mode:

```powershell
$env:HF_HOME = 'C:\InferDeck\models\cache'
$env:HF_HUB_CACHE = 'C:\InferDeck\models\cache'
huggingface-cli download Qwen/Qwen3-ForcedAligner-0.6B --revision c7cbfc2048c462b0d63a45797104fc9db3ad62b7
```

Start the service from its Python 3.12 environment:

```powershell
$env:HF_HOME = 'C:\InferDeck\models\cache'
$env:HF_HUB_CACHE = 'C:\InferDeck\models\cache'
$env:HF_HUB_OFFLINE = '1'
$env:TRANSFORMERS_OFFLINE = '1'
$env:ALIGNER_DEVICE = 'rocm'
$env:ALIGNER_CPU_FALLBACK = 'true'
$env:ALIGNER_MIN_GPU_FREE_MB = '3072'
$env:ALIGNER_AUTH_TOKEN_FILE = 'C:\InferDeck\config\forced-aligner-token.txt'
$env:ALIGNER_HOST = '192.168.0.168'
$env:ALIGNER_PORT = '11436'
inferdeck-forced-aligner
```

Editor configuration:

```text
AI_FORCED_ALIGNER_URL=http://ai.homelab.com:11436/v1/audio/alignments
AI_FORCED_ALIGNER_MODEL=Qwen/Qwen3-ForcedAligner-0.6B
```

Smoke test:

```bash
curl -X POST \
  -F "file=@sample.wav" \
  -F "text=This is a test of exact word alignment." \
  -F "language=en" \
  http://localhost:11436/v1/audio/alignments
```

Set `ALIGNER_AUTH_TOKEN` to require `Authorization: Bearer <token>`. The service only loads a local cached model snapshot and can run with outbound networking disabled after installation.

The deployed native service binds only to `192.168.0.168`. Its firewall rule permits TCP 11436 only from the editor at `192.168.0.172`. The Bearer token is loaded from `C:\InferDeck\config\forced-aligner-token.txt`, which is not stored in Git. Startup is rejected if a non-loopback bind has no token.

The Docker image binds its container port on all interfaces and therefore requires `ALIGNER_AUTH_TOKEN` to be supplied at runtime. Mount the persistent cache at `/models/cache`.

The live Windows deployment uses `Start-ForcedAligner.ps1` plus a hidden per-user startup watchdog. `Install-ForcedAlignerService.ps1` is the administrator-only NSSM alternative.

Run `scripts/windows/Enable-ForcedAlignerRemote.ps1` from an elevated PowerShell window to install the host-specific listener and the firewall rule restricted to `192.168.0.172`.
