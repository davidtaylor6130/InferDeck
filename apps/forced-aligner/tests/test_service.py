import sys
import types
import wave
from pathlib import Path

import pytest
from fastapi.testclient import TestClient

from inferdeck_forced_aligner.app import create_app
from inferdeck_forced_aligner.audio import AudioPreparer, PreparedAudio
from inferdeck_forced_aligner.backend import QwenBackend
from inferdeck_forced_aligner.config import Settings
from inferdeck_forced_aligner.errors import AlignmentError
from inferdeck_forced_aligner.words import AlignedUnit, aggregate_units_to_words, transcript_words


class FakePreparer:
    def __init__(self, duration: float = 4.0):
        self.duration = duration

    def prepare(self, upload, workdir: Path) -> PreparedAudio:
        output = workdir / "normalized.wav"
        output.write_bytes(b"RIFFfake")
        return PreparedAudio(output, self.duration)


class FakeBackend:
    loaded = True
    backend_name = "cpu"

    def load(self):
        self.loaded = True

    def align(self, audio: Path, text: str, language: str):
        words = transcript_words(text)
        width = 3.0 / max(len(words), 1)
        return [AlignedUnit(word, 0.5 + index * width, 0.5 + (index + 0.8) * width, 0.95) for index, word in enumerate(words)]


class FailingBackend(FakeBackend):
    def align(self, audio: Path, text: str, language: str):
        raise AlignmentError(422, "ALIGNMENT_FAILED", "The transcript cannot be aligned reliably")


def settings(temp_root: Path, **changes) -> Settings:
    values = {
        "model": "Qwen/Qwen3-ForcedAligner-0.6B",
        "revision": "c7cbfc2048c462b0d63a45797104fc9db3ad62b7",
        "device": "rocm",
        "cpu_fallback": True,
        "max_audio_seconds": 300,
        "auth_token": "",
        "hf_home": temp_root / "cache",
        "host": "127.0.0.1",
        "port": 11436,
        "temp_root": temp_root,
        "min_word_confidence": 0.01,
        "min_gpu_free_mb": 3072,
        "max_upload_bytes": 1024 * 1024,
    }
    values.update(changes)
    return Settings(**values)


def client_for(temp_root: Path, backend=None, duration=4.0):
    app = create_app(settings(temp_root), backend or FakeBackend())
    app.state.audio_preparer = FakePreparer(duration)
    return TestClient(app)


def post(client: TestClient, text: str, field: str = "file"):
    return client.post(
        "/v1/audio/alignments",
        files={field: ("sample.wav", b"audio", "audio/wav")},
        data={"text": text, "language": "en"},
    )


def test_short_english_sentence_and_health(tmp_path):
    with client_for(tmp_path) as client:
        health = client.get("/health").json()
        response = post(client, "This is a short sentence.")
    assert health == {
        "ok": True,
        "model": "Qwen/Qwen3-ForcedAligner-0.6B",
        "device": "cpu",
        "modelLoaded": True,
    }
    assert response.status_code == 200
    payload = response.json()
    assert [word["word"] for word in payload["words"]] == ["This", "is", "a", "short", "sentence"]
    assert payload["alignment"] == {"status": "aligned", "approximate": False}


def test_punctuation_contractions_and_audio_alias(tmp_path):
    with client_for(tmp_path) as client:
        response = post(client, "I'm ready, and I don't wait!", field="audio")
    assert response.status_code == 200
    assert [word["word"] for word in response.json()["words"]] == ["I'm", "ready", "and", "I", "don't", "wait"]


def test_repeated_words_are_not_deduplicated(tmp_path):
    with client_for(tmp_path) as client:
        response = post(client, "go go go now")
    assert [word["word"] for word in response.json()["words"]] == ["go", "go", "go", "now"]


def test_leading_and_trailing_silence_is_preserved(tmp_path):
    class SilenceBackend(FakeBackend):
        def align(self, audio, text, language):
            return [AlignedUnit("hello", 1.25, 2.0, 0.9)]

    with client_for(tmp_path, SilenceBackend(), duration=4.0) as client:
        response = post(client, "hello")
    assert response.json()["duration"] == 4.0
    assert response.json()["words"][0]["start"] == 1.25
    assert response.json()["words"][0]["end"] == 2.0


def test_300_second_recording_is_accepted(tmp_path):
    with client_for(tmp_path, duration=300.0) as client:
        response = post(client, "the final supported window")
    assert response.status_code == 200
    assert response.json()["duration"] == 300.0


