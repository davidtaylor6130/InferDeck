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
from inferdeck_forced_aligner.words import AlignedUnit, aggregate_units_to_words, transcript_words, validate_words


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
    timestamp_grid_seconds = 0.08

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


def test_one_timestamp_grid_boundary_overrun_is_clamped():
    words = [AlignedUnit("one", 3.2, 4.08, 0.9, 40, 51)]
    validated = validate_words(words, 4.0, 0.01, 0.08)
    assert validated == [AlignedUnit("one", 3.2, 4.0, 0.9, 40, 51)]


def test_more_than_one_timestamp_grid_boundary_overrun_is_rejected(caplog):
    words = [AlignedUnit("one", 3.2, 4.081, 0.9, 40, 52)]
    with pytest.raises(AlignmentError, match="source-unbounded"):
        validate_words(words, 4.0, 0.01, 0.08, "request-123")
    assert "request_id=request-123" in caplog.text
    assert "word_index=0" in caplog.text
    assert "raw_timestamp_token=52" in caplog.text
    assert "converted_seconds=4.081000" in caplog.text


def test_development_diagnostic_identifies_rejected_timestamp():
    words = [AlignedUnit("one", 3.2, 4.16, 0.9, 40, 52)]
    with pytest.raises(AlignmentError) as raised:
        validate_words(words, 4.0, 0.01, 0.08, "request-123", True)
    assert "word_index=0 field=end raw_timestamp_token=52" in raised.value.message


def test_qwen_timestamp_token_units_are_milliseconds():
    assert QwenBackend.timestamp_token_to_seconds(1, 80) == 0.08
    assert QwenBackend.timestamp_token_to_seconds(375, 80) == 30.0
    assert QwenBackend.timestamp_token_to_seconds(3750, 80) == 300.0


def test_window_partition_preserves_repeated_units_without_duplicates():
    units = [
        AlignedUnit("go", 0.2, 0.4, 0.9),
        AlignedUnit("go", 1.2, 1.4, 0.9),
        AlignedUnit("go", 4.2, 4.4, 0.9),
        AlignedUnit("now", 7.2, 7.4, 0.9),
    ]
    partitions = QwenBackend._partition_units(units, [0.0, 4.0, 8.0])
    assert partitions == [(0.0, 4.0, 0, 2), (4.0, 8.0, 2, 4)]
    indexes = [index for _, _, first, last in partitions for index in range(first, last)]
    assert indexes == [0, 1, 2, 3]


def test_window_partition_rebalances_impossible_terminal_density():
    units = [AlignedUnit(str(index), 10.0, 10.1, 0.9) for index in range(18)]
    partitions = QwenBackend._partition_units(units, [0.0, 4.0, 8.0, 12.0], 2.0)
    assert partitions == [(0.0, 4.0, 0, 2), (4.0, 8.0, 2, 10), (8.0, 12.0, 10, 18)]
    indexes = [index for _, _, first, last in partitions for index in range(first, last)]
    assert indexes == list(range(18))


def test_saturated_tail_is_rebased_before_empty_terminal_window():
    groups = [list(range(0, 4)), list(range(4, 12)), list(range(12, 14)), []]
    boundaries = [0.0, 4.0, 8.0, 12.0, 16.0]
    assert QwenBackend._saturated_tail_index(groups, boundaries, 2.0) == 1


def test_full_nonterminal_window_without_remaining_units_is_not_rebased():
    groups = [list(range(0, 4)), list(range(4, 12)), [], []]
    boundaries = [0.0, 4.0, 8.0, 12.0, 16.0]
    assert QwenBackend._saturated_tail_index(groups, boundaries, 2.0) is None


