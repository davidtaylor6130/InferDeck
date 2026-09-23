import concurrent.futures
import copy
import importlib.util
import json
import shutil
import sys
import tempfile
import unittest
from unittest import mock
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[3]
PROBE = ROOT / "build" / "runtime-radiance-probe"
MODEL = PROBE / "models" / "Qwen3.8-27B-Quark-AWQ-MXFP4"
sys.path.insert(0, str(ROOT / "libs/vllm_radiance_wrapper/python"))
sys.path.insert(0, str(PROBE / "vLLM_for_AMD"))
from inferdeck_vllm_radiance_tokenizer import MODE, artifact_revision, register_tokenizer


class TokenizerRegistryTests(unittest.TestCase):
    def test_actual_renderer_reuse_and_frozen_continuation(self):
        from transformers import AutoTokenizer
        from vllm.tokenizers.registry import cached_tokenizer_from_config
        from vllm.renderers.registry import renderer_from_config
        revision = register_tokenizer(str(MODEL))
        config = SimpleNamespace(
            model_config=SimpleNamespace(tokenizer=str(MODEL), tokenizer_mode=MODE,
                tokenizer_revision=revision, runner_type="generate", trust_remote_code=False,
                skip_tokenizer_init=False, hf_config=None, enable_prompt_embeds=False,
                renderer_num_workers=1, is_multimodal_model=False),
            parallel_config=SimpleNamespace(_api_process_rank=0))
        first = cached_tokenizer_from_config(config.model_config)
        for _ in range(20):
            tokenizer = cached_tokenizer_from_config(config.model_config)
            renderer = renderer_from_config(config)
            try:
                self.assertIs(renderer.tokenizer, first)
                self.assertIs(copy.copy(first), first)
            finally:
                renderer._executor.shutdown(wait=True)
                renderer._mm_executor.shutdown(wait=True)
        helper_path = ROOT / "build/perf/matched-runtime-baseline/test-real-profile-cpu.py"
        spec = importlib.util.spec_from_file_location("frozen_profile_fixture", helper_path)
        helper = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(helper)
        case = json.loads((PROBE / "tool-quality-case.json").read_text(encoding="utf-8-sig"))
        messages = helper.continuation_messages(case)
        for message in messages:
            for call in message.get("tool_calls", []):
                call["function"]["arguments"] = json.loads(call["function"]["arguments"])
        kwargs = dict(tools=case["tools"], tokenize=True, return_dict=False,
            add_generation_prompt=True, enable_thinking=True, reasoning_effort=case["reasoning_effort"])
        fresh = AutoTokenizer.from_pretrained(MODEL, local_files_only=True, trust_remote_code=False, use_fast=True)
        expected = fresh.apply_chat_template(messages, **kwargs)
        self.assertEqual(len(expected), 407)
        def exercise(_):
            ids = first.apply_chat_template(messages, **kwargs)
            return ids, first.decode(ids)
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as workers:
            outputs = list(workers.map(exercise, range(100)))
        self.assertTrue(all(ids == expected and text == fresh.decode(expected) for ids, text in outputs))
        with self.assertRaises(RuntimeError):
            first.add_tokens(["forbidden"])
        with self.assertRaises(RuntimeError):
            first.chat_template = "changed"

    def test_transient_deserialize_retries_only_once(self):
        from inferdeck_vllm_radiance_tokenizer import ImmutablePooledTokenizer
        from vllm.tokenizers.hf import CachedHfTokenizer
        revision = artifact_revision(str(MODEL))
        original = CachedHfTokenizer.from_pretrained
        calls = 0

        def fail_first(*args, **kwargs):
            nonlocal calls
            calls += 1
            if calls == 1:
                raise ValueError("Error while attempting to unpickle Tokenizer: transient")
            return original(*args, **kwargs)

        with mock.patch.object(CachedHfTokenizer, "from_pretrained", side_effect=fail_first):
            tokenizer = ImmutablePooledTokenizer.from_pretrained(
                str(MODEL), revision=revision, truncation_side="left")
        self.assertEqual(calls, 2)
        self.assertTrue(tokenizer.is_fast)
        self.assertEqual(tokenizer.encode("Hello"), original(
            str(MODEL), local_files_only=True, use_fast=True,
            truncation_side="left").encode("Hello"))

        with mock.patch.object(CachedHfTokenizer, "from_pretrained",
                               side_effect=RuntimeError("unrelated failure")) as loader:
            with self.assertRaisesRegex(RuntimeError, "unrelated failure"):
                ImmutablePooledTokenizer.from_pretrained(
                    str(MODEL), revision=revision, truncation_side="left")
            self.assertEqual(loader.call_count, 1)

        with mock.patch.object(CachedHfTokenizer, "from_pretrained",
                               side_effect=ValueError(
                                   "Error while attempting to unpickle Tokenizer: persistent")) as loader:
            with self.assertRaisesRegex(ValueError, "persistent"):
                ImmutablePooledTokenizer.from_pretrained(
                    str(MODEL), revision=revision, truncation_side="left")
            self.assertEqual(loader.call_count, 2)

    def test_artifact_revision_changes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            shutil.copy2(MODEL / "tokenizer.json", root / "tokenizer.json")
            initial = artifact_revision(str(root))
            (root / "tokenizer_config.json").write_text('{}', encoding="utf-8")
            configured = artifact_revision(str(root))
            self.assertNotEqual(initial, configured)
            (root / "chat_template.jinja").write_text('changed', encoding="utf-8")
            self.assertNotEqual(configured, artifact_revision(str(root)))

if __name__ == "__main__":
    unittest.main()
