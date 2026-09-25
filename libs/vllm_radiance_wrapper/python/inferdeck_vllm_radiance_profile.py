"""Pinned synchronous V1 InprocClient bridge. No inference worker or HTTP server; Triton may invoke compiler tools."""
from __future__ import annotations
import importlib.util
import importlib
import copy
import faulthandler
from contextlib import contextmanager
import json
import gc
import hashlib
import math
import re
from inferdeck_vllm_radiance_lifecycle import release_language_model_cache, capture_lifecycle_refs, lifecycle_diagnostics, finalize_engine_caches
import os
import sys
import threading
import time
import uuid
from pathlib import Path
from typing import Any

_DLL_HANDLES = []
_RELEASED_SNAPSHOT = None
_EXTENSION_SHA256 = "64124749ed12f72c3d13544b313dcdcff33582e3bd7e001b519a2c5bb1f2ed4d"
_CAPTURE_MARKER_PATH = Path(__file__).with_name("capture-next-request.json")
_CAPTURE_OUTPUT_PATH = Path(__file__).with_name("captured-request.json")

_REQUIRED=("model","python_root","python_site","vllm_source","radiance_source","radiance_extension","rocm","selector_overlay","pread_overlay","prefill_overlay","prefill_dll","prefill_dll_sha256")
_PREFILL_ATTENTION_DEFAULT = "r4d"
_PREFILL_ATTENTION_OPTIONS = frozenset(("r4d", "r4d_int4", "upstream"))
_KV_CACHE_DTYPE_OPTIONS = frozenset(("auto", "int4_per_token_head"))

def _gpu_memory_utilization(prefill_attention:str)->float:
    return 0.925 if prefill_attention == "r4d" else 0.90

def _is_r4d_prefill(prefill_attention:str)->bool:
    return prefill_attention in ("r4d", "r4d_int4")

def _compilation_config(kv_cache_dtype:str, prefill_attention:str)->dict[str,Any]:
    if kv_cache_dtype == "int4_per_token_head" and prefill_attention == "upstream":
        return {"mode":0,"cudagraph_mode":"NONE"}
    return {"mode":0,"cudagraph_mode":"FULL_DECODE_ONLY","cudagraph_capture_sizes":[1]}

def _kv_cache_token_capacity(kv_cache_config:Any)->int:
    num_blocks = int(kv_cache_config.num_blocks)
    block_sizes = [int(group.kv_cache_spec.block_size) for group in kv_cache_config.kv_cache_groups]
    if num_blocks <= 0 or not block_sizes or any(block_size <= 0 for block_size in block_sizes):
        raise RuntimeError("R4D KV cache has invalid block capacity metadata")
    return num_blocks * min(block_sizes)

def _kv_cache_max_concurrency(kv_cache_config:Any, vllm_config:Any)->float:
    num_blocks = int(kv_cache_config.num_blocks)
    groups = kv_cache_config.kv_cache_groups
    if num_blocks <= 0 or not groups:
        raise RuntimeError("R4D KV cache has invalid block capacity metadata")
    blocks_per_request = 0
    for group in groups:
        spec = group.kv_cache_spec
        page_size_bytes = int(spec.page_size_bytes)
        memory_usage_bytes = int(spec.max_memory_usage_bytes(vllm_config))
        if page_size_bytes <= 0 or memory_usage_bytes <= 0:
            raise RuntimeError("R4D KV cache has invalid per-request memory metadata")
        blocks_per_request += math.ceil(memory_usage_bytes / page_size_bytes)
    if blocks_per_request <= 0:
        raise RuntimeError("R4D KV cache has invalid per-request block demand")
    return num_blocks / blocks_per_request

@contextmanager
def _r4d_cache_retention(prefill_attention:str):
    if not _is_r4d_prefill(prefill_attention):
        yield
        return
    key = "VLLM_PREFIX_CACHE_RETENTION_INTERVAL"
    previous = os.environ.get(key)
    os.environ[key] = "0"
    try:
        yield
    finally:
        if previous is None:
            os.environ.pop(key, None)
        else:
            os.environ[key] = previous

def _need(c:dict[str,Any],k:str)->str:
    v=c.get(k)
    if not isinstance(v,str) or not v or not Path(v).exists(): raise RuntimeError(f"required vllm_radiance artifact is unavailable: {k}")
    return v
