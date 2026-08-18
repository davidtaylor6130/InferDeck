import os
from dataclasses import dataclass, field
from pathlib import Path


def _bool(name: str, default: bool) -> bool:
    value = os.getenv(name)
    return default if value is None else value.strip().lower() in {"1", "true", "yes", "on"}


def _auth_token() -> str:
    token = os.getenv("ALIGNER_AUTH_TOKEN", "").strip()
    if token:
        return token
    token_file = os.getenv("ALIGNER_AUTH_TOKEN_FILE", "").strip()
    if not token_file:
        return ""
    return Path(token_file).read_text(encoding="utf-8").strip()


@dataclass(frozen=True)
class Settings:
    model: str = os.getenv("ALIGNER_MODEL", "Qwen/Qwen3-ForcedAligner-0.6B")
    revision: str = os.getenv("ALIGNER_MODEL_REVISION", "c7cbfc2048c462b0d63a45797104fc9db3ad62b7")
    device: str = os.getenv("ALIGNER_DEVICE", "rocm").lower()
    cpu_fallback: bool = _bool("ALIGNER_CPU_FALLBACK", True)
    max_audio_seconds: float = float(os.getenv("ALIGNER_MAX_AUDIO_SECONDS", "300"))
    auth_token: str = field(default_factory=_auth_token)
    hf_home: Path = Path(os.getenv("HF_HOME", "/models/cache"))
    host: str = os.getenv("ALIGNER_HOST", "127.0.0.1")
    port: int = int(os.getenv("ALIGNER_PORT", "11436"))
    temp_root: Path | None = Path(os.environ["ALIGNER_TEMP_ROOT"]) if os.getenv("ALIGNER_TEMP_ROOT") else None
    min_word_confidence: float = float(os.getenv("ALIGNER_MIN_WORD_CONFIDENCE", "0.01"))
    min_gpu_free_mb: int = int(os.getenv("ALIGNER_MIN_GPU_FREE_MB", "3072"))
    max_upload_bytes: int = int(os.getenv("ALIGNER_MAX_UPLOAD_BYTES", str(1024 * 1024 * 1024)))
