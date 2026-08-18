import math
import unicodedata
from dataclasses import dataclass

from .errors import AlignmentError


@dataclass(frozen=True)
class AlignedUnit:
    text: str
    start: float
    end: float
    confidence: float


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
        result.append(AlignedUnit(original, collected[0].start, collected[-1].end, confidence))
    if any(_normal(unit.text) for unit in units[unit_index:]):
        raise AlignmentError(422, "ALIGNMENT_FAILED", "The model returned units that are absent from the supplied transcript")
    return result


def validate_words(words: list[AlignedUnit], duration: float, minimum_confidence: float) -> None:
    previous_end = 0.0
    for word in words:
        values = (word.start, word.end, word.confidence)
        if not all(math.isfinite(value) for value in values):
            raise AlignmentError(422, "ALIGNMENT_FAILED", "The model returned non-finite alignment values")
        if word.start < 0 or word.end > duration + 0.001 or word.end <= word.start:
            raise AlignmentError(422, "ALIGNMENT_FAILED", "The model returned invalid or source-unbounded timestamps")
        if word.start < previous_end:
            raise AlignmentError(422, "ALIGNMENT_FAILED", "The model returned non-chronological timestamps")
        previous_end = word.end
    if not words:
        raise AlignmentError(422, "ALIGNMENT_FAILED", "The model returned no aligned words")
    if min(word.confidence for word in words) < minimum_confidence:
        raise AlignmentError(422, "LOW_CONFIDENCE", "The transcript could not be aligned with sufficient confidence")