def _load(n:str,p:str)->Any:
    spec=importlib.util.spec_from_file_location(n,p)
    if spec is None or spec.loader is None: raise RuntimeError(f"cannot load {p}")
    module=importlib.util.module_from_spec(spec);sys.modules[n]=module;spec.loader.exec_module(module);return module
def validate_config(c:dict[str,Any])->None:
    if c.get("runtime")!="vllm_radiance":raise RuntimeError("runtime must be vllm_radiance")
    context_size = int(c.get("context_size", 0))
    n_slots = int(c.get("n_slots", 0))
    min_slots = int(c.get("min_slots", 0))
    if context_size != 106496:
        raise RuntimeError("requires 106496 context")
    prefill_attention = c.get("prefill_attention", _PREFILL_ATTENTION_DEFAULT)
    if not isinstance(prefill_attention, str) or prefill_attention not in _PREFILL_ATTENTION_OPTIONS:
        raise RuntimeError("prefill_attention must be one of: r4d, r4d_int4, upstream")
    if "gpu_memory_utilization" in c:
        raise RuntimeError("gpu_memory_utilization is not a supported profile setting")
    maximum_slots = 4 if prefill_attention == "r4d_int4" else 1
    if not 1 <= min_slots == n_slots <= maximum_slots:
        raise RuntimeError(f"{prefill_attention} requires min_slots == n_slots between 1 and {maximum_slots}")
    kv_cache_dtype = c.get("kv_cache_dtype", "auto")
    if not isinstance(kv_cache_dtype, str) or kv_cache_dtype not in _KV_CACHE_DTYPE_OPTIONS:
        raise RuntimeError("kv_cache_dtype must be one of: auto, int4_per_token_head")
    if kv_cache_dtype == "int4_per_token_head" and prefill_attention == "r4d":
        raise RuntimeError("R4D prefill requires BF16 KV; select r4d_int4 for the native four-bit kernel")
    if prefill_attention == "r4d_int4" and kv_cache_dtype != "int4_per_token_head":
        raise RuntimeError("r4d_int4 prefill requires kv_cache_dtype int4_per_token_head")
    optional_r4d = {"prefill_overlay", "prefill_dll", "prefill_dll_sha256"}
    for key in _REQUIRED:
        if prefill_attention == "upstream" and key in optional_r4d:
            continue
        if key == "prefill_dll_sha256":
            if not re.fullmatch(r"[0-9a-fA-F]{64}", str(c.get(key, ""))):
                raise RuntimeError("prefill_dll_sha256 must be a SHA-256 digest")
        else:
            _need(c, key)
    decode_keys = ("decode_dll", "decode_dll_sha256")
    decode_present = [key in c for key in decode_keys]
    if any(decode_present):
        if not all(decode_present):
            raise RuntimeError("decode_dll and decode_dll_sha256 must be configured together")
        if prefill_attention != "r4d_int4":
            raise RuntimeError("decode DLL artifacts require prefill_attention r4d_int4")
        _need(c, "decode_dll")
        if not isinstance(c.get("decode_dll_sha256"), str) or not re.fullmatch(r"[0-9a-fA-F]{64}", c["decode_dll_sha256"]):
            raise RuntimeError("decode_dll_sha256 must be a SHA-256 digest")
    elif prefill_attention == "r4d_int4":
        raise RuntimeError("r4d_int4 requires decode_dll and decode_dll_sha256")
def _configure_native_compiler(python_site: str)->str:
    cc=Path(python_site)/"_rocm_sdk_core"/"lib"/"llvm"/"bin"/"clang-cl.exe"
    if not cc.is_file(): raise RuntimeError(f"pinned native compiler is unavailable: {cc}")
    os.environ["CC"]=str(cc)
    return str(cc)
def _register_qwen35_model_class()->type:
    from vllm.model_executor.models import ModelRegistry
    from vllm.model_executor.models.qwen3_5 import Qwen3_5ForConditionalGeneration
    ModelRegistry.register_model("Qwen3_5ForConditionalGeneration", Qwen3_5ForConditionalGeneration)
    return Qwen3_5ForConditionalGeneration
