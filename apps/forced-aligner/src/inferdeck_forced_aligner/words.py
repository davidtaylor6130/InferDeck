import math
import logging
import unicodedata
from dataclasses import dataclass, replace

from .errors import AlignmentError


logger = logging.getLogger("inferdeck.forced_aligner")


@dataclass(frozen=True)
class AlignedUnit:
    text: str
    start: float
    end: float
    confidence: float
    raw_start: int | None = None
    raw_end: int | None = None


def _kept(character: str) -> bool:
    return character == "'" or unicodedata.category(character).startswith(("L", "N"))


def clean_token(token: str) -> str:
    return "".join(character for character in token if _kept(character))


def transcript_words(text: str) -> list[str]:
    return [cleaned for part in text.split() if (cleaned := clean_token(part))]


def _normal(value: str) -> str:
    return unicodedata.normalize("NFKC", clean_token(value)).casefold()


def aggregate_units_to_words(units: list[AlignedUnit], transcript: str) -> list[AlignedUnit]:
    expected = transcript_words(transcript)
    if not expected:
        raise AlignmentError(400, "EMPTY_TRANSCRIPT", "Transcript text must contain at least one word")
    result: list[AlignedUnit] = []
    unit_index = 0
    for original in expected:
        target = _normal(original)
        collected: list[AlignedUnit] = []
        combined = ""
        while unit_index < len(units) and len(combined) < len(target):
            unit = units[unit_index]
            candidate = _normal(unit.text)
            if not candidate:
                unit_index += 1
                continue
            if not target.startswith(combined + candidate):
                break
            collected.append(unit)
            combined += candidate
            unit_index += 1
        if combined != target or not collected:
            raise AlignmentError(422, "ALIGNMENT_FAILED", "The model output could not be matched reliably to the supplied transcript")
        confidence = min(max(0.0, min(1.0, item.confidence)) for item in collected)
        result.append(
            AlignedUnit(
                original,
                collected[0].start,
                collected[-1].end,
                confidence,
                collected[0].raw_start,
                collected[-1].raw_end,
            )
        )
    if any(_normal(unit.text) for unit in units[unit_index:]):
        raise AlignmentError(422, "ALIGNMENT_FAILED", "The model returned units that are absent from the supplied transcript")
    return result


def _range_error(
    request_id: str,
    word_index: int,
    field: str,
    raw_timestamp: int | None,
    converted_seconds: float,
    duration: float,
    tolerance: float,
    development_diagnostics: bool,
) -> AlignmentError:
    logger.warning(
        "alignment timestamp rejected request_id=%s word_index=%d field=%s raw_timestamp_token=%s converted_seconds=%.6f audio_duration_seconds=%.6f tolerance_seconds=%.6f",
        request_id,
        word_index,
        field,
        raw_timestamp,
        converted_seconds,
        duration,
        tolerance,
    )
    message = "The model returned invalid or source-unbounded timestamps"
    if development_diagnostics:
        message = (
            f"Timestamp range violation at word_index={word_index} field={field} "
            f"raw_timestamp_token={raw_timestamp} converted_seconds={converted_seconds:.6f} "
            f"audio_duration_seconds={duration:.6f} tolerance_seconds={tolerance:.6f}"
        )
    return AlignmentError(422, "ALIGNMENT_FAILED", message)


def validate_words(
    words: list[AlignedUnit],
    duration: float,
    minimum_confidence: float,
    boundary_tolerance: float = 0.0,
    request_id: str = "unknown",
    development_diagnostics: bool = False,
) -> list[AlignedUnit]:
    previous_end = 0.0
    validated: list[AlignedUnit] = []
    tolerance = max(0.0, boundary_tolerance)
    for index, original in enumerate(words):
        word = original
        values = (word.start, word.end, word.confidence)
        if not all(math.isfinite(value) for value in values):
            raise AlignmentError(422, "ALIGNMENT_FAILED", "The model returned non-finite alignment values")
        if word.start < 0:
            raise _range_error(request_id, index, "start", word.raw_start, word.start, duration, tolerance, development_diagnostics)
        if word.end > duration:
            overrun = word.end - duration
            if overrun <= tolerance + 1e-9 and word.start < duration:
                word = replace(word, end=duration)
            else:
                raise _range_error(request_id, index, "end", word.raw_end, word.end, duration, tolerance, development_diagnostics)
        if word.start >= duration:
            raise _range_error(request_id, index, "start", word.raw_start, word.start, duration, tolerance, development_diagnostics)
        if word.end <= word.start:
            raise _range_error(request_id, index, "end", word.raw_end, word.end, duration, tolerance, development_diagnostics)
        if word.start < previous_end:
            raise AlignmentError(422, "ALIGNMENT_FAILED", "The model returned non-chronological timestamps")
        previous_end = word.end
        validated.append(word)
    if not words:
        raise AlignmentError(422, "ALIGNMENT_FAILED", "The model returned no aligned words")
    lowest_index, lowest_word = min(enumerate(validated), key=lambda item: item[1].confidence)
    if lowest_word.confidence < minimum_confidence:
        logger.warning(
            "alignment confidence rejected request_id=%s word_index=%d confidence=%.9f minimum_confidence=%.9f",
            request_id,
            lowest_index,
            lowest_word.confidence,
            minimum_confidence,
        )
        raise AlignmentError(422, "LOW_CONFIDENCE", "The transcript could not be aligned with sufficient confidence")
    return validated
