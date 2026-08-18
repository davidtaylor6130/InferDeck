# InferDeck forced-alignment service

This local sidecar serves `Qwen/Qwen3-ForcedAligner-0.6B` on port 11436. It decodes uploads with FFmpeg, preserves the source timeline, serializes inference, and returns model-derived word timestamps and timestamp-token confidence values. It never generates a transcript.

The pinned model classifies timestamps on an 80 ms grid (`timestamp_segment_time: 80`, in milliseconds). The service converts token IDs with `token_id * 80 / 1000` to seconds. Start tokens must remain strictly inside the media. At the media end only, an end-token overrun of at most one model grid step is clamped to the exact decoded WAV duration. Larger overruns, negative timestamps, zero-length spans, and non-chronological spans are rejected. Rejection logs contain only request ID, word index, raw timestamp token, converted seconds, duration, and tolerance; transcript contents are never logged. Set `ALIGNER_DEVELOPMENT_DIAGNOSTICS=true` to include the same non-text range details in HTTP 422 messages.

Qwen can emit zero-duration lexical spans after its independent hard-argmax timestamp decoding. The service preserves Qwen's upstream hard-argmax/LIS timeline wherever it is valid. If a direct pass contains an invalid span, that original full-duration timeline is used only to partition the ordered transcript units; the service cuts the unchanged normalized PCM at detected silence boundaries into windows no longer than 30 seconds and genuinely re-aligns each contiguous unit slice. Thirty seconds is the longest duration covered by the live single-window regression and avoids introducing brittle transcript boundaries every four seconds. Deterministic maximum-likelihood constrained decoding is applied only inside a window whose raw Qwen output remains invalid. Absolute offsets are restored and every unit must appear exactly once. The service does not synthesize durations for zero-length model output.

Window partitioning also rejects impossible coarse-timeline density. English and other space-delimited languages start at four transcript units per second per window; Chinese, Cantonese, Japanese, and Korean start at twelve. Excess units remain contiguous and are deterministically cascaded into earlier silence-bounded windows before model re-alignment. If the genuine model result remains below the configured confidence threshold, partitioning retries at 3.0 then 2.5 units per second for space-delimited languages, or 10 then 8 for CJK languages. The threshold is never weakened.

If the long-duration coarse pass saturates a silence window at that density limit while transcript units remain, the service treats the saturated tail as unreliable. It preserves the already aligned prefix, rebases the unchanged remaining audio to zero, reruns Qwen on that shorter tail, and recursively restores the absolute offset. This is model inference on the real tail, not a proportional or approximate timestamp distribution.

When a window remains below the confidence threshold, the service deterministically tests nearest-first bounded shifts of the shared transcript cut with either neighboring silence window. Both windows are genuinely re-aligned for every candidate, and the first cut meeting the required confidence is accepted. Each recursion level is limited to 24 candidate pairs to avoid destabilizing the pinned ROCm model with an unbounded inference search. No timestamp is interpolated or synthesized during boundary refinement.

Unmodified raw Qwen timestamps have confidence 1.0. For a repaired invalid span, timestamp confidence is the likelihood of the selected constrained token relative to Qwen's best raw token at the same timestamp position. This avoids making confidence shrink merely because the 5,000-bin distribution is diffuse or the recording contains many words, while retaining the configured rejection threshold for constrained choices that are materially worse than Qwen's preferred timestamp.

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

Run the real-speech duration regression against an existing speech recording of at least 300 seconds. The script asks the local Parakeet service for each exact clip transcript, then authenticates to the aligner and verifies 4, 8, 30, 73, and 300-second results without printing transcript contents:

```powershell
& scripts/windows/Test-ForcedAlignerRealAudio.ps1 -SourceAudio 'C:\path\to\real-speech.wav'
```