def _verify_upstream_prefill_attention()->Any:
    """Return the active vLLM Triton attention binding after identity validation."""
    module_name = "vllm.v1.attention.ops.triton_unified_attention"
    backend_name = "vllm.v1.attention.backends.triton_attn"
    try:
        definition_module = importlib.import_module(module_name)
        backend_module = importlib.import_module(backend_name)
    except Exception as error:
        raise RuntimeError(f"upstream prefill attention module is unavailable: {error}") from error
    definition = getattr(definition_module, "unified_attention", None)
    active = getattr(backend_module, "unified_attention", None)
    if not callable(definition) or getattr(definition, "__module__", None) != module_name:
        raise RuntimeError("upstream prefill attention definition is not the expected callable")
    if active is not definition or getattr(active, "__module__", None) != module_name:
        raise RuntimeError("vLLM Triton attention backend has an unexpected unified_attention binding")
    return active

def _install_prefill_overlay(c:dict[str,Any], prefill_attention:str)->Any:
    if prefill_attention == "upstream":
        return None
    if prefill_attention == "r4d_int4":
        _verify_upstream_prefill_attention()
    overlay = _load("inferdeck_prefill", _need(c, "prefill_overlay"))
    dll_path = Path(_need(c, "prefill_dll"))
    dll_sha256 = c["prefill_dll_sha256"]
    if prefill_attention == "r4d_int4":
        decode_dll = Path(_need(c, "decode_dll"))
        return overlay.install_int4_prefill_overlay(
            dll_path, compare_reference=False, expected_dll_sha256=dll_sha256,
            decode_dll_path=decode_dll,
            expected_decode_dll_sha256=c.get("decode_dll_sha256"),
            decode_fallback_to_upstream=False)
    return overlay.install_r4d_prefill_overlay(
        dll_path, compare_reference=False, expected_dll_sha256=dll_sha256)

def _capture_next_request(r:dict[str,Any], ids:list[Any])->None:
    """Consume a short-lived operator marker and save one rendered request privately."""
    try:
        try:
            with _CAPTURE_MARKER_PATH.open("r", encoding="utf-8") as marker_file:
                marker = json.load(marker_file)
        except FileNotFoundError:
            return
        expires_at = float(marker["expires_at"])
        now = time.time()
        _CAPTURE_MARKER_PATH.unlink()
        if not math.isfinite(expires_at) or expires_at < now or expires_at > now + 600:
            return
        payload = {
            "captured_at_epoch": now,
            "captured_at_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(now)),
            "request": json.loads(json.dumps(r, default=str)),
            "rendered_ids": [int(token_id) for token_id in ids],
        }
        with _CAPTURE_OUTPUT_PATH.open("x", encoding="utf-8") as output_file:
            json.dump(payload, output_file, ensure_ascii=False, indent=2)
            output_file.write("\n")
    except Exception:
        print("vllm_radiance request capture failed", file=sys.stderr, flush=True)


def create(c:dict[str,Any])->dict[str,Any]:
    return _create(c)


