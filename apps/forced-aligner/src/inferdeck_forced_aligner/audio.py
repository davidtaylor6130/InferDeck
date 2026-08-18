import subprocess
import wave
from dataclasses import dataclass
from pathlib import Path

from fastapi import UploadFile
from imageio_ffmpeg import get_ffmpeg_exe

from .errors import AlignmentError


SUPPORTED_EXTENSIONS = {
    ".wav", ".mp3", ".m4a", ".flac", ".aac", ".ogg", ".wma",
    ".mp4", ".mov", ".mkv", ".webm",
}


@dataclass(frozen=True)
class PreparedAudio:
    path: Path
    duration: float


class AudioPreparer:
    def __init__(self, max_seconds: float, max_upload_bytes: int):
        self.max_seconds = max_seconds
        self.max_upload_bytes = max_upload_bytes

    def prepare(self, upload: UploadFile, workdir: Path) -> PreparedAudio:
        suffix = Path(upload.filename or "").suffix.lower()
        if suffix not in SUPPORTED_EXTENSIONS:
            raise AlignmentError(415, "UNSUPPORTED_AUDIO", "Unsupported audio format")
        source = workdir / f"source{suffix}"
        size = 0
        with source.open("wb") as destination:
            while chunk := upload.file.read(1024 * 1024):
                size += len(chunk)
                if size > self.max_upload_bytes:
                    raise AlignmentError(413, "AUDIO_TOO_LARGE", "Uploaded audio exceeds the configured size limit")
                destination.write(chunk)
        if size == 0:
            raise AlignmentError(400, "MISSING_AUDIO", "Uploaded audio is empty")
        output = workdir / "normalized.wav"
        command = [
            get_ffmpeg_exe(), "-hide_banner", "-loglevel", "error", "-nostdin",
            "-i", str(source), "-map_metadata", "-1", "-vn", "-ac", "1", "-ar", "16000",
            "-c:a", "pcm_s16le", "-y", str(output),
        ]
        try:
            completed = subprocess.run(command, capture_output=True, text=True, timeout=180, check=False)
        except (OSError, subprocess.TimeoutExpired) as exc:
            raise AlignmentError(500, "AUDIO_DECODE_FAILED", f"FFmpeg could not decode the audio: {exc}") from exc
        if completed.returncode != 0 or not output.is_file():
            message = completed.stderr.strip().splitlines()[-1] if completed.stderr.strip() else "unknown decode error"
            raise AlignmentError(415, "UNSUPPORTED_AUDIO", f"FFmpeg could not decode the audio: {message}")
        try:
            with wave.open(str(output), "rb") as wav:
                duration = wav.getnframes() / float(wav.getframerate())
        except (OSError, wave.Error) as exc:
            raise AlignmentError(500, "AUDIO_DECODE_FAILED", "Normalized audio is not a valid PCM WAV") from exc
        if duration <= 0:
            raise AlignmentError(400, "MISSING_AUDIO", "Uploaded audio has no decodable samples")
        if duration > self.max_seconds + (1.0 / 16000.0):
            raise AlignmentError(413, "AUDIO_TOO_LONG", f"Audio exceeds the configured maximum of {self.max_seconds:g} seconds")
        return PreparedAudio(output, duration)
