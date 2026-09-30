"""Output-history llama.cpp-compatible sampling penalties for vLLM V1."""
from __future__ import annotations

from collections import Counter
from typing import Any, Iterable

import torch
from vllm.v1.sample.logits_processor import AdapterLogitsProcessor

_EXTRA_KEY = "inferdeck_penalties"


def _history(output_ids: list[int], last_n: int) -> Iterable[int]:
    if last_n == 0:
        return []
    if last_n < 0:
        return output_ids
    return output_ids[-last_n:]


def _apply_penalties(
    prompt_ids: list[int], output_ids: list[int], logits: torch.Tensor,
    repeat_penalty: float, frequency_penalty: float,
    presence_penalty: float, repeat_last_n: int,
) -> torch.Tensor:
    del prompt_ids
    history = _history(output_ids, repeat_last_n)
    if not history or (repeat_penalty == 1.0 and frequency_penalty == 0.0 and presence_penalty == 0.0):
        return logits
    counts = Counter(history)
    ids_list = list(counts)
    ids = torch.tensor(ids_list, device=logits.device, dtype=torch.long)
    values = logits.index_select(0, ids)
    if repeat_penalty != 1.0:
        values = torch.where(values <= 0, values * repeat_penalty, values / repeat_penalty)
    if frequency_penalty != 0.0:
        counts_tensor = torch.tensor([counts[token] for token in ids_list], device=logits.device, dtype=values.dtype)
        values = values - counts_tensor * frequency_penalty
    if presence_penalty != 0.0:
        values = values - presence_penalty
    logits.index_copy_(0, ids, values)
    return logits


class InferDeckPenaltiesProcessor(AdapterLogitsProcessor):
    """Apply output-history, bounded penalties once per sampled token."""

    def is_argmax_invariant(self) -> bool:
        return False

    def new_req_logits_processor(self, params: Any):
        values = (getattr(params, "extra_args", None) or {}).get(_EXTRA_KEY)
        if not values:
            return None
        repeat_last_n = int(values.get("repeat_last_n", 0))
        repeat_penalty = float(values.get("repetition_penalty", 1.0))
        frequency_penalty = float(values.get("frequency_penalty", 0.0))
        presence_penalty = float(values.get("presence_penalty", 0.0))
        if repeat_last_n == 0 or (repeat_penalty == 1.0 and frequency_penalty == 0.0 and presence_penalty == 0.0):
            return None

        def apply(prompt_ids: list[int], output_ids: list[int], logits: torch.Tensor) -> torch.Tensor:
            return _apply_penalties(prompt_ids, output_ids, logits, repeat_penalty,
                                    frequency_penalty, presence_penalty, repeat_last_n)
        return apply

    @staticmethod
    def validate_params(params: Any) -> None:
        values = (getattr(params, "extra_args", None) or {}).get(_EXTRA_KEY)
        if values is None:
            return
        if int(values.get("repeat_last_n", 0)) < -1:
            raise ValueError("inferdeck repeat_last_n must be -1 or non-negative")