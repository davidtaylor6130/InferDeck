"""Release and inspect model-scoped references in the pinned vLLM runtime."""

import gc
import sys
import weakref
import types
from typing import Any


def _runner(engine: Any) -> Any:
    client = getattr(engine, "engine_core", None)
    core = getattr(client, "engine_core", None)
    executor = getattr(core, "model_executor", None)
    wrapper = getattr(executor, "driver_worker", None)
    worker = getattr(wrapper, "worker", wrapper)
    return getattr(worker, "model_runner", None)


def _model_chain(engine: Any) -> list[Any]:
    model = getattr(_runner(engine), "model", None)
    chain = []
    for _ in range(5):
        if model is None:
            break
        chain.append(model)
        child = getattr(model, "runnable", None)
        if child is model:
            break
        model = child
    return chain


def _weak(value: Any) -> dict[str, Any]:
    result = {"id": id(value), "type": type(value).__name__}
    try:
        result["ref"] = weakref.ref(value)
    except TypeError:
        result["ref"] = None
    return result


def _state(entry: dict[str, Any]) -> dict[str, Any]:
    reference = entry["ref"]
    return {"id": entry["id"], "type": entry["type"],
            "weakref_supported": reference is not None,
            "alive": reference() is not None if reference is not None else None}


def _referrers(value: Any, depth: int = 0) -> list[dict[str, Any]]:
    result = []
    for referrer in gc.get_referrers(value)[:16]:
        item = {"type": type(referrer).__name__}
        if isinstance(referrer, dict):
            item["keys"] = [key[:80] if isinstance(key, str) else "<" + type(key).__name__ + ">" for key, item_value in referrer.items()
                            if item_value is value][:8]
        elif isinstance(referrer, (list, tuple)):
            item["length"] = len(referrer)
            if isinstance(referrer, tuple) and depth < 3:
                item["owners"] = _referrers(referrer, depth + 1)
        elif isinstance(referrer, types.CellType) and depth < 3:
            item["owners"] = _referrers(referrer, depth + 1)
        elif isinstance(referrer, (types.FunctionType, types.MethodType)) or type(referrer).__name__ == "_lru_cache_wrapper":
            item["function"] = getattr(referrer, "__qualname__", "")
            item["module"] = getattr(referrer, "__module__", "")
        elif isinstance(referrer, types.FrameType):
            item["function"] = referrer.f_code.co_name
            item["file"] = referrer.f_code.co_filename
        result.append(item)
    return result


def capture_lifecycle_refs(engine: Any) -> dict[str, Any]:
    """Capture only weak references before shutdown; this does not retain a model."""
    runner = _runner(engine)
    config = getattr(engine, "vllm_config", None)
    compilation = getattr(config, "compilation_config", None)
    layers = getattr(compilation, "static_forward_context", {})
    layers = layers if isinstance(layers, dict) else {}
    forward = sys.modules.get("vllm.forward_context")
    context = getattr(forward, "_forward_context", None)
    return {
        "engine": _weak(engine),
        "runner": _weak(runner) if runner is not None else None,
        "models": [_weak(model) for model in _model_chain(engine)],
        "layers": [_weak(layer) for layer in layers.values()],
        "engine_config_id": id(config) if config is not None else None,
        "forward_context": _weak(context) if context is not None else None,
    }


def lifecycle_diagnostics(snapshot: dict[str, Any]) -> dict[str, Any]:
    """Return bounded non-mutating retention evidence after shutdown and GC."""
    engine_ref = snapshot["engine"]["ref"]
    engine = engine_ref() if engine_ref is not None else None
    runner_ref = snapshot.get("runner", {}).get("ref") if snapshot.get("runner") else None
    runner = runner_ref() if runner_ref is not None else None
    runner_model = getattr(runner, "model", None)
    interfaces = sys.modules.get("vllm.model_executor.models.interfaces")
    language_cache = getattr(interfaces, "_language_model_by_module", None)
    config_module = sys.modules.get("vllm.config.vllm")
    current_config = getattr(config_module, "_current_vllm_config", None)
    compilation = getattr(current_config, "compilation_config", None)
    static_layers = getattr(compilation, "static_forward_context", None)
    forward_module = sys.modules.get("vllm.forward_context")
    forward_context = getattr(forward_module, "_forward_context", None)
    cached = getattr(config_module, "get_cached_compilation_config", None)
    cache_info = cached.cache_info() if callable(getattr(cached, "cache_info", None)) else None
    model_entries = snapshot.get("models", ())
    model_ids = {entry["id"] for entry in model_entries}
    live_models = [entry["ref"]() for entry in model_entries if entry["ref"] is not None and entry["ref"]() is not None]
    return {
        "engine": _state(snapshot["engine"]),
        "engine_referrers": _referrers(engine) if engine is not None else [],
        "gc": {"enabled": gc.isenabled(), "frozen": gc.get_freeze_count(), "collect_module": getattr(gc.collect, "__module__", None), "collect_name": getattr(gc.collect, "__qualname__", None)},
        "runner": {"captured": _state(snapshot["runner"]) if snapshot.get("runner") else None,
                   "model_present": runner_model is not None,
                   "model_matches_captured": id(runner_model) in model_ids if runner_model is not None else False},
        "models": [_state(entry) for entry in model_entries],
        "captured_layers": [_state(entry) for entry in snapshot.get("layers", ())],
        "live_model_referrers": [_referrers(model) for model in live_models],
        "language_model_cache": {"present": isinstance(language_cache, dict),
                                  "size": len(language_cache) if isinstance(language_cache, dict) else None,
                                  "contains_captured_model": any(id(key) in model_ids for key in language_cache) if isinstance(language_cache, dict) else False},
        "current_config": {"present": current_config is not None,
                           "matches_engine_config": id(current_config) == snapshot.get("engine_config_id"),
                           "static_forward_context_size": len(static_layers) if isinstance(static_layers, dict) else None},
        "forward_context": {"captured": _state(snapshot["forward_context"]) if snapshot.get("forward_context") else None,
                            "present": forward_context is not None,
                            "uses_current_static_layers": bool(forward_context is not None and getattr(forward_context, "no_compile_layers", None) is static_layers)},
        "compilation_config_cache": {"hits": getattr(cache_info, "hits", None),
                                     "misses": getattr(cache_info, "misses", None),
                                     "currsize": getattr(cache_info, "currsize", None)},
    }


def release_language_model_cache(engine: Any) -> int:
    interfaces = sys.modules.get("vllm.model_executor.models.interfaces")
    cache = getattr(interfaces, "_language_model_by_module", None)
    if cache is None:
        return 0
    if not isinstance(cache, dict):
        raise RuntimeError("unsupported vLLM language-model cache representation")
    removed = 0
    for model in _model_chain(engine):
        if model in cache:
            del cache[model]
            removed += 1
    return removed

def finalize_engine_caches(engine: Any) -> bool:
    finalizer = getattr(engine, "_finalizer", None)
    if finalizer is None:
        return False
    if not isinstance(finalizer, weakref.finalize):
        raise RuntimeError("unsupported vLLM engine finalizer representation")
    active = finalizer.alive
    finalizer()
    return active
