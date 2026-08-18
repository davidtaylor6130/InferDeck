import gc
from pathlib import Path
from typing import Protocol

from .errors import AlignmentError
from .words import AlignedUnit


LANGUAGES = {
    "zh": "Chinese", "en": "English", "yue": "Cantonese", "fr": "French",
    "de": "German", "it": "Italian", "ja": "Japanese", "ko": "Korean",
    "pt": "Portuguese", "ru": "Russian", "es": "Spanish",
}


def language_for_model(language: str) -> str:
    normalized = language.strip().lower().replace("_", "-")
    base = normalized.split("-", 1)[0]
    if base in LANGUAGES:
        return LANGUAGES[base]
    names = {name.lower(): name for name in LANGUAGES.values()}
    if normalized in names:
        return names[normalized]
    raise AlignmentError(400, "INVALID_LANGUAGE", "Language is not supported by the forced-alignment model")


class Backend(Protocol):
    loaded: bool
    backend_name: str

    def load(self) -> None: ...
    def align(self, audio: Path, text: str, language: str) -> list[AlignedUnit]: ...


class QwenBackend:
    def __init__(
        self,
        model: str,
        revision: str,
        requested_device: str,
        cpu_fallback: bool,
        min_gpu_free_mb: int = 3072,
        cache_dir: Path | None = None,
    ):
        self.model_name = model
        self.revision = revision
        self.requested_device = requested_device
        self.cpu_fallback = cpu_fallback
        self.min_gpu_free_mb = min_gpu_free_mb
        self.cache_dir = cache_dir
        self.loaded = False
        self.backend_name = "unavailable"
        self.aligner = None
        self.torch = None

    def load(self) -> None:
        import torch
        from huggingface_hub import snapshot_download
        from qwen_asr import Qwen3ForcedAligner

        self.torch = torch
        model_source = self.model_name
        if self.cache_dir:
            model_source = snapshot_download(
                repo_id=self.model_name,
                revision=self.revision,
                cache_dir=self.cache_dir,
                local_files_only=True,
            )
        wants_gpu = self.requested_device in {"rocm", "cuda", "gpu"}
        gpu_available = wants_gpu and torch.cuda.is_available()
        if gpu_available:
            try:
                free_bytes, _ = torch.cuda.mem_get_info()
                gpu_available = free_bytes >= self.min_gpu_free_mb * 1024 * 1024
            except Exception:
                gpu_available = True
        if gpu_available:
            try:
                self.aligner = Qwen3ForcedAligner.from_pretrained(
                    model_source,
                    local_files_only=True,
                    dtype=torch.bfloat16,
                    device_map="cuda:0",
                )
                self.backend_name = "rocm" if torch.version.hip else "cuda"
                self.loaded = True
                return
            except Exception:
                self.aligner = None
                gc.collect()
                torch.cuda.empty_cache()
                if not self.cpu_fallback:
                    raise
        elif wants_gpu and not self.cpu_fallback:
            raise RuntimeError(f"Requested {self.requested_device} backend is unavailable")
        self.aligner = Qwen3ForcedAligner.from_pretrained(
            model_source,
            local_files_only=True,
            dtype=torch.float32,
            device_map="cpu",
        )
        self.backend_name = "cpu"
        self.loaded = True

    def align(self, audio: Path, text: str, language: str) -> list[AlignedUnit]:
        if not self.loaded or self.aligner is None or self.torch is None:
            raise AlignmentError(503, "MODEL_UNAVAILABLE", "The forced-alignment model is unavailable")
        torch = self.torch
        language_name = language_for_model(language)
        with torch.inference_mode():
            word_list, input_text = self.aligner.aligner_processor.encode_timestamp(text, language_name)
            if not word_list:
                raise AlignmentError(400, "EMPTY_TRANSCRIPT", "Transcript text must contain at least one word")
            from qwen_asr.inference.utils import normalize_audios

            normalized_audio = normalize_audios(str(audio))
            inputs = self.aligner.processor(
                text=[input_text], audio=normalized_audio, return_tensors="pt", padding=True
            )
            inputs = inputs.to(self.aligner.model.device).to(self.aligner.model.dtype)
            logits = self.aligner.model.thinker(**inputs).logits
            output_ids = logits.argmax(dim=-1)
            mask = inputs["input_ids"][0] == self.aligner.timestamp_token_id
            selected_ids = output_ids[0][mask]
            selected_logits = logits[0][mask]
            probabilities = torch.exp(
                selected_logits.gather(1, selected_ids.unsqueeze(1)).squeeze(1)
                - torch.logsumexp(selected_logits.float(), dim=-1)
            ).to("cpu").tolist()
            timestamp_ms = (selected_ids * self.aligner.timestamp_segment_time).to("cpu").numpy()
            parsed = self.aligner.aligner_processor.parse_timestamp(word_list, timestamp_ms)
            if len(probabilities) != len(parsed) * 2:
                raise AlignmentError(422, "ALIGNMENT_FAILED", "The model returned an incomplete timestamp sequence")
            return [
                AlignedUnit(
                    str(item["text"]),
                    round(float(item["start_time"]) / 1000.0, 3),
                    round(float(item["end_time"]) / 1000.0, 3),
                    round(min(probabilities[index * 2:index * 2 + 2]), 6),
                )
                for index, item in enumerate(parsed)
            ]
