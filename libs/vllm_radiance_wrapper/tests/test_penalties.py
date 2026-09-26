import unittest
from types import SimpleNamespace

import torch

from inferdeck_vllm_radiance_penalties import (
    InferDeckPenaltiesProcessor,
    _apply_penalties,
)


class PenaltyTests(unittest.TestCase):
    def test_sign_aware_counts_and_presence(self):
        logits = torch.tensor([0.0, 1.0, -2.0, 4.0])
        result = _apply_penalties([2, 2], [2, 2, 3], logits, 2.0, 0.5, 0.25, -1)
        self.assertTrue(torch.allclose(result, torch.tensor([0.0, 1.0, -5.25, 1.25])))

    def test_window_eviction_and_zero_disable(self):
        original = torch.tensor([0.0, 2.0, 2.0, 2.0])
        windowed = _apply_penalties([1, 2], [3], original.clone(), 2.0, 0.0, 0.0, 2)
        self.assertTrue(torch.allclose(windowed, torch.tensor([0.0, 2.0, 2.0, 1.0])))
        disabled = _apply_penalties([1, 2], [3], original.clone(), 2.0, 0.0, 0.0, 0)
        self.assertTrue(torch.equal(disabled, original))

    def test_adapter_reads_extra_args_and_neutral_request(self):
        adapter = object.__new__(InferDeckPenaltiesProcessor)
        params = SimpleNamespace(extra_args={"inferdeck_penalties": {
            "repeat_last_n": -1, "repetition_penalty": 2.0,
            "frequency_penalty": 0.0, "presence_penalty": 0.0}})
        processor = adapter.new_req_logits_processor(params)
        self.assertIsNotNone(processor)
        logits = processor([1], [1], torch.tensor([0.0, 2.0]))
        self.assertEqual(float(logits[1]), 1.0)
        self.assertIsNone(adapter.new_req_logits_processor(SimpleNamespace(extra_args=None)))

    def test_adapter_tracks_output_moves_and_removal(self):
        from vllm.v1.sample.logits_processor import BatchUpdate, MoveDirectionality
        adapter = InferDeckPenaltiesProcessor(None, torch.device("cpu"), False)
        params = SimpleNamespace(extra_args={"inferdeck_penalties": {
            "repeat_last_n": 1, "repetition_penalty": 2.0}})
        output = [1]
        adapter.update_state(BatchUpdate(1, [], [(0, params, [2], output)], []))
        self.assertTrue(torch.equal(adapter.apply(torch.tensor([[0., 4., 4.]])), torch.tensor([[0., 2., 4.]])))
        output.append(2)
        adapter.update_state(None)
        self.assertTrue(torch.equal(adapter.apply(torch.tensor([[0., 4., 4.]])), torch.tensor([[0., 4., 2.]])))
        adapter.update_state(BatchUpdate(2, [], [], [(0, 1, MoveDirectionality.UNIDIRECTIONAL)]))
        self.assertTrue(torch.equal(adapter.apply(torch.tensor([[0., 4., 4.], [0., 4., 4.]])), torch.tensor([[0., 4., 4.], [0., 4., 2.]])))
        adapter.update_state(BatchUpdate(0, [1], [], []))
        self.assertFalse(adapter.req_info)
        self.assertTrue(torch.equal(adapter.apply(torch.tensor([[0., 4., 4.]])), torch.tensor([[0., 4., 4.]])))
    def test_invalid_window_rejected(self):
        params = SimpleNamespace(extra_args={"inferdeck_penalties": {"repeat_last_n": -2}})
        with self.assertRaisesRegex(ValueError, "repeat_last_n"):
            InferDeckPenaltiesProcessor.validate_params(params)


if __name__ == "__main__":
    unittest.main()