def _create(c:dict[str,Any])->dict[str,Any]:
    validate_config(c)
    faulthandler.enable(file=sys.stderr, all_threads=True)
    os.environ.setdefault("GPU_RESOURCE_CACHE_SIZE", "64")
    prefill_attention = c.get("prefill_attention", _PREFILL_ATTENTION_DEFAULT)
    for path in reversed([_need(c,key) for key in ("python_site","vllm_source","radiance_source","radiance_extension")]):
        if path not in sys.path:sys.path.insert(0,path)
    os.environ.update({"PYTHONNOUSERSITE":"1","PYTHONDONTWRITEBYTECODE":"1","TOKENIZERS_PARALLELISM":"false","VLLM_TARGET_DEVICE":"rocm","VLLM_ENABLE_V1_MULTIPROCESSING":"0","VLLM_NO_USAGE_STATS":"1","VLLM_USE_RUST_FRONTEND":"0","ROCM_PATH":_need(c,"rocm"),"HIP_PATH":_need(c,"rocm"),"RADIANCE_MXFP4_W4A8":"1","RADIANCE_MXFP4":"1","RADIANCE_MXFP4_WPERM":"1","RADIANCE_MXFP4_A_TILED_MIN_M":"513","RADIANCE_MXFP4_W4A8_MIN_M":"0","RADIANCE_MXFP4_DECODE_MAX_M":"8","RADIANCE_MXFP4_R4D_DECODE_MAX_M":"0","RADIANCE_FUSE_RMS_QUANT":"1"})
    dll_paths = [Path(c["python_root"]), Path(c["python_root"]) / "DLLs", Path(c["python_site"]),
                 Path(c["rocm"]), Path(c["radiance_extension"])]
    if not _DLL_HANDLES:
        for directory in dll_paths:
            if directory.is_dir():
                _DLL_HANDLES.append(os.add_dll_directory(str(directory.resolve())))
    extension_path = Path(c["radiance_extension"]) / "radiance_mxfp4_fp8.cp312-win_amd64.pyd"
    if hashlib.sha256(extension_path.read_bytes()).hexdigest() != _EXTENSION_SHA256:
        raise RuntimeError("Radiance extension differs from the validated kmag-rne-wide profile")
    _configure_native_compiler(c["python_site"])
    from inferdeck_vllm_radiance_tokenizer import MODE, register_tokenizer
    from vllm.tokenizers.registry import cached_tokenizer_from_config
    from vllm import SamplingParams
    from vllm.engine.arg_utils import EngineArgs
    from vllm.utils.import_utils import has_quark
    from vllm.v1.engine.core_client import InprocClient
    from vllm.v1.engine.llm_engine import LLMEngine
    from inferdeck_vllm_radiance_penalties import InferDeckPenaltiesProcessor
    _register_qwen35_model_class()
    if not has_quark():raise RuntimeError("amd-quark is required")
    import torch
    if not torch.cuda.is_available() or torch.version.hip is None:raise RuntimeError("vllm_radiance requires an available ROCm/HIP device")
    import radiance_mxfp4_fp8
    if Path(radiance_mxfp4_fp8.__file__).resolve() != extension_path.resolve():
        raise RuntimeError("A different Radiance extension is already loaded")
    contexts = []
    engine = None
    try:
        pread=_load("inferdeck_pread",_need(c,"pread_overlay")).install_pread_overlay();pread.__enter__();contexts.insert(0,pread)
        selector=_load("inferdeck_selector",_need(c,"selector_overlay")).install_radiance_overlay(observe_apply=False,max_observations=128);selector.__enter__();contexts.insert(0,selector)
        if prefill_attention == "upstream":
            _verify_upstream_prefill_attention()
        else:
            prefill = _install_prefill_overlay(c, prefill_attention)
            prefill.__enter__()
            contexts.insert(0, prefill)
        model=_need(c,"model");tokenizer_revision=register_tokenizer(model)
        scheduler_options = {"async_scheduling": False} if _is_r4d_prefill(prefill_attention) else {}
        if prefill_attention == "r4d_int4":
            scheduler_options["long_prefill_token_threshold"] = 2048
        kv_cache_dtype = c.get("kv_cache_dtype", "auto")
        compilation_config = _compilation_config(kv_cache_dtype, prefill_attention)
        memory_utilization = _gpu_memory_utilization(prefill_attention)
        n_slots = int(c["n_slots"])
        engine_args=EngineArgs(model=model,logits_processors=[InferDeckPenaltiesProcessor],tokenizer=model,tokenizer_mode=MODE,tokenizer_revision=tokenizer_revision,trust_remote_code=False,load_format="safetensors",quantization="quark",kv_cache_dtype=kv_cache_dtype,max_model_len=106496,max_num_batched_tokens=4096,max_num_seqs=n_slots,tensor_parallel_size=1,pipeline_parallel_size=1,enforce_eager=False,gpu_memory_utilization=memory_utilization,seed=1234,enable_prefix_caching=True,attention_backend="TRITON_ATTN",reasoning_parser="qwen3",compilation_config=compilation_config,**scheduler_options)
        with _r4d_cache_retention(prefill_attention):
            engine=LLMEngine.from_engine_args(engine_args,enable_multiprocessing=False)
        if engine.vllm_config.cache_config.cache_dtype != kv_cache_dtype:
            raise RuntimeError("effective KV cache dtype differs from requested profile")
        if _is_r4d_prefill(prefill_attention):
            cache_manager=engine.engine_core.engine_core.scheduler.kv_cache_manager
            free_bytes,total_bytes=torch.cuda.mem_get_info()
            kv_token_capacity = _kv_cache_token_capacity(cache_manager.kv_cache_config)
            kv_max_concurrency = _kv_cache_max_concurrency(cache_manager.kv_cache_config, engine.vllm_config)
            actual={"async":engine.vllm_config.scheduler_config.async_scheduling,"long_prefill_token_threshold":engine.vllm_config.scheduler_config.long_prefill_token_threshold,"retention":cache_manager.coordinator.retention_interval,"blocks":cache_manager.kv_cache_config.num_blocks,"kv_token_capacity":kv_token_capacity,"kv_max_concurrency":kv_max_concurrency,"context":engine.vllm_config.model_config.max_model_len,"memory_utilization":engine.vllm_config.cache_config.gpu_memory_utilization,"free_bytes":free_bytes,"total_bytes":total_bytes}
            print("event=isolated_cache_profile "+json.dumps(actual),file=sys.stderr,flush=True)
            expected_memory_utilization = memory_utilization
            minimum_blocks = 185 if prefill_attention == "r4d" else 0
            capacity_valid = kv_max_concurrency >= n_slots if prefill_attention == "r4d_int4" else actual["blocks"] >= minimum_blocks
            if actual["async"] or actual["long_prefill_token_threshold"] != scheduler_options.get("long_prefill_token_threshold", 0) or actual["retention"] != 0 or actual["context"] != 106496 or actual["memory_utilization"] != expected_memory_utilization or not capacity_valid or free_bytes < 1073741824:
                raise RuntimeError("R4D cache profile or free-memory guard failed")
        tokenizer=cached_tokenizer_from_config(engine.vllm_config.model_config)
        if engine.vllm_config.model_config.enable_prompt_embeds:raise RuntimeError("native pooled tokenizer does not support prompt embeds")
        if not isinstance(engine.engine_core,InprocClient):raise RuntimeError("V1 InprocClient was not constructed")
        if engine.vllm_config.model_config.enforce_eager or engine.vllm_config.scheduler_config.max_num_batched_tokens!=4096 or engine.vllm_config.scheduler_config.max_num_seqs!=n_slots:raise RuntimeError("effective engine differs from configured profile")
        if prefill_attention == "upstream":
            _verify_upstream_prefill_attention()
        decode_kernel_enabled = prefill_attention == "r4d_int4" and "decode_dll" in c
        print(f"vllm_radiance prefill_attention={prefill_attention} kv_cache_dtype={kv_cache_dtype} gpu_memory_utilization={memory_utilization} int4_decode_kernel={str(decode_kernel_enabled).lower()}", file=sys.stderr, flush=True)
        return {"engine":engine,"engine_lock":threading.RLock(),"tokenizer":tokenizer,"SamplingParams":SamplingParams,"active":set(),"requests":{},"contexts":tuple(contexts),"prefill_attention":prefill_attention,"int4_decode_kernel":decode_kernel_enabled}
    except Exception as load_error:
        try:
            shutdown({"engine": engine, "active": set(), "requests": {},
                      "contexts": tuple(contexts)})
        except Exception as cleanup_error:
            raise RuntimeError(f"load failed: {load_error}; cleanup failed: {cleanup_error}") from load_error
        raise
    finally:
        engine = None


