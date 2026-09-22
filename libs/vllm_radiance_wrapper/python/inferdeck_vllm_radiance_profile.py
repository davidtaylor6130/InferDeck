"""Pinned synchronous V1 InprocClient bridge. No inference worker or HTTP server; Triton may invoke compiler tools."""
from __future__ import annotations
import importlib.util
import importlib
import copy
import faulthandler
import json
import gc
import hashlib
import math
import re
from inferdeck_vllm_radiance_lifecycle import release_language_model_cache, capture_lifecycle_refs, lifecycle_diagnostics, finalize_engine_caches
import os
import sys
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
_PREFILL_ATTENTION_OPTIONS = frozenset(("r4d", "upstream"))
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
    if int(c.get("context_size",0))!=106496 or int(c.get("n_slots",0))!=1 or int(c.get("min_slots",0))!=1:raise RuntimeError("requires 106496 context and exactly one slot")
    prefill_attention = c.get("prefill_attention", _PREFILL_ATTENTION_DEFAULT)
    if not isinstance(prefill_attention, str) or prefill_attention not in _PREFILL_ATTENTION_OPTIONS:
        raise RuntimeError("prefill_attention must be one of: r4d, upstream")
    optional_r4d = {"prefill_overlay", "prefill_dll", "prefill_dll_sha256"}
    for key in _REQUIRED:
        if prefill_attention == "upstream" and key in optional_r4d:
            continue
        if key == "prefill_dll_sha256":
            if not re.fullmatch(r"[0-9a-fA-F]{64}", str(c.get(key, ""))):
                raise RuntimeError("prefill_dll_sha256 must be a SHA-256 digest")
        else:
            _need(c, key)
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
    diagnostic_started = False
    try:
        try:
            faulthandler.dump_traceback_later(60, repeat=True, file=sys.stderr)
            diagnostic_started = True
        except (RuntimeError, OSError, ValueError, AttributeError) as error:
            print(f"vllm_radiance load stack diagnostics unavailable: {error}", file=sys.stderr, flush=True)
        return _create(c)
    finally:
        if diagnostic_started:
            faulthandler.cancel_dump_traceback_later()