def test_saturated_tail_is_reinferred_and_offset_without_lost_units(monkeypatch, tmp_path):
    backend = QwenBackend("model", "revision", "cpu", True)
    backend.timestamp_grid_seconds = 0.08
    words = [str(index) for index in range(14)]
    coarse = [AlignedUnit(words[index], index + 0.1, index + 0.2, 1.0) for index in range(14)]
    coarse[4:12] = [AlignedUnit(words[index], 4.1 + (index - 4) * 0.1, 4.15 + (index - 4) * 0.1, 1.0) for index in range(4, 12)]
    coarse[12:] = [AlignedUnit(words[index], 8.1 + (index - 12) * 0.1, 8.15 + (index - 12) * 0.1, 1.0) for index in range(12, 14)]
    tail_coarse = [
        AlignedUnit(words[index + 4], (index // 3) * 4 + 0.1, (index // 3) * 4 + 0.2, 1.0)
        for index in range(10)
    ]

    monkeypatch.setattr(backend, "_silence_boundaries", lambda audio, maximum: [0.0, 4.0, 8.0, 12.0, 16.0] if audio.name == "audio.wav" else [0.0, 4.0, 8.0, 12.0])
    monkeypatch.setattr(backend, "_write_window", lambda audio, output, start, end: end - start)
    monkeypatch.setattr(backend, "_infer", lambda audio, selected_words, input_text, duration, repair: tail_coarse)

    def align_partitions(audio, selected_words, partitions, depth, maximum_units_per_second):
        aligned = []
        for start, end, first, last in partitions:
            step = (end - start) / (last - first + 1)
            for offset, index in enumerate(range(first, last)):
                aligned.append(AlignedUnit(selected_words[index], start + (offset + 0.25) * step, start + (offset + 0.75) * step, 1.0))
        return aligned

    monkeypatch.setattr(backend, "_align_partitions", align_partitions)
    aligned = backend._align_windows(tmp_path / "audio.wav", words, coarse, 16.0, 2.0)
    assert [unit.text for unit in aligned] == words
    assert aligned[4].start >= 4.0
    assert aligned[-1].end <= 16.0


def test_low_confidence_shared_cut_is_refined_by_model_score(monkeypatch, tmp_path):
    backend = QwenBackend("model", "revision", "cpu", True)
    words = [str(index) for index in range(10)]

    def align_partition(audio, selected_words, partition, depth, window_index):
        start, end, first, last = partition
        confidence = 1.0 if partition in {(0.0, 4.0, 0, 3), (4.0, 8.0, 3, 10)} else 0.001
        return [
            AlignedUnit(selected_words[index], start + 0.1, start + 0.2, confidence)
            for index in range(first, last)
        ]

    monkeypatch.setattr(backend, "_align_partition", align_partition)
    aligned = backend._align_partitions(
        tmp_path / "audio.wav",
        words,
        [(0.0, 4.0, 0, 5), (4.0, 8.0, 5, 10)],
        0,
        4.0,
    )
    assert [unit.text for unit in aligned] == words
    assert min(unit.confidence for unit in aligned) == 1.0


def test_invalid_direct_alignment_requires_windowing():
    valid = [AlignedUnit("one", 0.2, 0.4, 0.9), AlignedUnit("two", 0.4, 0.8, 0.9)]
    zero = [AlignedUnit("one", 0.2, 0.2, 0.9)]
    assert QwenBackend._requires_windowing(valid, 1.0, 0.08) is False
    assert QwenBackend._requires_windowing(zero, 1.0, 0.08) is True


def test_constrained_timestamp_decode_eliminates_zero_width_words():
    import torch

    logits = torch.full((4, 6), -10.0)
    logits[0, 1] = 10.0
    logits[1, 1] = 10.0
    logits[1, 2] = 9.0
    logits[2, 2] = 10.0
    logits[3, 2] = 10.0
    logits[3, 3] = 9.0
    decoded = QwenBackend._constrained_timestamp_ids(logits, 5).tolist()
    assert decoded[0] < decoded[1] <= decoded[2] < decoded[3]


def test_constrained_timestamp_decode_respects_media_bound():
    import torch

    logits = torch.full((2, 8), -10.0)
    logits[0, 6] = 10.0
    logits[1, 7] = 10.0
    decoded = QwenBackend._constrained_timestamp_ids(logits, 4, 3).tolist()
    assert decoded[0] <= 3
    assert decoded[0] < decoded[1] <= 4


def test_relative_timestamp_confidence_does_not_penalize_diffuse_logits():
    import torch

    logits = torch.zeros((2, 5000))
    selected = torch.tensor([10, 11])
    chosen = logits.gather(1, selected.unsqueeze(1)).squeeze(1)
    relative = torch.exp(chosen - logits.amax(dim=-1))
    absolute = torch.exp(chosen - torch.logsumexp(logits, dim=-1))
    assert relative.tolist() == [1.0, 1.0]
    assert max(absolute.tolist()) < 0.01


def test_low_confidence_log_identifies_word_without_text(caplog):
    words = [AlignedUnit("secret", 0.1, 0.2, 0.001)]
    with pytest.raises(AlignmentError, match="sufficient confidence"):
        validate_words(words, 1.0, 0.01, request_id="request-456")
    assert "request_id=request-456" in caplog.text
    assert "word_index=0" in caplog.text
    assert "secret" not in caplog.text


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
        timestamp_segment_time = 80

        @classmethod
        def from_pretrained(cls, model, **kwargs):
            calls.append(kwargs["device_map"])
            if kwargs["device_map"] == "cuda:0":
                raise RuntimeError("ROCm allocation failed")
            return cls()

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


def test_non_loopback_bind_accepts_authentication(tmp_path):
    app = create_app(settings(tmp_path, host="192.168.0.168", auth_token="x" * 43), FakeBackend())
    assert app is not None


def test_bearer_token_loads_from_protected_file(tmp_path, monkeypatch):
    token_file = tmp_path / "token.txt"
    token_file.write_text("t" * 43, encoding="utf-8")
    monkeypatch.delenv("ALIGNER_AUTH_TOKEN", raising=False)
    monkeypatch.setenv("ALIGNER_AUTH_TOKEN_FILE", str(token_file))
    assert Settings().auth_token == "t" * 43


def test_unsupported_language_rejected(tmp_path):
    with client_for(tmp_path) as client:
        response = client.post(
            "/v1/audio/alignments",
            files={"file": ("sample.wav", b"audio", "audio/wav")},
            data={"text": "hello", "language": "xx"},
        )
    assert response.status_code == 400
    assert response.json()["error"]["code"] == "INVALID_LANGUAGE"