def begin(s:dict[str,Any],r:dict[str,Any])->str:
    with s["engine_lock"]:
        return _begin_locked(s,r)

def _template_messages(messages:list[dict[str,Any]])->list[dict[str,Any]]:
    system = [message["content"] for message in messages
              if message.get("role") == "system" and message.get("content")]
    developer = [message["content"] for message in messages
                 if message.get("role") == "developer" and message.get("content")]
    result = [copy.deepcopy(message) for message in messages
              if message.get("role") not in ("system", "developer")]
    if system or developer:
        result.insert(0, {"role": "system", "content": "\n\n".join(system + developer)})
    return result

def _has_image_input(messages:list[dict[str,Any]])->bool:
    return any(
        isinstance(part, dict) and part.get("type") == "image_url"
        for message in messages
        for part in (message.get("content")
                     if isinstance(message.get("content"), list) else [])
    )

def _render_image_prompt(s:dict[str,Any], request:Any,
                         messages:list[dict[str,Any]], kwargs:dict[str,Any])->tuple[list[int],Any]:
    from vllm.renderers import ChatParams
    params=ChatParams(
        chat_template_content_format="auto",
        chat_template_kwargs=kwargs,
        tool_choice=request.tool_choice,
        response_format=request.response_format,
    )
    _, engine_inputs=s["engine"].renderer.render_chat([messages],params)
    if len(engine_inputs) != 1:
        raise RuntimeError("vLLM renderer returned an invalid image prompt count")
    engine_input=engine_inputs[0]
    ids=engine_input.get("prompt_token_ids")
    if not isinstance(ids, list) or not engine_input.get("multi_modal_data"):
        raise RuntimeError("vLLM renderer did not produce multimodal image input")
    return ids,engine_input