def _create(c:dict[str,Any])->dict[str,Any]:
    validate_config(c)
    os.environ.setdefault("GPU_RESOURCE_CACHE_SIZE", "64")
    prefill_attention = c.get("prefill_attention", _PREFILL_ATTENTION_DEFAULT)
    for path in reversed([_need(c,key) for key in ("python_site","vllm_source","radiance_source","radiance_extension")]):
        if path not in sys.path:sys.path.insert(0,path)
    os.environ.update({"PYTHONNOUSERSITE":"1","PYTHONDONTWRITEBYTECODE":"1","VLLM_TARGET_DEVICE":"rocm","VLLM_ENABLE_V1_MULTIPROCESSING":"0","VLLM_NO_USAGE_STATS":"1","VLLM_USE_RUST_FRONTEND":"0","ROCM_PATH":_need(c,"rocm"),"HIP_PATH":_need(c,"rocm"),"RADIANCE_MXFP4_W4A8":"1","RADIANCE_MXFP4":"1","RADIANCE_MXFP4_WPERM":"1","RADIANCE_MXFP4_A_TILED_MIN_M":"513","RADIANCE_MXFP4_W4A8_MIN_M":"0","RADIANCE_MXFP4_DECODE_MAX_M":"8","RADIANCE_MXFP4_R4D_DECODE_MAX_M":"0","RADIANCE_FUSE_RMS_QUANT":"1"})
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
        if prefill_attention == "r4d":
            prefill=_load("inferdeck_prefill",_need(c,"prefill_overlay")).install_r4d_prefill_overlay(Path(_need(c,"prefill_dll")),compare_reference=False,expected_dll_sha256=c["prefill_dll_sha256"]);prefill.__enter__();contexts.insert(0,prefill)
        else:
            _verify_upstream_prefill_attention()
        model=_need(c,"model");tokenizer_revision=register_tokenizer(model)
        engine=LLMEngine.from_engine_args(EngineArgs(model=model,logits_processors=[InferDeckPenaltiesProcessor],tokenizer=model,tokenizer_mode=MODE,tokenizer_revision=tokenizer_revision,trust_remote_code=False,load_format="safetensors",quantization="quark",max_model_len=106496,max_num_batched_tokens=4096,max_num_seqs=1,tensor_parallel_size=1,pipeline_parallel_size=1,enforce_eager=False,gpu_memory_utilization=.90,seed=1234,enable_prefix_caching=True,attention_backend="TRITON_ATTN",reasoning_parser="qwen3",compilation_config={"mode":0,"cudagraph_mode":"FULL_DECODE_ONLY","cudagraph_capture_sizes":[1]}),enable_multiprocessing=False)
        tokenizer=cached_tokenizer_from_config(engine.vllm_config.model_config)
        if engine.vllm_config.model_config.enable_prompt_embeds:raise RuntimeError("native pooled tokenizer does not support prompt embeds")
        if not isinstance(engine.engine_core,InprocClient):raise RuntimeError("V1 InprocClient was not constructed")
        if engine.vllm_config.model_config.enforce_eager or engine.vllm_config.scheduler_config.max_num_batched_tokens!=4096 or engine.vllm_config.scheduler_config.max_num_seqs!=1:raise RuntimeError("effective engine differs from measured profile")
        if prefill_attention == "upstream":
            _verify_upstream_prefill_attention()
        print(f"vllm_radiance prefill_attention={prefill_attention}", file=sys.stderr, flush=True)
        return {"engine":engine,"tokenizer":tokenizer,"SamplingParams":SamplingParams,"active":set(),"requests":{},"contexts":tuple(contexts),"prefill_attention":prefill_attention}
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
    if r.get("logprobs"):raise RuntimeError("logprobs are unsupported by vllm_radiance")
    from vllm.entrypoints.openai.chat_completion.protocol import ChatCompletionRequest
    from vllm.parser.qwen3 import Qwen3Parser
    from vllm.tool_parsers.qwen3_engine_tool_parser import Qwen3EngineToolParser
    from vllm.sampling_params import RequestOutputKind, StructuredOutputsParams
    from vllm.tool_parsers.structural_tag_registry import get_model_structural_tag
    request_kwargs=dict(model=r["model"],messages=r["messages"],tools=r.get("tools") or None,tool_choice=(r.get("tool_choice") or "auto") if r.get("tools") else "none",include_reasoning=bool(r.get("include_reasoning",True)),stream=True)
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
    if r.get("enable_reasoning") is not None:kwargs["enable_thinking"]=r["enable_reasoning"]
    if r.get("reasoning_effort"):kwargs["reasoning_effort"]=r["reasoning_effort"]
    template_messages = copy.deepcopy(r["messages"])
    for message in template_messages:
        for call in message.get("tool_calls", ()):
            function = call.get("function", {})
            if isinstance(function.get("arguments"), str):
                function["arguments"] = json.loads(function["arguments"])
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
    rid=uuid.uuid4().hex;s["engine"].add_request(rid,ids,sampling,priority=0);s["active"].add(rid);s["requests"][rid]={"request":request,"parser":Qwen3Parser(s["tokenizer"],request.tools,chat_template_kwargs=parser_kwargs),"ids":ids,"completion_tokens":0,"had_tools":False};return rid
def step(s:dict[str,Any],rid:str,r:dict[str,Any])->list[dict[str,Any]]:
    state=s["requests"].get(rid)
    if state is None:raise RuntimeError("unknown vllm_radiance request")
    result=[]
    for output in s["engine"].step():
        if output.request_id!=rid:continue
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
        if done and all(math.isfinite(value) for value in (scheduled, first, last)) and 0 < scheduled <= first <= last:
            result[-1]["prompt_duration_ms"] = (first - scheduled) * 1000.0
            result[-1]["generation_duration_ms"] = (last - first) * 1000.0
        if done:s["active"].discard(rid);s["requests"].pop(rid,None)
    return result
def abort(s:dict[str,Any],rid:str)->None:
    if rid in s["active"]:s["engine"].abort_request([rid]);s["active"].discard(rid);s["requests"].pop(rid,None)
def active_count(s:dict[str,Any])->int:return len(s["active"])
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