@pytest.mark.parametrize("text", ["", "   "])
def test_empty_transcript_rejected(tmp_path, text):
    with client_for(tmp_path) as client:
        response = post(client, text)
    assert response.status_code == 400
    assert response.json()["error"]["code"] == "EMPTY_TRANSCRIPT"


def test_unsupported_audio_rejected(tmp_path):
    app = create_app(settings(tmp_path), FakeBackend())
    app.state.audio_preparer = AudioPreparer(300, 1024 * 1024)
    with TestClient(app) as client:
        response = client.post(
            "/v1/audio/alignments",
            files={"file": ("sample.txt", b"not audio", "text/plain")},
            data={"text": "hello", "language": "en"},
        )
    assert response.status_code == 415
    assert response.json()["error"]["code"] == "UNSUPPORTED_AUDIO"


@pytest.mark.parametrize(
    "units",
    [
        [AlignedUnit("one", 1.0, 0.9, 0.9)],
        [AlignedUnit("one", -0.1, 0.5, 0.9)],
        [AlignedUnit("one", 0.1, 4.1, 0.9)],
    ],
)
def test_invalid_or_source_unbounded_timestamps_are_rejected(tmp_path, units):
    class InvalidBackend(FakeBackend):
        def align(self, audio, text, language):
            return units

    with client_for(tmp_path, InvalidBackend(), duration=4.0) as client:
        response = post(client, "one")
    assert response.status_code == 422
    assert response.json()["error"]["code"] == "ALIGNMENT_FAILED"


def test_character_timestamps_aggregate_deterministically_to_words():
    units = [
        AlignedUnit("d", 0.1, 0.2, 0.99),
        AlignedUnit("o", 0.2, 0.3, 0.98),
        AlignedUnit("n", 0.3, 0.4, 0.97),
        AlignedUnit("'", 0.4, 0.45, 0.96),
        AlignedUnit("t", 0.45, 0.6, 0.95),
        AlignedUnit("stop", 0.7, 1.0, 0.94),
    ]
    words = aggregate_units_to_words(units, "don't stop.")
    assert words == [AlignedUnit("don't", 0.1, 0.6, 0.95), AlignedUnit("stop", 0.7, 1.0, 0.94)]


def test_rocm_load_failure_falls_back_to_cpu(monkeypatch):
    calls = []

    class FakeCuda:
        @staticmethod
        def is_available():
            return True

        @staticmethod
        def empty_cache():
            return None

        @staticmethod
        def mem_get_info():
            return 8 * 1024**3, 32 * 1024**3

    fake_torch = types.SimpleNamespace(
        cuda=FakeCuda(),
        version=types.SimpleNamespace(hip="7.2.1"),
        bfloat16="bf16",
        float32="float32",
    )

    class FakeAligner:
        @classmethod
        def from_pretrained(cls, model, **kwargs):
            calls.append(kwargs["device_map"])
            if kwargs["device_map"] == "cuda:0":
                raise RuntimeError("ROCm allocation failed")
            return object()

    monkeypatch.setitem(sys.modules, "torch", fake_torch)
    monkeypatch.setitem(sys.modules, "qwen_asr", types.SimpleNamespace(Qwen3ForcedAligner=FakeAligner))
    backend = QwenBackend("model", "revision", "rocm", True)
    backend.load()
    assert calls == ["cuda:0", "cpu"]
    assert backend.backend_name == "cpu"
    assert backend.loaded is True


@pytest.mark.parametrize("backend", [FakeBackend(), FailingBackend()])
def test_no_temporary_files_remain_after_success_or_failure(tmp_path, backend):
    with client_for(tmp_path, backend) as client:
        post(client, "cleanup test")
    assert list(tmp_path.iterdir()) == []


def test_options_preflight(tmp_path):
    with client_for(tmp_path) as client:
        response = client.options(
            "/v1/audio/alignments",
            headers={"Origin": "http://editor.local", "Access-Control-Request-Method": "POST"},
        )
    assert response.status_code in {200, 204}
    assert response.headers["access-control-allow-origin"] == "*"


def test_non_loopback_bind_requires_authentication(tmp_path):
    with pytest.raises(RuntimeError, match="ALIGNER_AUTH_TOKEN"):
        create_app(settings(tmp_path, host="0.0.0.0"), FakeBackend())


def test_unsupported_language_rejected(tmp_path):
    with client_for(tmp_path) as client:
        response = client.post(
            "/v1/audio/alignments",
            files={"file": ("sample.wav", b"audio", "audio/wav")},
            data={"text": "hello", "language": "xx"},
        )
    assert response.status_code == 400
    assert response.json()["error"]["code"] == "INVALID_LANGUAGE"