def _begin_locked(s:dict[str,Any],r:dict[str,Any])->str:
    if r.get("logprobs"):raise RuntimeError("logprobs are unsupported by vllm_radiance")
    from vllm.entrypoints.openai.chat_completion.protocol import ChatCompletionRequest
    from vllm.parser.qwen3 import Qwen3Parser
    from vllm.tool_parsers.qwen3_engine_tool_parser import Qwen3EngineToolParser
    from vllm.sampling_params import RequestOutputKind, StructuredOutputsParams
    from vllm.tool_parsers.structural_tag_registry import get_model_structural_tag
    reasoning_effort = r.get("reasoning_effort")
    request_kwargs=dict(model=r["model"],messages=r["messages"],tools=r.get("tools") or None,tool_choice=(r.get("tool_choice") or "auto") if r.get("tools") else "none",include_reasoning=bool(r.get("include_reasoning",True)) and reasoning_effort != "none",stream=True)
    request_kwargs.update({key:value for key,value in r["sampling"].items() if value is not None})
    if request_kwargs.get("logit_bias"):
        request_kwargs["logit_bias"] = {str(token): bias for token, bias in request_kwargs["logit_bias"].items()}
    if r.get("stop"):request_kwargs["stop"]=r["stop"]
    if r.get("structured_outputs") is not None:request_kwargs["structured_outputs"]=r["structured_outputs"]
    request=Qwen3EngineToolParser(s["tokenizer"], request_kwargs["tools"]).adjust_request(ChatCompletionRequest(**request_kwargs))
    if request.tool_choice == "required" or isinstance(request.tool_choice, dict) or getattr(request.tool_choice,"type",None) == "function":
        structural_tag=get_model_structural_tag("qwen_3_coder",request.tools,request.tool_choice,reasoning=False)
        if structural_tag is None:raise RuntimeError("Qwen3 required or named tool choice has no structural tag")
        request.structured_outputs=StructuredOutputsParams(structural_tag=json.dumps(structural_tag.model_dump()))
        request.response_format=None
    kwargs={"tokenize":True,"return_dict":False,"add_generation_prompt":bool(r.get("add_generation_prompt",True))}
    if r.get("tools"):kwargs["tools"]=r["tools"]
    if reasoning_effort == "none":
        kwargs["enable_thinking"] = False
    else:
        if r.get("enable_reasoning") is not None:kwargs["enable_thinking"]=r["enable_reasoning"]
        if reasoning_effort:kwargs["reasoning_effort"]=reasoning_effort
    template_messages = _template_messages(r["messages"])
    for message in template_messages:
        for call in message.get("tool_calls", ()):
            function = call.get("function", {})
            if isinstance(function.get("arguments"), str):
                function["arguments"] = json.loads(function["arguments"])
    engine_input=None
    if _has_image_input(template_messages):
        ids,engine_input=_render_image_prompt(s,request,template_messages,kwargs)
    else:
        ids=s["tokenizer"].apply_chat_template(template_messages,**kwargs)
        if not isinstance(ids, list):
            raise RuntimeError("tokenizer did not return token IDs")
    maximum = int(r["max_output_tokens"])
    if maximum == -1:
        maximum = 106496 - len(ids)
    if maximum <= 0 or len(ids) + maximum > 106496:
        raise RuntimeError("request exceeds configured context")
    _capture_next_request(r, ids)
    sampling=request.to_sampling_params(maximum,{})
    penalties = {
        "repeat_last_n": int(r.get("repeat_last_n", 64)),
        "repetition_penalty": float(r["sampling"].get("repetition_penalty", 1.0)),
        "frequency_penalty": float(r["sampling"].get("frequency_penalty", 0.0)),
        "presence_penalty": float(r["sampling"].get("presence_penalty", 0.0)),
    }
    sampling.extra_args = {**(getattr(sampling, "extra_args", None) or {}), "inferdeck_penalties": penalties}
    sampling.repetition_penalty = 1.0
    sampling.frequency_penalty = 0.0
    sampling.presence_penalty = 0.0
    sampling.output_kind=RequestOutputKind.DELTA
    print("event=sampling_resolved runtime=vllm_radiance " + json.dumps({
        "model": r["model"], "inferdeck_penalties": penalties,
        "temperature": getattr(sampling, "temperature", None), "top_p": getattr(sampling, "top_p", None),
        "top_k": getattr(sampling, "top_k", None), "min_p": getattr(sampling, "min_p", None),
        "repetition_penalty": getattr(sampling, "repetition_penalty", None),
        "frequency_penalty": getattr(sampling, "frequency_penalty", None),
        "presence_penalty": getattr(sampling, "presence_penalty", None),
        "seed": getattr(sampling, "seed", None), "max_tokens": getattr(sampling, "max_tokens", None),
    }), file=sys.stderr, flush=True)
    if r.get("stop"):sampling.stop=r["stop"]
    parser_kwargs = {"enable_thinking": kwargs["enable_thinking"]} if "enable_thinking" in kwargs else {}
    rid=uuid.uuid4().hex
    state={"request":request,"parser":Qwen3Parser(s["tokenizer"],request.tools,chat_template_kwargs=parser_kwargs),"ids":ids,"completion_tokens":0,"had_tools":False,"pending":[]}
    s["engine"].add_request(rid,engine_input if engine_input is not None else ids,
                            sampling,priority=0)
    s["active"].add(rid)
    s["requests"][rid]=state
    return rid
