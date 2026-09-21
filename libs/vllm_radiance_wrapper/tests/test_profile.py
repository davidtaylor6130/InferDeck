import os
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace as NS
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
import inferdeck_vllm_radiance_profile as profile


class ProfileTests(unittest.TestCase):
    def test_digest_is_not_a_path(self):
        with tempfile.TemporaryDirectory() as directory:
            config = {key: directory for key in profile._REQUIRED}
            config.update(runtime="vllm_radiance", context_size=106496,
                          n_slots=1, min_slots=1, prefill_dll_sha256="a" * 64)
            profile.validate_config(config)
            config["prefill_dll_sha256"] = "not-a-digest"
            with self.assertRaisesRegex(RuntimeError, "SHA-256"):
                profile.validate_config(config)

    def test_configures_pinned_native_compiler(self):
        with tempfile.TemporaryDirectory() as directory:
            compiler = Path(directory) / "_rocm_sdk_core" / "lib" / "llvm" / "bin" / "clang-cl.exe"
            compiler.parent.mkdir(parents=True)
            compiler.write_bytes(b"pinned compiler")
            with patch.dict(os.environ, {"CC": "old-compiler"}):
                configured = profile._configure_native_compiler(directory)
                self.assertEqual(configured, str(compiler))
                self.assertEqual(os.environ["CC"], str(compiler))
    def test_registers_actual_qwen_model_class_in_process(self):
        captured = {}

        class ActualModel:
            pass

        class Registry:
            @staticmethod
            def register_model(model_arch, model_cls):
                captured["model_arch"] = model_arch
                captured["model_cls"] = model_cls

        modules = {
            "vllm.model_executor.models": NS(ModelRegistry=Registry),
            "vllm.model_executor.models.qwen3_5": NS(
                Qwen3_5ForConditionalGeneration=ActualModel
            ),
        }
        with patch.dict(sys.modules, modules):
            registered = profile._register_qwen35_model_class()

        self.assertIs(registered, ActualModel)
        self.assertEqual(captured["model_arch"], "Qwen3_5ForConditionalGeneration")
        self.assertIs(captured["model_cls"], ActualModel)
        self.assertNotIsInstance(captured["model_cls"], str)
    def test_begin_uses_adjusted_request_sampling_and_reserves_context_budget(self):
        captured = {}
        delta_kind = object()
        structured_outputs = {"json": {"type": "object"}}

        class Request:
            def __init__(self, **kwargs):
                self.__dict__.update(kwargs)
                self.tools = kwargs["tools"]

            def to_sampling_params(self, maximum, defaults):
                captured["maximum"] = maximum
                captured["defaults"] = defaults
                result = NS(output_kind=None, structured_outputs=self.structured_outputs,
                            stop=self.stop)
                captured["sampling"] = result
                return result

        class ToolParser:
            def __init__(self, tokenizer, tools):
                captured["parser_tools"] = tools

            def adjust_request(self, request):
                captured["adjusted_request"] = request
                return request

        modules = {
            "vllm.entrypoints.openai.chat_completion.protocol": NS(ChatCompletionRequest=Request),
            "vllm.parser.qwen3": NS(Qwen3Parser=lambda *a, **kw: NS()),
            "vllm.sampling_params": NS(RequestOutputKind=NS(DELTA=delta_kind), StructuredOutputsParams=lambda **kw: NS(**kw)),
            "vllm.tool_parsers.structural_tag_registry": NS(get_model_structural_tag=lambda *args, **kw: NS(model_dump=lambda: {"format": "qwen"})),
            "vllm.tool_parsers.qwen3_engine_tool_parser": NS(Qwen3EngineToolParser=ToolParser),
        }

        def tokenize(*args, **kwargs):
            self.assertIs(kwargs.get("return_dict"), False)
            return list(range(10))

        def add_request(request_id, ids, sampling, **kwargs):
            captured["engine_sampling"] = sampling
            captured["engine_ids"] = ids

        state = {"tokenizer": NS(apply_chat_template=tokenize),
                 "engine": NS(add_request=add_request), "active": set(), "requests": {}}
        request = {"model": "qwen", "messages": [], "tools": [{"type": "function"}],
                   "tool_choice": {"type": "function", "function": {"name": "lookup"}},
                   "sampling": {}, "structured_outputs": structured_outputs,
                   "stop": ["END"], "max_output_tokens": -1}
        with patch.dict(sys.modules, modules):
            profile.begin(state, request)
            self.assertEqual(captured["maximum"], 106486)
            self.assertEqual(captured["defaults"], {})
            self.assertIs(captured["engine_sampling"], captured["sampling"])
            self.assertIs(captured["sampling"].output_kind, delta_kind)
            self.assertEqual(captured["sampling"].structured_outputs.structural_tag, '{"format": "qwen"}')
            self.assertEqual(captured["sampling"].stop, ["END"])
            self.assertIs(captured["adjusted_request"], state["requests"][next(iter(state["requests"]))]["request"])
            request["max_output_tokens"] = 106487
            with self.assertRaisesRegex(RuntimeError, "exceeds configured context"):
                profile.begin(state, request)

    def test_stream_counts_delta_tokens_and_tool_finish(self):
        seen = []
        class Parser:
            def parse_delta(self, text, tokens, *args, **kwargs):
                seen.append((text, tokens))
                return NS(content="", reasoning="", tool_calls=[
                    NS(index=0, id="call" if len(seen) == 1 else "", type="function",
                       function=NS(name="lookup" if len(seen) == 1 else "", arguments=text))])
        class Engine:
            def step(self):
                last = bool(seen)
                return [NS(request_id="r", finished=last, prompt_token_ids=[1, 2],
                           num_cached_tokens=1, metrics=NS(scheduled_ts=10.0, first_token_ts=11.0, last_token_ts=13.0), outputs=[NS(text='}' if last else '{"x":1',
                           token_ids=[5] if last else [3, 4], finish_reason="stop" if last else None)])]
        state = {"engine": Engine(), "active": {"r"}, "requests": {
            "r": {"parser": Parser(), "request": object(), "ids": [1, 2],
                  "completion_tokens": 0, "had_tools": False}}}
        first = profile.step(state, "r", {})[0]
        final = profile.step(state, "r", {})[0]
        self.assertEqual(first["completion_tokens"], 2)
        self.assertEqual(final["completion_tokens"], 3)
        self.assertEqual(final["prompt_duration_ms"], 1000.0)
        self.assertEqual(final["generation_duration_ms"], 2000.0)
        self.assertEqual(final["finish_reason"], "tool_calls")
        self.assertEqual(seen, [('{"x":1', [3, 4]), ('}', [5])])
        self.assertFalse(state["active"])
        self.assertFalse(state["requests"])

    def test_post_state_cleanup_releases_unused_pinned_host_cache(self):
        calls = []
        torch = NS(cuda=NS(synchronize=lambda: calls.append("sync"),
            empty_cache=lambda: calls.append("device_empty"),
            memory=NS(host_memory_stats=lambda: {"allocated_bytes.current": 0}),
            memory_allocated=lambda: 0, memory_reserved=lambda: 0),
            accelerator=NS(memory=NS(empty_host_cache=lambda: calls.append("host_empty"))))
        with patch.dict(sys.modules, {"torch": torch}), patch.object(profile, "_RELEASED_SNAPSHOT", None):
            profile.collect_released_resources()
        self.assertEqual(calls, ["sync", "device_empty", "host_empty"])

    def test_cleanup_attempts_remaining_steps_after_failure(self):
        calls = []
        class Core:
            def shutdown(self):
                calls.append("shutdown")
                raise RuntimeError("injected engine failure")
        class Context:
            def __exit__(self, *args):
                calls.append("overlay_exit")
        state = {"engine": NS(engine_core=Core()), "active": set(),
                 "requests": {}, "contexts": (Context(), Context())}
        torch = NS(cuda=NS(synchronize=lambda: calls.append("sync"),
                          empty_cache=lambda: calls.append("empty")))
        with patch.dict(sys.modules, {"torch": torch}):
            with self.assertRaisesRegex(RuntimeError, "injected engine failure"):
                profile.shutdown(state)
        self.assertIsNone(state["engine"])
        self.assertEqual(calls, ["shutdown", "overlay_exit", "overlay_exit", "sync", "empty"])


if __name__ == "__main__":
    unittest.main()
