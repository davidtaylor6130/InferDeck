import json
import math
import os
import sys
import tempfile
import threading
import time
import unittest
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from types import SimpleNamespace as NS
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
import inferdeck_vllm_radiance_profile as profile


class ProfileTests(unittest.TestCase):
    def test_idle_vulkan_cleanup_uses_loaded_backend_and_propagates_failure(self):
        from unittest.mock import Mock
        get_module = Mock(return_value=123)
        release = Mock(return_value=1)
        kernel = NS(GetModuleHandleW=get_module)
        backend = NS(inferdeck_ggml_vk_release_idle_cache=release)
        with patch.object(profile.os, "name", "nt"), patch.object(profile.ctypes, "WinDLL", create=True, side_effect=[kernel, backend]) as load:
            self.assertEqual(profile._release_idle_vulkan_cache(), 1)
            get_module.assert_called_once_with("ggml-vulkan.dll")
            self.assertEqual(load.call_args_list[1].kwargs, {"handle": 123})
            release.assert_called_once_with()
        release.return_value = -1
        with patch.object(profile.os, "name", "nt"), patch.object(profile.ctypes, "WinDLL", create=True, side_effect=[kernel, backend]):
            with self.assertRaisesRegex(RuntimeError, "cleanup failed"):
                profile._release_idle_vulkan_cache()

    def test_idle_vulkan_cleanup_skips_missing_backend_or_older_export(self):
        from unittest.mock import Mock
        for handle, backend in ((None, None), (123, NS())):
            with self.subTest(handle=handle), patch.object(profile.os, "name", "nt"), patch.object(profile.ctypes, "WinDLL", create=True, side_effect=[NS(GetModuleHandleW=Mock(return_value=handle)), backend]) as load:
                self.assertEqual(profile._release_idle_vulkan_cache(), 0)
                self.assertEqual(load.call_count, 1 if handle is None else 2)

    def test_hillclimb_sampling_uses_greedy_temperature_and_keeps_request_controls(self):
        request = {"reasoning_effort": "medium", "max_output_tokens": 16384,
                   "sampling": {"temperature": 0.8, "top_p": 0.95, "top_k": 40, "seed": 42}}
        selected = profile._request_sampling_profile({"hillclimb_sampling": True},request)
        self.assertEqual(selected["sampling"]["temperature"], 0.0)
        self.assertEqual(selected["sampling"]["top_p"], 1.0)
        self.assertEqual(selected["sampling"]["top_k"], 20)
        self.assertEqual(selected["sampling"]["seed"], 42)
        self.assertEqual(selected["reasoning_effort"], "medium")
        self.assertEqual(selected["max_output_tokens"], 16384)
        self.assertEqual(request["sampling"]["top_p"], 0.95)
        self.assertIs(profile._request_sampling_profile({},request),request)

    def test_template_messages_combine_instructions_before_conversation(self):
        messages = [
            {"role": "developer", "content": "developer rules"},
            {"role": "user", "content": "first turn"},
            {"role": "assistant", "content": "answer"},
            {"role": "system", "content": "system rules"},
            {"role": "system", "content": "later system rules"},
            {"role": "user", "content": "second turn"},
        ]
        result = profile._template_messages(messages)
        self.assertEqual(result, [
            {"role": "system", "content": "system rules\n\nlater system rules\n\ndeveloper rules"},
            {"role": "user", "content": "first turn"},
            {"role": "assistant", "content": "answer"},
            {"role": "user", "content": "second turn"},
        ])
        result[1]["content"] = "changed"
        self.assertEqual(messages[1]["content"], "first turn")
        self.assertEqual(profile._template_messages(messages[1:2]), messages[1:2])

    def test_pal_resource_cache_is_bounded_before_runtime_dependencies(self):
        for explicit in (None, "128"):
            with self.subTest(explicit=explicit):
                environment = {} if explicit is None else {"GPU_RESOURCE_CACHE_SIZE": explicit}
                def dependency_boundary(*args):
                    self.assertEqual(environment.get("GPU_RESOURCE_CACHE_SIZE"), explicit or "64")
                    raise RuntimeError("dependency boundary reached")
                with patch.object(profile, "os", NS(environ=environment)), patch.object(profile, "validate_config"), patch.object(profile, "_need", side_effect=dependency_boundary):
                    with self.assertRaisesRegex(RuntimeError, "dependency boundary reached"):
                        profile._create({})

    def test_create_returns_engine_and_propagates_load_failure(self):
        with patch.object(profile, "_create", return_value={"engine": "loaded"}) as create:
            self.assertEqual(profile.create({}), {"engine": "loaded"})
            create.assert_called_once_with({})
        with patch.object(profile, "_create", side_effect=RuntimeError("engine load failed")):
            with self.assertRaisesRegex(RuntimeError, "engine load failed"):
                profile.create({})

    def test_r4d_retention_scope_restores_environment(self):
        key = "VLLM_PREFIX_CACHE_RETENTION_INTERVAL"
        with patch.dict(os.environ, {key: "512"}):
            with profile._r4d_cache_retention("r4d"):
                self.assertEqual(os.environ[key], "0")
            self.assertEqual(os.environ[key], "512")
            with self.assertRaisesRegex(RuntimeError, "engine load failed"):
                with profile._r4d_cache_retention("r4d"):
                    raise RuntimeError("engine load failed")
            self.assertEqual(os.environ[key], "512")
            with profile._r4d_cache_retention("upstream"):
                self.assertEqual(os.environ[key], "512")
            with profile._r4d_cache_retention("r4d_int4"):
                self.assertEqual(os.environ[key], "0")
        with patch.dict(os.environ, {}, clear=False):
            previous = os.environ.pop(key, None)
            try:
                with profile._r4d_cache_retention("r4d"):
                    self.assertEqual(os.environ[key], "0")
                self.assertNotIn(key, os.environ)
            finally:
                if previous is not None:
                    os.environ[key] = previous

    def test_profile_uses_fixed_memory_default_and_rejects_removed_override(self):
        self.assertEqual(profile._gpu_memory_utilization("r4d"), 0.925)
        for attention in ("r4d_int4", "upstream"):
            self.assertEqual(profile._gpu_memory_utilization(attention), 0.90)
        with self.assertRaisesRegex(RuntimeError, "not a supported profile setting"):
            profile.validate_config({"runtime": "vllm_radiance", "context_size": 106496,
                "n_slots": 1, "min_slots": 1, "gpu_memory_utilization": 0.80})

    def test_int4_accepts_up_to_four_slots_and_other_profiles_remain_single_slot(self):
        base = {"runtime": "vllm_radiance", "context_size": 106496,
                "prefill_attention": "r4d_int4", "kv_cache_dtype": "int4_per_token_head",
                "prefill_dll_sha256": "a" * 64, "decode_dll": "decode.dll",
                "decode_dll_sha256": "b" * 64}
        with tempfile.TemporaryDirectory() as directory, patch.object(profile, "_need", return_value=directory):
            for slots in range(1, 5):
                with self.subTest(slots=slots):
                    profile.validate_config(dict(base, n_slots=slots, min_slots=slots))
            for n_slots, min_slots in ((0, 0), (1, 0), (1, 2), (5, 5), (2, 1), (2, 3)):
                with self.subTest(n_slots=n_slots, min_slots=min_slots), self.assertRaisesRegex(RuntimeError, "min_slots == n_slots"):
                    profile.validate_config(dict(base, n_slots=n_slots, min_slots=min_slots))
            for attention in ("r4d", "upstream"):
                config = dict(base, prefill_attention=attention, kv_cache_dtype="auto",
                              n_slots=2, min_slots=1)
                with self.subTest(attention=attention), self.assertRaisesRegex(RuntimeError, "min_slots == n_slots"):
                    profile.validate_config(config)

    def test_four_bit_cache_requires_matching_attention(self):
        config = {"runtime": "vllm_radiance", "context_size": 106496,
                  "n_slots": 1, "min_slots": 1,
                  "kv_cache_dtype": "int4_per_token_head"}
        with self.assertRaisesRegex(RuntimeError, "select r4d_int4"):
            profile.validate_config(config)
        config["prefill_attention"] = "upstream"
        with patch.object(profile, "_need", return_value="available"):
            profile.validate_config(config)
        config["prefill_attention"] = "r4d_int4"
        with tempfile.TemporaryDirectory() as directory, patch.object(profile, "_need", return_value=directory):
            config.update({key: directory for key in profile._REQUIRED})
            config["prefill_dll_sha256"] = "a" * 64
            with self.assertRaisesRegex(RuntimeError, "requires decode_dll"):
                profile.validate_config(config)
            config.update(decode_dll=directory, decode_dll_sha256="b" * 64)
            profile.validate_config(config)
            for partial in ({"decode_dll": directory}, {"decode_dll_sha256": "b" * 64}):
                invalid = dict(config)
                invalid.pop("decode_dll", None)
                invalid.pop("decode_dll_sha256", None)
                invalid.update(partial)
                with self.assertRaisesRegex(RuntimeError, "configured together"):
                    profile.validate_config(invalid)
            invalid = dict(config, decode_dll_sha256="invalid")
            with self.assertRaisesRegex(RuntimeError, "decode_dll_sha256"):
                profile.validate_config(invalid)
            invalid = dict(config, prefill_attention="r4d", kv_cache_dtype="auto")
            with self.assertRaisesRegex(RuntimeError, "require prefill_attention r4d_int4"):
                profile.validate_config(invalid)
        config["kv_cache_dtype"] = "q4_0"
        with self.assertRaisesRegex(RuntimeError, "kv_cache_dtype must be one of"):
            profile.validate_config(config)

    def test_r4d_int4_requires_int4_cache_and_retains_decode_graph(self):
        with self.assertRaisesRegex(RuntimeError, "requires kv_cache_dtype int4_per_token_head"):
            profile.validate_config({"runtime": "vllm_radiance", "context_size": 106496,
                "n_slots": 1, "min_slots": 1, "prefill_attention": "r4d_int4"})
        self.assertEqual(profile._compilation_config("int4_per_token_head", "r4d_int4"), {
            "mode": 0, "cudagraph_mode": "FULL_DECODE_ONLY", "cudagraph_capture_sizes": [1]})
        self.assertEqual(profile._compilation_config("int4_per_token_head", "upstream"), {
            "mode": 0, "cudagraph_mode": "NONE"})
        self.assertEqual(profile._compilation_config("auto", "r4d"), {
            "mode": 0, "cudagraph_mode": "FULL_DECODE_ONLY", "cudagraph_capture_sizes": [1]})

    def test_prefill_overlay_selection_calls_int4_installer_with_pinned_artifact(self):
        calls = []
        expected = object()
        overlay = NS(
            install_r4d_prefill_overlay=lambda *args, **kwargs: calls.append(("r4d", args, kwargs)) or expected,
            install_int4_prefill_overlay=lambda *args, **kwargs: calls.append(("int4", args, kwargs)) or expected)
        config = {"prefill_overlay": "overlay.py", "prefill_dll": "kernel.dll",
                  "prefill_dll_sha256": "a" * 64, "decode_dll": "decode.dll",
                  "decode_dll_sha256": "b" * 64}
        with patch.object(profile, "_need", side_effect=lambda config, key: config[key]), \
             patch.object(profile, "_verify_upstream_prefill_attention", side_effect=lambda: calls.append(("verify", (), {}))), \
             patch.object(profile, "_load", side_effect=lambda *args: calls.append(("load", args, {})) or overlay):
            selected = profile._install_prefill_overlay(config, "r4d_int4")
        self.assertIs(selected, expected)
        self.assertEqual(calls, [("verify", (), {}), ("load", ("inferdeck_prefill", "overlay.py"), {}),
            ("int4", (Path("kernel.dll"),), {
                "compare_reference": False, "expected_dll_sha256": "a" * 64,
                "decode_dll_path": Path("decode.dll"), "expected_decode_dll_sha256": "b" * 64,
                "decode_fallback_to_upstream": False})])

    def test_prefill_overlay_passes_opt_in_int4_decode_artifact(self):
        calls = []
        overlay = NS(install_int4_prefill_overlay=lambda *args, **kwargs: calls.append((args, kwargs)))
        config = {"prefill_overlay": "overlay.py", "prefill_dll": "prefill.dll",
                  "prefill_dll_sha256": "a" * 64, "decode_dll": "decode.dll",
                  "decode_dll_sha256": "b" * 64}
        with patch.object(profile, "_need", side_effect=lambda config, key: config[key]), \
             patch.object(profile, "_verify_upstream_prefill_attention"), \
             patch.object(profile, "_load", return_value=overlay):
            profile._install_prefill_overlay(config, "r4d_int4")
        self.assertEqual(calls, [((Path("prefill.dll"),), {
            "compare_reference": False, "expected_dll_sha256": "a" * 64,
            "decode_dll_path": Path("decode.dll"), "expected_decode_dll_sha256": "b" * 64,
            "decode_fallback_to_upstream": False})])

    def test_kv_cache_token_capacity_uses_limiting_group(self):
        cache_config = NS(num_blocks=700, kv_cache_groups=[
            NS(kv_cache_spec=NS(block_size=160)),
            NS(kv_cache_spec=NS(block_size=152))])
        self.assertEqual(profile._kv_cache_token_capacity(cache_config), 106400)
        cache_config.num_blocks = 701
        self.assertEqual(profile._kv_cache_token_capacity(cache_config), 106552)
        with self.assertRaisesRegex(RuntimeError, "invalid block capacity"):
            profile._kv_cache_token_capacity(NS(num_blocks=700, kv_cache_groups=[]))

    def test_kv_cache_concurrency_sums_rounded_group_block_demands(self):
        class Spec:
            def __init__(self, memory_usage, page_size):
                self.memory_usage = memory_usage
                self.page_size_bytes = page_size

            def max_memory_usage_bytes(self, vllm_config):
                return self.memory_usage

        cache_config = NS(num_blocks=170, kv_cache_groups=[
            NS(kv_cache_spec=Spec(240, 10)),
            NS(kv_cache_spec=Spec(171, 10))])
        concurrency = profile._kv_cache_max_concurrency(cache_config, object())
        self.assertAlmostEqual(concurrency, 170 / 42)
        self.assertGreaterEqual(concurrency, 4)
        self.assertLess(concurrency, 4.1)
        with self.assertRaisesRegex(RuntimeError, "invalid block capacity"):
            profile._kv_cache_max_concurrency(NS(num_blocks=0, kv_cache_groups=[]), object())

    def _capture_patches(self, directory):
        return (
            patch.object(profile, "_CAPTURE_MARKER_PATH", Path(directory) / "capture-next-request.json"),
            patch.object(profile, "_CAPTURE_OUTPUT_PATH", Path(directory) / "captured-request.json"),
        )

    def test_request_capture_ignores_absent_marker(self):
        with tempfile.TemporaryDirectory() as directory:
            marker_patch, output_patch = self._capture_patches(directory)
            with marker_patch, output_patch:
                profile._capture_next_request({"messages": [{"content": "private"}]}, [1, 2])
            self.assertFalse((Path(directory) / "captured-request.json").exists())

    def test_request_capture_consumes_expired_marker(self):
        with tempfile.TemporaryDirectory() as directory:
            marker = Path(directory) / "capture-next-request.json"
            marker.write_text(json.dumps({"expires_at": time.time() - 1}), encoding="utf-8")
            marker_patch, output_patch = self._capture_patches(directory)
            with marker_patch, output_patch:
                profile._capture_next_request({"messages": []}, [1])
            self.assertFalse(marker.exists())
            self.assertFalse((Path(directory) / "captured-request.json").exists())

    def test_request_capture_is_one_shot_and_preserves_existing_output(self):
        with tempfile.TemporaryDirectory() as directory:
            marker = Path(directory) / "capture-next-request.json"
            output = Path(directory) / "captured-request.json"
            marker.write_text(json.dumps({"expires_at": time.time() + 60}), encoding="utf-8")
            marker_patch, output_patch = self._capture_patches(directory)
            with marker_patch, output_patch:
                profile._capture_next_request({"messages": []}, [7, 8])
                first = output.read_text(encoding="utf-8")
                marker.write_text(json.dumps({"expires_at": time.time() + 60}), encoding="utf-8")
                profile._capture_next_request({"messages": []}, [9])
            self.assertEqual(json.loads(first)["rendered_ids"], [7, 8])
            self.assertEqual(output.read_text(encoding="utf-8"), first)
            self.assertFalse(marker.exists())

    def test_request_capture_rejects_nan_and_malformed_markers(self):
        with tempfile.TemporaryDirectory() as directory:
            marker = Path(directory) / "capture-next-request.json"
            marker.write_text(json.dumps({"expires_at": float("nan")}), encoding="utf-8")
            marker_patch, output_patch = self._capture_patches(directory)
            with marker_patch, output_patch:
                profile._capture_next_request({"messages": []}, [1])
            self.assertFalse(marker.exists())
            self.assertFalse((Path(directory) / "captured-request.json").exists())
            marker.write_text("not-json", encoding="utf-8")
            with marker_patch, output_patch:
                profile._capture_next_request({"messages": []}, [2])
            self.assertTrue(marker.exists())
            self.assertFalse((Path(directory) / "captured-request.json").exists())

    def test_request_capture_rejects_marker_over_ten_minutes(self):
        with tempfile.TemporaryDirectory() as directory:
            marker = Path(directory) / "capture-next-request.json"
            marker.write_text(json.dumps({"expires_at": time.time() + 601}), encoding="utf-8")
            marker_patch, output_patch = self._capture_patches(directory)
            with marker_patch, output_patch:
                profile._capture_next_request({"messages": []}, [1])
            self.assertFalse(marker.exists())
            self.assertFalse((Path(directory) / "captured-request.json").exists())

    def test_prefill_attention_defaults_to_r4d_and_upstream_skips_custom_artifacts(self):
        with tempfile.TemporaryDirectory() as directory:
            config = {key: directory for key in profile._REQUIRED}
            config.update(runtime="vllm_radiance", context_size=106496,
                          n_slots=1, min_slots=1, prefill_dll_sha256="a" * 64)
            profile.validate_config(config)
            upstream = dict(config, prefill_attention="upstream")
            for key in ("prefill_overlay", "prefill_dll", "prefill_dll_sha256"):
                upstream.pop(key)
            profile.validate_config(upstream)
            int4 = dict(config, prefill_attention="r4d_int4", kv_cache_dtype="int4_per_token_head")
            int4.update(decode_dll=directory, decode_dll_sha256="b" * 64)
            profile.validate_config(int4)

    def test_invalid_prefill_attention_is_rejected_before_loading(self):
        config = {"runtime": "vllm_radiance", "context_size": 106496,
                  "n_slots": 1, "min_slots": 1, "prefill_attention": "other"}
        with patch.object(profile, "_load", side_effect=AssertionError("load happened")):
            with self.assertRaisesRegex(RuntimeError, "prefill_attention"):
                profile.create(config)

    def test_upstream_attention_requires_expected_callable(self):
        with patch.dict(sys.modules, {
            "vllm.v1.attention.ops.triton_unified_attention": NS(unified_attention=object()),
            "vllm.v1.attention.backends.triton_attn": NS(unified_attention=object()),
        }):
            with self.assertRaisesRegex(RuntimeError, "expected callable"):
                profile._verify_upstream_prefill_attention()

    def test_upstream_attention_rejects_stale_backend_binding(self):
        def upstream_attention(*args, **kwargs):
            return None
        upstream_attention.__module__ = "vllm.v1.attention.ops.triton_unified_attention"
        def stale_attention(*args, **kwargs):
            return None
        with patch.dict(sys.modules, {
            "vllm.v1.attention.ops.triton_unified_attention": NS(unified_attention=upstream_attention),
            "vllm.v1.attention.backends.triton_attn": NS(unified_attention=stale_attention),
        }):
            with self.assertRaisesRegex(RuntimeError, "unexpected unified_attention binding"):
                profile._verify_upstream_prefill_attention()

    def test_upstream_attention_accepts_matching_backend_binding_without_custom_hook(self):
        def upstream_attention(*args, **kwargs):
            return None
        upstream_attention.__module__ = "vllm.v1.attention.ops.triton_unified_attention"
        upstream = NS(unified_attention=upstream_attention)
        with patch.dict(sys.modules, {
            "vllm.v1.attention.ops.triton_unified_attention": upstream,
            "vllm.v1.attention.backends.triton_attn": NS(unified_attention=upstream_attention),
        }):
            with patch.object(profile, "_load", side_effect=AssertionError("custom hook loaded")):
                self.assertIs(profile._verify_upstream_prefill_attention(), upstream_attention)

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

        class DraftModel:
            pass

        class Registry:
            @staticmethod
            def register_model(model_arch, model_cls):
                captured.setdefault("all", []).append((model_arch, model_cls))

        modules = {
            "vllm.model_executor.models": NS(ModelRegistry=Registry),
            "vllm.model_executor.models.qwen3_5": NS(
                Qwen3_5ForConditionalGeneration=ActualModel
            ),
            "vllm.model_executor.models.qwen3_5_mtp": NS(
                Qwen3_5MTP=DraftModel, Qwen3_5MoeMTP=DraftModel
            ),
        }
        with patch.dict(sys.modules, modules):
            registered = profile._register_qwen35_model_class()

        self.assertIs(registered, ActualModel)
        self.assertIn(("Qwen3_5ForConditionalGeneration", ActualModel), captured["all"])
        for arch, cls in captured["all"]:
            self.assertNotIsInstance(cls, str)
        self.assertEqual(
            [arch for arch, _ in captured["all"] if arch in ("Qwen3_5MTP", "Qwen3_5MoeMTP")],
            ["Qwen3_5MTP", "Qwen3_5MoeMTP"],
            "MTP draft heads must be registered so vLLM never inspects them in a subprocess")
    def test_begin_routes_image_messages_through_vllm_multimodal_renderer(self):
        captured = {}
        engine_input = {"type": "multimodal", "prompt_token_ids": [10, 11],
                        "mm_kwargs": {"image": object()}, "mm_hashes": {},
                        "mm_placeholders": {"image": []}}

        class Request:
            def __init__(self, **kwargs):
                self.__dict__.update(kwargs)
                self.tools = kwargs["tools"]
                self.response_format = kwargs.get("response_format")

            def to_sampling_params(self, maximum, defaults):
                return NS(output_kind=None, structured_outputs=None, stop=[])

        class ToolParser:
            def __init__(self, tokenizer, tools):
                pass

            def adjust_request(self, request):
                return request

        def render_chat(conversations, params):
            captured["messages"] = conversations[0]
            captured["params"] = params
            return ([[]], [engine_input])

        class ChatParams:
            def __init__(self, **kwargs):
                self.__dict__.update(kwargs)

        modules = {
            "vllm.entrypoints.openai.chat_completion.protocol": NS(ChatCompletionRequest=Request),
            "vllm.parser.qwen3": NS(Qwen3Parser=lambda *a, **kw: NS()),
            "vllm.sampling_params": NS(RequestOutputKind=NS(DELTA=object()), StructuredOutputsParams=lambda **kw: NS(**kw)),
            "vllm.tool_parsers.structural_tag_registry": NS(get_model_structural_tag=lambda *args, **kw: None),
            "vllm.tool_parsers.qwen3_engine_tool_parser": NS(Qwen3EngineToolParser=ToolParser),
            "vllm.renderers": NS(ChatParams=ChatParams),
        }

        def tokenize(*args, **kwargs):
            self.fail("image requests must use the multimodal renderer")

        def add_request(request_id, prompt, sampling, **kwargs):
            captured["engine_input"] = prompt

        state = {"tokenizer": NS(apply_chat_template=tokenize),
                 "engine": NS(renderer=NS(render_chat=render_chat), add_request=add_request),
                 "engine_lock": threading.RLock(), "active": set(), "requests": {}}
        request = {"model": "qwen", "messages": [{"role": "user", "content": [
                       {"type": "text", "text": "Describe this."},
                       {"type": "image_url", "image_url": {"url": "data:image/png;base64,AA=="}},
                   ]}], "sampling": {}, "max_output_tokens": 8}
        with patch.dict(sys.modules, modules):
            profile.begin(state, request)

        self.assertIs(captured["engine_input"], engine_input)
        self.assertEqual(captured["messages"], request["messages"])
        self.assertTrue(captured["params"].chat_template_kwargs["tokenize"])
        self.assertEqual(state["requests"][next(iter(state["requests"]))]["ids"], [10, 11])

    def test_begin_uses_adjusted_request_sampling_and_reserves_context_budget(self):
        captured = {}
        engine_lock = threading.Lock()
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
            self.assertTrue(engine_lock.locked(), "request tokenization must exclude engine stepping")
            self.assertIs(kwargs.get("return_dict"), False)
            return list(range(10))

        def add_request(request_id, ids, sampling, **kwargs):
            captured["engine_sampling"] = sampling
            captured["engine_ids"] = ids

        state = {"tokenizer": NS(apply_chat_template=tokenize),
                 "engine": NS(add_request=add_request), "engine_lock": engine_lock, "active": set(), "requests": {}}
        request = {"model": "qwen", "messages": [], "tools": [{"type": "function"}],
                   "tool_choice": {"type": "function", "function": {"name": "lookup"}},
                   "sampling": {"repetition_penalty": 1.2, "frequency_penalty": 0.5, "presence_penalty": 0.25}, "repeat_last_n": 64, "structured_outputs": structured_outputs,
                   "stop": ["END"], "max_output_tokens": -1}
        with patch.dict(sys.modules, modules):
            profile.begin(state, request)
            self.assertEqual(captured["maximum"], 106486)
            self.assertEqual(captured["defaults"], {})
            self.assertIs(captured["engine_sampling"], captured["sampling"])
            self.assertIs(captured["sampling"].output_kind, delta_kind)
            self.assertEqual(captured["sampling"].repetition_penalty, 1.0)
            self.assertEqual(captured["sampling"].frequency_penalty, 0.0)
            self.assertEqual(captured["sampling"].presence_penalty, 0.0)
            self.assertEqual(captured["sampling"].extra_args["inferdeck_penalties"], {
                "repeat_last_n": 64, "repetition_penalty": 1.2,
                "frequency_penalty": 0.5, "presence_penalty": 0.25})
            self.assertEqual(captured["sampling"].structured_outputs.structural_tag, '{"format": "qwen"}')
            self.assertEqual(captured["sampling"].stop, ["END"])
            self.assertIs(captured["adjusted_request"], state["requests"][next(iter(state["requests"]))]["request"])
            request["max_output_tokens"] = 106487
            with self.assertRaisesRegex(RuntimeError, "exceeds configured context"):
                profile.begin(state, request)

    def test_begin_forwards_reasoning_effort_to_vllm_and_qwen_template(self):
        parser_calls = []
        template_calls = []
        request_calls = []

        class Request:
            def __init__(self, **kwargs):
                request_calls.append(kwargs)
                self.__dict__.update(kwargs)
                self.tools = kwargs["tools"]

            def to_sampling_params(self, maximum, defaults):
                return NS(output_kind=None, structured_outputs=None, stop=[])

        class ToolParser:
            def __init__(self, tokenizer, tools):
                pass

            def adjust_request(self, request):
                return request

        def parser(*args, **kwargs):
            parser_calls.append(kwargs)
            return NS()

        modules = {
            "vllm.entrypoints.openai.chat_completion.protocol": NS(ChatCompletionRequest=Request),
            "vllm.parser.qwen3": NS(Qwen3Parser=parser),
            "vllm.sampling_params": NS(RequestOutputKind=NS(DELTA=object()), StructuredOutputsParams=lambda **kw: NS(**kw)),
            "vllm.tool_parsers.structural_tag_registry": NS(get_model_structural_tag=lambda *args, **kw: None),
            "vllm.tool_parsers.qwen3_engine_tool_parser": NS(Qwen3EngineToolParser=ToolParser),
        }

        def tokenize(*args, **kwargs):
            template_calls.append(kwargs)
            return [1, 2]

        state = {"tokenizer": NS(apply_chat_template=tokenize),
                 "engine": NS(add_request=lambda *args, **kwargs: None), "engine_lock": threading.RLock(), "active": set(), "requests": {}}
        base = {"model": "qwen", "messages": [], "sampling": {}, "max_output_tokens": 1}
        with patch.dict(sys.modules, modules):
            for value in ("absent", None, False, True):
                request = dict(base)
                if value != "absent":
                    request["enable_reasoning"] = value
                profile.begin(state, request)
            profile.begin(state, {**base, "reasoning_effort": "none"})
            for effort in ("low", "medium", "xhigh"):
                profile.begin(state, {**base, "reasoning_effort": effort})

        self.assertEqual([call["chat_template_kwargs"].get("enable_thinking", True)
                          for call in parser_calls], [True, True, False, True, False, True, True, True])
        self.assertEqual([call.get("enable_thinking", True)
                          for call in template_calls], [True, True, False, True, False, True, True, True])
        self.assertNotIn("reasoning_effort", template_calls[4])
        self.assertFalse(request_calls[4]["include_reasoning"])
        self.assertEqual(request_calls[4]["reasoning_effort"], "none")
        self.assertEqual([call.get("reasoning_effort") for call in request_calls[-3:]],
                         ["low", "medium", "xhigh"])
        self.assertEqual([call.get("reasoning_effort") for call in template_calls[-3:]],
                         ["low", "medium", "xhigh"])
        self.assertEqual([call["add_generation_prompt"] for call in template_calls], [True] * 8)
        self.assertEqual([call["tokenize"] for call in template_calls], [True] * 8)
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
        state = {"engine": Engine(), "engine_lock": threading.RLock(), "active": {"r"}, "requests": {
            "r": {"parser": Parser(), "request": object(), "ids": [1, 2],
                  "completion_tokens": 0, "had_tools": False}}}
        first = profile.step(state, "r", {})[0]
        final = profile.step(state, "r", {})[0]
        self.assertEqual(first["completion_tokens"], 2)
        self.assertEqual(first["prompt_duration_ms"], 1000.0)
        self.assertEqual(first["generation_duration_ms"], 2000.0)
        self.assertEqual(final["completion_tokens"], 3)
        self.assertEqual(final["prompt_duration_ms"], 1000.0)
        self.assertEqual(final["generation_duration_ms"], 2000.0)
        self.assertEqual(final["finish_reason"], "tool_calls")
        self.assertEqual(seen, [('{"x":1', [3, 4]), ('}', [5])])
        self.assertFalse(state["active"])
        self.assertFalse(state["requests"])

    def test_step_demultiplexes_concurrent_requests_and_abort_discards_queued_output(self):
        class Parser:
            def parse_delta(self, text, tokens, *args, **kwargs):
                return NS(content=text, reasoning="", tool_calls=[])

        def output(rid, text, done=False):
            return NS(request_id=rid, finished=done, prompt_token_ids=[1],
                      num_cached_tokens=0, metrics=None,
                      outputs=[NS(text=text, token_ids=[2], finish_reason="stop" if done else None)])

        class Engine:
            def __init__(self):
                self.batches = [[output("a", "A"), output("b", "B", True)], []]
                self.aborted = []

            def step(self):
                return self.batches.pop(0)

            def abort_request(self, request_ids):
                self.aborted.extend(request_ids)

        engine = Engine()
        request_state = {rid: {"parser": Parser(), "request": object(), "ids": [1],
                               "completion_tokens": 0, "had_tools": False, "pending": []}
                         for rid in ("a", "b")}
        state = {"engine": engine, "engine_lock": threading.RLock(), "active": {"a", "b"}, "requests": request_state}

        self.assertEqual(profile.step(state, "a", {})[0]["text"], "A")
        self.assertEqual(len(state["requests"]["b"]["pending"]), 1)
        profile.abort(state, "b")
        self.assertEqual(engine.aborted, ["b"])
        self.assertNotIn("b", state["requests"])
        self.assertEqual(profile.step(state, "a", {}), [])

    def test_shared_engine_step_is_serialized_across_slot_threads(self):
        class Engine:
            active_calls = 0
            maximum_calls = 0

            def step(self):
                self.active_calls += 1
                self.maximum_calls = max(self.maximum_calls, self.active_calls)
                time.sleep(0.02)
                self.active_calls -= 1
                return []

        engine = Engine()
        state = {"engine": engine, "engine_lock": threading.RLock(),
                 "active": {"r"}, "requests": {"r": {"pending": []}}}
        with ThreadPoolExecutor(max_workers=4) as pool:
            self.assertEqual(list(pool.map(lambda _: profile.step(state, "r", {}), range(4))), [[], [], [], []])
        self.assertEqual(engine.maximum_calls, 1)

    def test_post_state_cleanup_releases_unused_pinned_host_cache(self):
        calls = []
        torch = NS(cuda=NS(synchronize=lambda: calls.append("sync"),
            empty_cache=lambda: calls.append("device_empty"),
            memory=NS(host_memory_stats=lambda: {"allocated_bytes.current": 0}),
            memory_allocated=lambda: 0, memory_reserved=lambda: 0),
            accelerator=NS(memory=NS(empty_host_cache=lambda: calls.append("host_empty"))),
            compiler=NS(reset=lambda: calls.append("compiler_reset")))
        with patch.dict(sys.modules, {"torch": torch}), patch.object(profile, "_RELEASED_SNAPSHOT", None):
            profile.collect_released_resources()
        self.assertEqual(calls, ["compiler_reset", "sync", "device_empty", "host_empty"])

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

    def test_begin_rejects_sampling_penalties_while_speculative(self):
        class ToolParser:
            def __init__(self, tokenizer, tools):
                del tokenizer, tools

            def adjust_request(self, request):
                return request

        class Request:
            def __init__(self, **kwargs):
                self.__dict__.update(kwargs)
                self.tools = None

            def to_sampling_params(self, maximum, defaults):
                del maximum, defaults
                return NS(output_kind=None, structured_outputs=None, stop=None)

        modules = {
            "vllm.entrypoints.openai.chat_completion.protocol": NS(ChatCompletionRequest=Request),
            "vllm.parser.qwen3": NS(Qwen3Parser=lambda *a, **kw: NS()),
            "vllm.sampling_params": NS(RequestOutputKind=NS(DELTA=object()),
                                       StructuredOutputsParams=lambda **kw: NS(**kw)),
            "vllm.tool_parsers.structural_tag_registry": NS(
                get_model_structural_tag=lambda *a, **kw: NS(model_dump=lambda: {})),
            "vllm.tool_parsers.qwen3_engine_tool_parser": NS(Qwen3EngineToolParser=ToolParser),
        }
        state = {"tokenizer": NS(apply_chat_template=lambda *a, **k: [1, 2, 3]),
                 "engine": NS(add_request=lambda *a, **k: None),
                 "engine_lock": threading.Lock(), "active": set(), "requests": {},
                 "speculative": True}
        request = {"model": "qwen", "messages": [], "sampling": {"repetition_penalty": 1.2},
                   "repeat_last_n": 64, "max_output_tokens": 16}
        with patch.dict(sys.modules, modules):
            with self.assertRaisesRegex(RuntimeError, "penalties are unsupported"):
                profile.begin(state, request)

    def test_begin_allows_default_penalties_while_speculative(self):
        added = []

        class ToolParser:
            def __init__(self, tokenizer, tools):
                del tokenizer, tools

            def adjust_request(self, request):
                return request

        class Request:
            def __init__(self, **kwargs):
                self.__dict__.update(kwargs)
                self.tools = None

            def to_sampling_params(self, maximum, defaults):
                del maximum, defaults
                return NS(output_kind=None, structured_outputs=None, stop=None)

        modules = {
            "vllm.entrypoints.openai.chat_completion.protocol": NS(ChatCompletionRequest=Request),
            "vllm.parser.qwen3": NS(Qwen3Parser=lambda *a, **kw: NS()),
            "vllm.sampling_params": NS(RequestOutputKind=NS(DELTA=object()),
                                       StructuredOutputsParams=lambda **kw: NS(**kw)),
            "vllm.tool_parsers.structural_tag_registry": NS(
                get_model_structural_tag=lambda *a, **kw: NS(model_dump=lambda: {})),
            "vllm.tool_parsers.qwen3_engine_tool_parser": NS(Qwen3EngineToolParser=ToolParser),
        }
        state = {"tokenizer": NS(apply_chat_template=lambda *a, **k: [1, 2, 3]),
                 "engine": NS(add_request=lambda *a, **k: added.append(a)),
                 "engine_lock": threading.Lock(), "active": set(), "requests": {},
                 "speculative": True}
        request = {"model": "qwen", "messages": [], "sampling": {}, "max_output_tokens": 16}
        with patch.dict(sys.modules, modules):
            profile.begin(state, request)
        self.assertTrue(added, "default-penalty request should still be admitted under MTP")

    def test_speculative_config_disabled_by_default(self):
        self.assertIsNone(profile._speculative_config({}))
        self.assertIsNone(profile._speculative_config({"speculative": "none"}))

    def test_speculative_config_mtp_uses_draft_tokens(self):
        self.assertEqual(profile._speculative_config({"speculative": "mtp"}),
                         {"method": "mtp", "num_speculative_tokens": 2, "enforce_eager": True})
        self.assertEqual(
            profile._speculative_config({"speculative": "mtp", "speculative_draft_tokens": "3"}),
            {"method": "mtp", "num_speculative_tokens": 3, "enforce_eager": True})

    def test_mtp_uses_measured_memory_budget_but_target_only_is_unchanged(self):
        self.assertEqual(profile._gpu_memory_utilization("r4d_int4"), 0.90)
        self.assertEqual(profile._gpu_memory_utilization("r4d"), 0.925)
        self.assertEqual(profile._gpu_memory_utilization("r4d_int4", True), 0.991)

    def test_mtp_bf16_exclusions_reach_the_draft_without_mutating_the_target(self):
        exclude = ["model.layers.0.mlp.gate_proj.weight", "mtp.fc.weight",
                   "mtp.eh_proj.weight", "lm_head.weight"]
        target = NS(quantization_config={"exclude": list(exclude)})
        self.assertEqual(profile.mtp_bf16_modules(target), ("mtp.eh_proj", "mtp.fc"))

        draft = profile.apply_mtp_bf16_exclusions(target)
        self.assertIsNot(draft, target)
        self.assertEqual(target.quantization_config["exclude"], exclude)
        # The drafter is derived from the same artifact, so every BF16 mtp
        # exclusion must survive; an emptied exclude list is the shape assert.
        self.assertLessEqual(set(exclude), set(draft.quantization_config["exclude"]))
        self.assertEqual(profile.mtp_bf16_modules(draft), ("mtp.eh_proj", "mtp.fc"))

    def test_mtp_bf16_exclusions_reject_a_quantized_artifact(self):
        with self.assertRaises(RuntimeError):
            profile.mtp_bf16_modules(NS(quantization_config={"exclude": ["model.layers.0.mlp.gate_proj.weight"]}))
        with self.assertRaises(RuntimeError):
            profile.mtp_bf16_modules(NS())

    def test_target_only_decode_graph_stays_at_one(self):
        self.assertEqual(
            profile._compilation_config("int4_per_token_head", "r4d_int4", None, 0),
            {"mode": 0, "cudagraph_mode": "FULL_DECODE_ONLY", "cudagraph_capture_sizes": [1]})

    def test_mtp_capture_sizes_round_up_to_a_spec_decode_multiple(self):
        # vLLM rounds spec-decode capture sizes up to a multiple of
        # draft_tokens+1 and fails closed when none survive, so 1 must become 3
        # rather than a load-time error.
        self.assertEqual(
            profile._compilation_config("int4_per_token_head", "r4d_int4", None, 2),
            {"mode": 3, "cudagraph_mode": "FULL_DECODE_ONLY", "cudagraph_capture_sizes": [3, 12]})
        self.assertEqual(
            profile._compilation_config("int4_per_token_head", "r4d_int4", "3,6,12", 2),
            {"mode": 3, "cudagraph_mode": "FULL_DECODE_ONLY", "cudagraph_capture_sizes": [3, 6, 12]})
        # Draft 4 forces a multiple of 5, so the same request rounds to 5.
        self.assertEqual(
            profile._compilation_config("int4_per_token_head", "r4d_int4", None, 4)["cudagraph_capture_sizes"],
            [5, 20])

    def test_mtp3_retains_single_and_four_request_graphs(self):
        self.assertEqual(
            profile._compilation_config("int4_per_token_head", "r4d_int4", None, 3),
            {"mode": 3, "cudagraph_mode": "FULL_DECODE_ONLY", "cudagraph_capture_sizes": [4, 16]})
        self.assertEqual(
            profile._compilation_config("auto", "r4d", None, 3),
            {"mode": 0, "cudagraph_mode": "FULL_DECODE_ONLY", "cudagraph_capture_sizes": [4]})

    def test_draft_acceptance_reports_per_position_acceptance(self):
        stats = NS(num_drafts=4, num_draft_tokens=8, num_accepted_tokens=5,
                   num_accepted_tokens_per_pos=[4, 1], num_draft_tokens_per_pos=[4, 4])
        with patch.object(profile, "_DRAFT_ACCEPTANCE", None):
            self.assertEqual(profile.draft_acceptance_snapshot(), {"drafts": 0})
        class StatLoggerBaseStub:
            def __init__(self, vllm_config, engine_index=0):
                self.vllm_config = vllm_config
                self.engine_index = engine_index
        logger_factory = profile._draft_acceptance_logger_factory(StatLoggerBaseStub)
        logger = logger_factory(NS(), 0)
        logger.record(NS(spec_decoding_stats=stats), None)
        logger.record(NS(spec_decoding_stats=None), None)
        snapshot = logger.snapshot()
        self.assertEqual(snapshot["drafts"], 4)
        self.assertEqual(snapshot["accepted"], 5)
        # A falling per-position rate is the signal that depth is being wasted.
        self.assertEqual(snapshot["acceptance_per_position"], [1.0, 0.25])
        self.assertEqual(snapshot["drafted_per_position"], [4, 4])

    def test_radiance_env_defaults_when_absent(self):
        self.assertEqual(profile._radiance_env({}), profile._RADIANCE_ENV_DEFAULTS)

    def test_mtp_int4_decode_default_preserves_four_request_native_path(self):
        config = {"prefill_attention": "r4d_int4", "speculative": "mtp"}
        self.assertEqual(profile._radiance_env(config)["RADIANCE_MXFP4_DECODE_MAX_M"], "16")
        config["radiance_env"] = json.dumps({"RADIANCE_MXFP4_DECODE_MAX_M": "8"})
        self.assertEqual(profile._radiance_env(config)["RADIANCE_MXFP4_DECODE_MAX_M"], "8")

    def test_compiler_cache_root_redirects_compiler_and_temporary_output(self):
        with tempfile.TemporaryDirectory() as directory, patch.dict(os.environ), patch.object(tempfile, "tempdir", None):
            root = Path(directory) / "compiler-cache"
            profile._configure_compiler_cache({"compiler_cache_root": str(root)})
            for key,name in {"VLLM_CACHE_ROOT":"vllm", "TORCHINDUCTOR_CACHE_DIR":"inductor",
                             "TRITON_CACHE_DIR":"triton", "TEMP":"tmp", "TMP":"tmp"}.items():
                self.assertEqual(Path(os.environ[key]), root / name)
                self.assertTrue((root / name).is_dir())
            self.assertEqual(Path(tempfile.gettempdir()), root / "tmp")

    def test_retained_memory_budget_preserves_four_by_100k_cache(self):
        total = 34208743424
        speculative = {"method": "mtp", "num_speculative_tokens": 3}
        self.assertEqual(profile._engine_memory_config("r4d_int4", speculative, 4, total, total),
                         {"gpu_memory_utilization": 0.991})
        free = total - 868 * 1024 * 1024
        options = profile._engine_memory_config("r4d_int4", speculative, 4, free, total)
        self.assertEqual(options["num_gpu_blocks_override"], 192)
        self.assertLess(math.ceil(total * options["gpu_memory_utilization"]), free)
        self.assertGreaterEqual(options["gpu_memory_utilization"], 0.97)
        with self.assertRaisesRegex(RuntimeError, "four-by-100K.*free=.*required=.*shortfall="):
            profile._engine_memory_config("r4d_int4", speculative, 4, int(total * 0.96), total)
        self.assertEqual(profile._engine_memory_config("r4d", None, 1, free, total),
                         {"gpu_memory_utilization": 0.925})

    def test_retained_memory_budget_admits_observed_qwen4b_swap_without_shrinking_cache(self):
        total = 34208743424
        free = 31634 * 1024 * 1024
        options = profile._engine_memory_config("r4d_int4", {"method": "mtp", "num_speculative_tokens": 3}, 4, free, total)
        self.assertEqual(options["num_gpu_blocks_override"], 192)
        self.assertAlmostEqual(options["gpu_memory_utilization"], (free - 64 * 1024 * 1024) / total)
        self.assertLess(options["gpu_memory_utilization"], 0.97)
        self.assertGreaterEqual(options["gpu_memory_utilization"], 0.96)

    def test_radiance_env_overrides_only_named_knobs(self):
        merged = profile._radiance_env({"radiance_env": json.dumps({
            "RADIANCE_MXFP4_DECODE_MAX_M": "64",
            "RADIANCE_MXFP4_A_TILED_MIN_M": 1,
        })})
        self.assertEqual(merged["RADIANCE_MXFP4_DECODE_MAX_M"], "64")
        self.assertEqual(merged["RADIANCE_MXFP4_A_TILED_MIN_M"], "1")
        self.assertEqual(merged["RADIANCE_MXFP4_R4D_DECODE_MAX_M"], "0")
        self.assertEqual(merged["RADIANCE_FUSE_RMS_QUANT"], "1")

    def test_radiance_env_rejects_non_radiance_keys(self):
        with self.assertRaisesRegex(RuntimeError, "is not a RADIANCE_ variable"):
            profile._radiance_env({"radiance_env": json.dumps({"PATH": "C:/evil"})})

    def test_radiance_env_rejects_malformed_payloads(self):
        for payload in ("not json", json.dumps([1, 2]), json.dumps({"RADIANCE_X": True}),
                        json.dumps({"RADIANCE_X": [1]}), json.dumps({1: "x"})):
            with self.subTest(payload=payload):
                with self.assertRaises(RuntimeError):
                    profile._radiance_env({"radiance_env": payload})

    def test_radiance_env_rejects_non_string_payload(self):
        with self.assertRaisesRegex(RuntimeError, "must be a JSON object string"):
            profile._radiance_env({"radiance_env": {"RADIANCE_MXFP4": "1"}})

    def test_validate_config_rejects_bad_radiance_env(self):
        with self.assertRaisesRegex(RuntimeError, "is not a RADIANCE_ variable"):
            profile.validate_config({"runtime": "vllm_radiance", "context_size": 106496,
                "n_slots": 1, "min_slots": 1, "radiance_env": json.dumps({"LD_PRELOAD": "/x"})})


if __name__ == "__main__":
    unittest.main()