def step(s:dict[str,Any],rid:str,r:dict[str,Any])->list[dict[str,Any]]:
    with s["engine_lock"]:
        return _step_locked(s,rid,r)
def _step_locked(s:dict[str,Any],rid:str,r:dict[str,Any])->list[dict[str,Any]]:
    state=s["requests"].get(rid)
    if state is None:raise RuntimeError("unknown vllm_radiance request")
    for output in s["engine"].step():
        output_state = s["requests"].get(output.request_id)
        if output_state is not None:
            output_state.setdefault("pending", []).append(output)
    result=[]
    while state["pending"]:
        output = state["pending"].pop(0)
        completion=output.outputs[0] if output.outputs else None;done=bool(output.finished)
        delta=state["parser"].parse_delta(completion.text if completion else "",list(completion.token_ids or []) if completion else [],state["request"],state["ids"],finished=done)
        state["completion_tokens"] += len(completion.token_ids or []) if completion else 0
        calls=[]
        for call in getattr(delta,"tool_calls",[]) or []:
            fn=getattr(call,"function",None);calls.append({"index":int(getattr(call,"index",0) or 0),"id":str(getattr(call,"id","") or ""),"type":str(getattr(call,"type","") or "function"),"name":str(getattr(fn,"name","") or ""),"arguments":str(getattr(fn,"arguments","") or "")})
        state["had_tools"] = state["had_tools"] or bool(calls)
        result.append({"text":str(getattr(delta,"content","") or ""),"reasoning":str(getattr(delta,"reasoning","") or ""),"tool_calls":calls,"finished":done,"prompt_tokens":len(output.prompt_token_ids or []),"cached_tokens":int(output.num_cached_tokens or 0),"completion_tokens":state["completion_tokens"],"finish_reason":("tool_calls" if done and state["had_tools"] and completion and completion.finish_reason == "stop" else (completion.finish_reason if completion else None) or "stop")})
        metrics = getattr(output, "metrics", None)
        scheduled = float(getattr(metrics, "scheduled_ts", 0.0))
        first = float(getattr(metrics, "first_token_ts", 0.0))
        last = float(getattr(metrics, "last_token_ts", 0.0))
        if all(math.isfinite(value) for value in (scheduled, first, last)) and 0 < scheduled <= first <= last:
            result[-1]["prompt_duration_ms"] = (first - scheduled) * 1000.0
            result[-1]["generation_duration_ms"] = (last - first) * 1000.0
            queued = float(getattr(metrics, "queued_ts", 0.0))
            if done and s.get("prefill_attention") == "r4d_int4" and math.isfinite(queued) and 0 < queued <= scheduled:
                print("event=vllm_radiance_request_timing " + json.dumps({
                    "engine_queue_ms": round((scheduled - queued) * 1000.0, 1),
                    "engine_prefill_ms": round((first - scheduled) * 1000.0, 1),
                    "engine_generation_ms": round((last - first) * 1000.0, 1),
                    "prompt_tokens": len(output.prompt_token_ids or []),
                    "cached_tokens": int(output.num_cached_tokens or 0),
                    "completion_tokens": state["completion_tokens"],
                }), file=sys.stderr, flush=True)
        if done:s["active"].discard(rid);s["requests"].pop(rid,None)
    return result
