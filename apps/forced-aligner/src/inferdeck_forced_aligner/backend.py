import gc
import logging
import math
import wave
from array import array
from dataclasses import replace
from pathlib import Path
from typing import Protocol

from .errors import AlignmentError
from .words import AlignedUnit


logger = logging.getLogger("inferdeck.forced_aligner")


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
        minimum_confidence: float = 0.01,
    ):
        self.model_name = model
        self.revision = revision
        self.requested_device = requested_device
        self.cpu_fallback = cpu_fallback
        self.min_gpu_free_mb = min_gpu_free_mb
        self.cache_dir = cache_dir
        self.minimum_confidence = minimum_confidence
        self.loaded = False
        self.backend_name = "unavailable"
        self.aligner = None
        self.torch = None
        self.timestamp_grid_seconds = 0.0
        self.window_seconds = 30.0

    @staticmethod
    def timestamp_token_to_seconds(token_id: int, segment_time_ms: float) -> float:
        return float(token_id) * float(segment_time_ms) / 1000.0

    @staticmethod
    def _constrained_timestamp_ids(selected_logits, maximum_end_token_id: int, maximum_start_token_id: int | None = None):
        position_count, class_count = selected_logits.shape
        usable_classes = min(class_count, maximum_end_token_id + 1)
        if position_count == 0 or position_count % 2 != 0 or usable_classes < 2:
            raise AlignmentError(422, "ALIGNMENT_FAILED", "The model returned an incomplete timestamp sequence")
        scores = selected_logits[:, :usable_classes].float()
        start_limit = min(usable_classes - 1, maximum_start_token_id if maximum_start_token_id is not None else maximum_end_token_id)
        if start_limit + 1 < usable_classes:
            scores[0::2, start_limit + 1:] = float("-inf")
        dynamic = scores[0]
        backpointers = []
        for position in range(1, position_count):
            required_step = 1 if position % 2 == 1 else 0
            prefix_values, prefix_indexes = dynamic.cummax(dim=0)
            next_dynamic = scores.new_full((usable_classes,), float("-inf"))
            limit = usable_classes - required_step
            next_dynamic[required_step:] = scores[position, required_step:] + prefix_values[:limit]
            pointer = prefix_indexes.new_full((usable_classes,), -1)
            pointer[required_step:] = prefix_indexes[:limit]
            backpointers.append(pointer.to("cpu"))
            dynamic = next_dynamic
        if not dynamic.isfinite().any().item():
            raise AlignmentError(422, "ALIGNMENT_FAILED", "No valid chronological timestamp sequence exists")
        current = int(dynamic.argmax().item())
        decoded = [current]
        for pointer in reversed(backpointers):
            current = int(pointer[current].item())
            if current < 0:
                raise AlignmentError(422, "ALIGNMENT_FAILED", "No valid chronological timestamp sequence exists")
            decoded.append(current)
        decoded.reverse()
        return selected_logits.new_tensor(decoded).long()

    def _finish_load(self, backend_name: str) -> None:
        self.backend_name = backend_name
        self.timestamp_grid_seconds = float(self.aligner.timestamp_segment_time) / 1000.0
        self.loaded = True
        logger.info(
            "forced aligner timestamp grid segment_ms=%.3f segment_seconds=%.6f",
            float(self.aligner.timestamp_segment_time),
            self.timestamp_grid_seconds,
        )

    @staticmethod
    def _duration(audio: Path) -> float:
        with wave.open(str(audio), "rb") as wav:
            return wav.getnframes() / float(wav.getframerate())

    @staticmethod
    def _silence_boundaries(audio: Path, maximum_window_seconds: float) -> list[float]:
        with wave.open(str(audio), "rb") as wav:
            sample_rate = wav.getframerate()
            frame_count = wav.getnframes()
            if wav.getnchannels() != 1 or wav.getsampwidth() != 2:
                raise AlignmentError(500, "AUDIO_DECODE_FAILED", "Normalized audio is not mono 16-bit PCM")
            samples = array("h", wav.readframes(frame_count))
        duration = frame_count / float(sample_rate)
        frame_size = max(1, round(sample_rate * 0.02))
        minimum_silence_frames = max(1, round(0.2 / 0.02))
        silent_runs: list[tuple[int, int]] = []
        run_start: int | None = None
        frame_index = 0
        for offset in range(0, len(samples), frame_size):
            frame = samples[offset:offset + frame_size]
            rms = math.sqrt(sum(sample * sample for sample in frame) / max(len(frame), 1))
            if rms <= 500:
                if run_start is None:
                    run_start = frame_index
            elif run_start is not None:
                if frame_index - run_start >= minimum_silence_frames:
                    silent_runs.append((run_start, frame_index))
                run_start = None
            frame_index += 1
        if run_start is not None and frame_index - run_start >= minimum_silence_frames:
            silent_runs.append((run_start, frame_index))
        silence_points = [((start + end) * 0.5 * frame_size) / sample_rate for start, end in silent_runs]
        boundaries = [0.0]
        cursor = 0.0
        while duration - cursor > maximum_window_seconds:
            target = cursor + maximum_window_seconds
            candidates = [point for point in silence_points if cursor + 0.5 <= point <= target]
            cut = max(candidates) if candidates else target
            if cut - cursor < 0.5:
                cut = target
            boundaries.append(min(cut, duration))
            cursor = cut
        boundaries.append(duration)
        return boundaries

    @staticmethod
    def _partition_units(
        units: list[AlignedUnit],
        boundaries: list[float],
        maximum_units_per_second: float = float("inf"),
    ) -> list[tuple[float, float, int, int]]:
        groups = QwenBackend._group_units(units, boundaries)
        if math.isfinite(maximum_units_per_second):
            capacities = [
                max(1, math.floor((boundaries[index + 1] - boundaries[index]) * maximum_units_per_second))
                for index in range(len(groups))
            ]
            if sum(capacities) < len(units):
                raise AlignmentError(422, "ALIGNMENT_FAILED", "Transcript density exceeds the windowed alignment limit")
            raw_ends: list[int] = []
            cumulative = 0
            for group in groups:
                cumulative += len(group)
                raw_ends.append(cumulative)
            balanced: list[list[int]] = []
            first = 0
            for index, raw_end in enumerate(raw_ends):
                remaining_capacity = sum(capacities[index + 1:])
                minimum_end = max(first, len(units) - remaining_capacity)
                maximum_end = min(len(units), first + capacities[index])
                end = min(max(raw_end, minimum_end), maximum_end)
                balanced.append(list(range(first, end)))
                first = end
            groups = balanced
        partitions: list[tuple[float, float, int, int]] = []
        for index, group in enumerate(groups):
            if group:
                partitions.append((boundaries[index], boundaries[index + 1], group[0], group[-1] + 1))
        return partitions

    @staticmethod
    def _group_units(units: list[AlignedUnit], boundaries: list[float]) -> list[list[int]]:
        groups: list[list[int]] = [[] for _ in range(len(boundaries) - 1)]
        window_index = 0
        for index, unit in enumerate(units):
            midpoint = (unit.start + unit.end) * 0.5
            while window_index + 1 < len(groups) and midpoint >= boundaries[window_index + 1]:
                window_index += 1
            groups[window_index].append(index)
        return groups

    @staticmethod
    def _saturated_tail_index(
        groups: list[list[int]],
        boundaries: list[float],
        maximum_units_per_second: float,
    ) -> int | None:
        for index, group in enumerate(groups[:-1]):
            capacity = max(1, math.floor((boundaries[index + 1] - boundaries[index]) * maximum_units_per_second))
            if len(group) >= capacity and any(groups[later] for later in range(index + 1, len(groups))):
                return index
        return None

    @staticmethod
    def _write_window(audio: Path, output: Path, start: float, end: float) -> float:
        with wave.open(str(audio), "rb") as source:
            sample_rate = source.getframerate()
            start_frame = max(0, min(source.getnframes(), round(start * sample_rate)))
            end_frame = max(start_frame + 1, min(source.getnframes(), round(end * sample_rate)))
            source.setpos(start_frame)
            frames = source.readframes(end_frame - start_frame)
            with wave.open(str(output), "wb") as destination:
                destination.setparams(source.getparams())
                destination.writeframes(frames)
        return (end_frame - start_frame) / float(sample_rate)

    @staticmethod
    def _requires_windowing(units: list[AlignedUnit], duration: float, tolerance: float) -> bool:
        previous_end = 0.0
        for unit in units:
            if not all(math.isfinite(value) for value in (unit.start, unit.end)):
                return True
            if unit.start < 0 or unit.start >= duration or unit.end <= unit.start:
                return True
            if unit.end > duration + tolerance + 1e-9 or unit.start < previous_end:
                return True
            previous_end = unit.end
        return False

    @staticmethod
    def _input_text(word_list: list[str]) -> str:
        timestamp = "<timestamp><timestamp>"
        return "<|audio_start|><|audio_pad|><|audio_end|>" + timestamp.join(word_list) + timestamp

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
                self._finish_load("rocm" if torch.version.hip else "cuda")
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
        self._finish_load("cpu")

    def _infer(
        self,
        audio: Path,
        word_list: list[str],
        input_text: str,
        duration: float,
        repair_invalid: bool,
    ) -> list[AlignedUnit]:
        torch = self.torch
        with torch.inference_mode():
            from qwen_asr.inference.utils import normalize_audios

            normalized_audio = normalize_audios(str(audio))
            inputs = self.aligner.processor(
                text=[input_text], audio=normalized_audio, return_tensors="pt", padding=True
            )
            inputs = inputs.to(self.aligner.model.device).to(self.aligner.model.dtype)
            logits = self.aligner.model.thinker(**inputs).logits
            mask = inputs["input_ids"][0] == self.aligner.timestamp_token_id
            selected_logits = logits[0][mask]
            raw_ids_tensor = selected_logits.argmax(dim=-1)
            raw_ids = raw_ids_tensor.to("cpu").tolist()

            def build_units(timestamp_ids, probabilities):
                timestamp_ms = (timestamp_ids * self.aligner.timestamp_segment_time).to("cpu").numpy()
                parsed = self.aligner.aligner_processor.parse_timestamp(word_list, timestamp_ms)
                if len(probabilities) != len(parsed) * 2:
                    raise AlignmentError(422, "ALIGNMENT_FAILED", "The model returned an incomplete timestamp sequence")
                return [
                    AlignedUnit(
                        str(item["text"]),
                        round(float(item["start_time"]) / 1000.0, 3),
                        round(float(item["end_time"]) / 1000.0, 3),
                        round(min(probabilities[index * 2:index * 2 + 2]), 6),
                        int(raw_ids[index * 2]),
                        int(raw_ids[index * 2 + 1]),
                    )
                    for index, item in enumerate(parsed)
                ]

            raw_probabilities = [1.0] * len(raw_ids)
            raw_units = build_units(raw_ids_tensor, raw_probabilities)
            if not repair_invalid or not self._requires_windowing(
                raw_units, duration, self.timestamp_grid_seconds
            ):
                return raw_units

            maximum_end_token_id = math.floor(
                (duration + self.timestamp_grid_seconds + 1e-9) / self.timestamp_grid_seconds
            )
            maximum_start_token_id = math.ceil(
                (duration - 1e-9) / self.timestamp_grid_seconds
            ) - 1
            selected_ids = self._constrained_timestamp_ids(
                selected_logits,
                maximum_end_token_id,
                maximum_start_token_id,
            )
            corrected_slots = int((selected_ids != raw_ids_tensor).sum().item())
            logger.info(
                "forced aligner constrained timestamp repair positions=%d corrected_slots=%d duration_seconds=%.6f maximum_start_token_id=%d maximum_end_token_id=%d",
                len(selected_ids),
                corrected_slots,
                duration,
                maximum_start_token_id,
                maximum_end_token_id,
            )
            chosen_logits = selected_logits.gather(1, selected_ids.unsqueeze(1)).squeeze(1).float()
            best_logits = selected_logits.float().amax(dim=-1)
            probabilities = torch.exp(chosen_logits - best_logits).clamp(max=1.0).to("cpu").tolist()
            return build_units(selected_ids, probabilities)

    def _align_windows(
        self,
        audio: Path,
        word_list: list[str],
        coarse: list[AlignedUnit],
        duration: float,
        maximum_units_per_second: float,
        depth: int = 0,
    ) -> list[AlignedUnit]:
        boundaries = self._silence_boundaries(audio, self.window_seconds)
        groups = self._group_units(coarse, boundaries)
        saturated_index = self._saturated_tail_index(groups, boundaries, maximum_units_per_second)
        if saturated_index is not None and saturated_index > 0 and depth < 8:
            prefix_partitions = [
                (boundaries[index], boundaries[index + 1], group[0], group[-1] + 1)
                for index, group in enumerate(groups[:saturated_index])
                if group
            ]
            tail_first = sum(len(group) for group in groups[:saturated_index])
            tail_start = boundaries[saturated_index]
            tail_path = audio.parent / f"alignment-tail-{depth:02d}.wav"
            tail_duration = self._write_window(audio, tail_path, tail_start, duration)
            tail_words = word_list[tail_first:]
            tail_coarse = self._infer(
                tail_path,
                tail_words,
                self._input_text(tail_words),
                tail_duration,
                False,
            )
            merged = self._align_partitions(
                audio,
                word_list,
                prefix_partitions,
                depth,
                maximum_units_per_second,
            )
            tail_aligned = self._align_windows(
                tail_path,
                tail_words,
                tail_coarse,
                tail_duration,
                maximum_units_per_second,
                depth + 1,
            )
            merged.extend(
                replace(unit, start=round(unit.start + tail_start, 6), end=round(unit.end + tail_start, 6))
                for unit in tail_aligned
            )
            if len(merged) != len(word_list) or [unit.text for unit in merged] != word_list:
                raise AlignmentError(422, "ALIGNMENT_FAILED", "Rebased window alignment did not preserve every transcript unit")
            logger.info(
                "forced aligner coarse tail rebase duration_seconds=%.3f tail_start_seconds=%.3f tail_duration_seconds=%.3f prefix_units=%d tail_units=%d depth=%d",
                duration,
                tail_start,
                tail_duration,
                tail_first,
                len(tail_words),
                depth,
            )
            return merged
        partitions = self._partition_units(coarse, boundaries, maximum_units_per_second)
        merged = self._align_partitions(
            audio,
            word_list,
            partitions,
            depth,
            maximum_units_per_second,
        )
        if len(merged) != len(word_list) or [unit.text for unit in merged] != word_list:
            raise AlignmentError(422, "ALIGNMENT_FAILED", "Windowed alignment did not preserve every transcript unit")
        logger.info(
            "forced aligner window fallback duration_seconds=%.3f windows=%d units=%d maximum_window_seconds=%.3f maximum_units_per_second=%.3f depth=%d",
            duration,
            len(partitions),
            len(merged),
            self.window_seconds,
            maximum_units_per_second,
            depth,
        )
        return merged

    def _align_partitions(
        self,
        audio: Path,
        word_list: list[str],
        partitions: list[tuple[float, float, int, int]],
        depth: int,
        maximum_units_per_second: float,
    ) -> list[AlignedUnit]:
        current = list(partitions)
        aligned_groups = [
            self._align_partition(audio, word_list, partition, depth, index)
            for index, partition in enumerate(current)
        ]
        maximum_shift = 12
        maximum_iterations = min(6, max(1, len(current)))
        maximum_candidate_pairs = 24
        evaluated_candidate_pairs = 0
        for _ in range(maximum_iterations):
            group_index, group_confidence = min(
                enumerate(aligned_groups),
                key=lambda item: min(unit.confidence for unit in item[1]),
            )
            lowest_confidence = min(unit.confidence for unit in group_confidence)
            if lowest_confidence >= self.minimum_confidence:
                break
            best: tuple[float, int, int, list[AlignedUnit], list[AlignedUnit]] | None = None
            threshold_candidate_found = False
            for boundary_index in (group_index - 1, group_index):
                if boundary_index < 0 or boundary_index + 1 >= len(current):
                    continue
                left = current[boundary_index]
                right = current[boundary_index + 1]
                if left[1] > right[0] + 1e-9 or left[3] != right[2]:
                    continue
                current_pair_confidence = min(
                    min(unit.confidence for unit in aligned_groups[boundary_index]),
                    min(unit.confidence for unit in aligned_groups[boundary_index + 1]),
                )
                cut = left[3]
                shifts = [direction * distance for distance in range(1, maximum_shift + 1) for direction in (-1, 1)]
                for shift in shifts:
                    if evaluated_candidate_pairs >= maximum_candidate_pairs:
                        break
                    candidate_cut = cut + shift
                    if candidate_cut <= left[2] or candidate_cut >= right[3]:
                        continue
                    left_capacity = max(1, math.floor((left[1] - left[0]) * maximum_units_per_second))
                    right_capacity = max(1, math.floor((right[1] - right[0]) * maximum_units_per_second))
                    if candidate_cut - left[2] > left_capacity or right[3] - candidate_cut > right_capacity:
                        continue
                    candidate_left = (left[0], left[1], left[2], candidate_cut)
                    candidate_right = (right[0], right[1], candidate_cut, right[3])
                    try:
                        evaluated_candidate_pairs += 1
                        aligned_left = self._align_partition(
                            audio,
                            word_list,
                            candidate_left,
                            depth,
                            boundary_index,
                        )
                        aligned_right = self._align_partition(
                            audio,
                            word_list,
                            candidate_right,
                            depth,
                            boundary_index + 1,
                        )
                    except AlignmentError:
                        continue
                    candidate_confidence = min(
                        min(unit.confidence for unit in aligned_left),
                        min(unit.confidence for unit in aligned_right),
                    )
                    if candidate_confidence <= current_pair_confidence + 1e-9:
                        continue
                    if best is None or candidate_confidence > best[0]:
                        best = (candidate_confidence, boundary_index, shift, aligned_left, aligned_right)
                    if candidate_confidence >= self.minimum_confidence:
                        threshold_candidate_found = True
                        break
                if threshold_candidate_found or evaluated_candidate_pairs >= maximum_candidate_pairs:
                    break
            if best is None:
                break
            candidate_confidence, boundary_index, shift, aligned_left, aligned_right = best
            left = current[boundary_index]
            right = current[boundary_index + 1]
            candidate_cut = left[3] + shift
            current[boundary_index] = (left[0], left[1], left[2], candidate_cut)
            current[boundary_index + 1] = (right[0], right[1], candidate_cut, right[3])
            aligned_groups[boundary_index] = aligned_left
            aligned_groups[boundary_index + 1] = aligned_right
            logger.info(
                "forced aligner transcript boundary refined depth=%d boundary_index=%d shift_units=%d confidence_before=%.9f confidence_after=%.9f evaluated_candidate_pairs=%d",
                depth,
                boundary_index,
                shift,
                lowest_confidence,
                candidate_confidence,
                evaluated_candidate_pairs,
            )
            if evaluated_candidate_pairs >= maximum_candidate_pairs:
                break
        return [unit for group in aligned_groups for unit in group]

    def _align_partition(
        self,
        audio: Path,
        word_list: list[str],
        partition: tuple[float, float, int, int],
        depth: int,
        window_index: int,
    ) -> list[AlignedUnit]:
        start, end, first, last = partition
        window_path = audio.parent / f"alignment-window-{depth:02d}-{window_index:04d}.wav"
        window_duration = self._write_window(audio, window_path, start, end)
        window_words = word_list[first:last]
        aligned = self._infer(
            window_path,
            window_words,
            self._input_text(window_words),
            window_duration,
            True,
        )
        result: list[AlignedUnit] = []
        for unit in aligned:
            local_end = unit.end
            if local_end > window_duration and local_end - window_duration <= self.timestamp_grid_seconds + 1e-9:
                local_end = window_duration
            result.append(replace(unit, start=round(unit.start + start, 6), end=round(local_end + start, 6)))
        return result

    def align(self, audio: Path, text: str, language: str) -> list[AlignedUnit]:
        if not self.loaded or self.aligner is None or self.torch is None:
            raise AlignmentError(503, "MODEL_UNAVAILABLE", "The forced-alignment model is unavailable")
        language_name = language_for_model(language)
        word_list, input_text = self.aligner.aligner_processor.encode_timestamp(text, language_name)
        if not word_list:
            raise AlignmentError(400, "EMPTY_TRANSCRIPT", "Transcript text must contain at least one word")
        duration = self._duration(audio)
        direct = self._infer(audio, word_list, input_text, duration, False)
        if (
            not self._requires_windowing(direct, duration, self.timestamp_grid_seconds)
            and min(unit.confidence for unit in direct) >= self.minimum_confidence
        ):
            return direct
        rates = [12.0, 10.0, 8.0] if language_name in {"Chinese", "Cantonese", "Japanese", "Korean"} else [4.0, 3.0, 2.5]
        best: list[AlignedUnit] | None = None
        best_confidence = -1.0
        for maximum_units_per_second in rates:
            try:
                candidate = self._align_windows(
                    audio,
                    word_list,
                    direct,
                    duration,
                    maximum_units_per_second,
                )
            except AlignmentError:
                continue
            candidate_confidence = min(unit.confidence for unit in candidate)
            if candidate_confidence > best_confidence:
                best = candidate
                best_confidence = candidate_confidence
            if (
                candidate_confidence >= self.minimum_confidence
                and not self._requires_windowing(candidate, duration, self.timestamp_grid_seconds)
            ):
                return candidate
            logger.info(
                "forced aligner window retry duration_seconds=%.3f maximum_units_per_second=%.3f minimum_confidence=%.9f required_confidence=%.9f",
                duration,
                maximum_units_per_second,
                candidate_confidence,
                self.minimum_confidence,
            )
        if best is None:
            raise AlignmentError(422, "ALIGNMENT_FAILED", "Windowed alignment could not preserve a valid transcript timeline")
        return best