def abort(s:dict[str,Any],rid:str)->None:
    with s["engine_lock"]:
        if rid in s["active"]:s["engine"].abort_request([rid]);s["active"].discard(rid);s["requests"].pop(rid,None)
def active_count(s:dict[str,Any])->int:
    with s["engine_lock"]:return len(s["active"])
def shutdown(s: dict[str, Any]) -> None:
    global _RELEASED_SNAPSHOT
    errors = []
    for rid in tuple(s["active"]):
        try:
            abort(s, rid)
        except Exception as error:
            errors.append(str(error))
    engine = s.get("engine")
    if engine is not None:
        _RELEASED_SNAPSHOT = capture_lifecycle_refs(engine)
        try:
            removed = release_language_model_cache(engine)
            print(f"vllm_radiance cleanup: released_model_cache_entries={removed}", file=sys.stderr, flush=True)
        except Exception as error:
            errors.append(str(error))
        try:
            engine.engine_core.shutdown()
        except Exception as error:
            errors.append(str(error))
        try:
            finalized = finalize_engine_caches(engine)
            print(f"vllm_radiance cleanup: finalized_engine_caches={finalized}", file=sys.stderr, flush=True)
        except Exception as error:
            errors.append(str(error))
    s["engine"] = None
    del engine
    s["active"].clear()
    s["requests"].clear()
    for context in s.pop("contexts", ()):
        try:
            context.__exit__(None, None, None)
        except Exception as error:
            errors.append(str(error))
    gc.collect()
    torch = sys.modules.get("torch")
    if torch is not None:
        torch.cuda.synchronize()
        torch.cuda.empty_cache()
    if errors:
        raise RuntimeError("vllm_radiance cleanup failed: " + "; ".join(errors))


def collect_released_resources() -> None:
    global _RELEASED_SNAPSHOT
    gc.collect()
    torch = sys.modules.get("torch")
    if torch is not None:
        torch.cuda.synchronize()
        torch.cuda.empty_cache()
        host_before = torch.cuda.memory.host_memory_stats()
        torch.accelerator.memory.empty_host_cache()
        host_after = torch.cuda.memory.host_memory_stats()
        print("vllm_radiance host cleanup: " + json.dumps({
            "allocated_before": host_before.get("allocated_bytes.current", 0),
            "allocated_after": host_after.get("allocated_bytes.current", 0),
            "active_after": host_after.get("active_bytes.current", 0)}), file=sys.stderr, flush=True)
        print(f"vllm_radiance cleanup: allocated_bytes={torch.cuda.memory_allocated()} reserved_bytes={torch.cuda.memory_reserved()}", file=sys.stderr, flush=True)
    if _RELEASED_SNAPSHOT is not None:
        print("vllm_radiance lifecycle: " + json.dumps(lifecycle_diagnostics(_RELEASED_SNAPSHOT)), file=sys.stderr, flush=True)
        _RELEASED_SNAPSHOT = None
