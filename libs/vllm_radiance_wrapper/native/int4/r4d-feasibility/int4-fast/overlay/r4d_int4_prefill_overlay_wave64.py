"""Private opt-in hook for the R4D packed INT4 prefill kernel."""
from __future__ import annotations

import ctypes
import hashlib
import math
import sys
from contextlib import contextmanager
from pathlib import Path
from typing import Any, Iterator

import torch

HEAD_DIM = 256
Q_HEADS = 24
KV_HEADS = 4
VIRTUAL_PAGE = 16
VLLM_SOURCE_REVISION = "bf87782b97a961f86ab3a709800ef1b4561d5cda"


class R4DInt4TiledArgs(ctypes.Structure):
    _fields_ = [
        ("q", ctypes.c_void_p),
        ("k_cache", ctypes.c_void_p),
        ("v_cache", ctypes.c_void_p),
        ("k_scale_zp", ctypes.c_void_p),
        ("v_scale_zp", ctypes.c_void_p),
        ("block_table", ctypes.c_void_p),
        ("seq_lens", ctypes.c_void_p),
        ("out", ctypes.c_void_p),
        ("seqs", ctypes.c_int32),
        ("q_len", ctypes.c_int32),
        ("q_heads", ctypes.c_int32),
        ("kv_heads", ctypes.c_int32),
        ("head_dim", ctypes.c_int32),
        ("virtual_page_size", ctypes.c_int32),
        ("virtual_subpages", ctypes.c_int32),
        ("max_blocks", ctypes.c_int32),
        ("physical_page_size", ctypes.c_int32),
        ("k_page_stride_bytes", ctypes.c_int64),
        ("v_page_stride_bytes", ctypes.c_int64),
        ("k_token_stride_bytes", ctypes.c_int64),
        ("v_token_stride_bytes", ctypes.c_int64),
        ("k_head_stride_bytes", ctypes.c_int64),
        ("v_head_stride_bytes", ctypes.c_int64),
        ("k_scale_page_stride", ctypes.c_int64),
        ("v_scale_page_stride", ctypes.c_int64),
        ("k_scale_token_stride", ctypes.c_int64),
        ("v_scale_token_stride", ctypes.c_int64),
        ("k_scale_head_stride", ctypes.c_int64),
        ("v_scale_head_stride", ctypes.c_int64),
        ("softmax_scale", ctypes.c_float),
        ("cu_seqlens_q", ctypes.c_void_p),
    ]


class R4DInt4DecodeArgs(ctypes.Structure):
    _fields_ = [
        ("q", ctypes.c_void_p), ("k", ctypes.c_void_p), ("v", ctypes.c_void_p),
        ("k_scale", ctypes.c_void_p), ("v_scale", ctypes.c_void_p),
        ("block_table", ctypes.c_void_p), ("seq_lens", ctypes.c_void_p),
        ("partial_o", ctypes.c_void_p), ("partial_m", ctypes.c_void_p),
        ("partial_l", ctypes.c_void_p), ("out", ctypes.c_void_p),
        ("q_heads", ctypes.c_int32), ("kv_heads", ctypes.c_int32),
        ("head_dim", ctypes.c_int32), ("context", ctypes.c_int32),
        ("page_size", ctypes.c_int32), ("max_blocks", ctypes.c_int32),
        ("splits", ctypes.c_int32),
        ("k_page_stride", ctypes.c_int64), ("v_page_stride", ctypes.c_int64),
        ("k_token_stride", ctypes.c_int64), ("v_token_stride", ctypes.c_int64),
        ("k_head_stride", ctypes.c_int64), ("v_head_stride", ctypes.c_int64),
        ("scale_page_stride", ctypes.c_int64), ("scale_token_stride", ctypes.c_int64),
        ("scale_head_stride", ctypes.c_int64), ("scale", ctypes.c_float),
    ]


def sha256_file(path: Path) -> str:
    h_digest = hashlib.sha256()
    with path.open("rb") as f_source:
        for b_chunk in iter(lambda: f_source.read(1024 * 1024), b""):
            h_digest.update(b_chunk)
    return h_digest.hexdigest()


def _ptr(tensor: torch.Tensor) -> int:
    if not tensor.is_cuda:
        raise RuntimeError("R4D INT4 prefill requires CUDA-resident tensors")
    return tensor.data_ptr()


def _check_scalar_tensor(tensor: Any, name: str) -> int:
    # Shape-only: the value lives in device memory and the kernel reads it there.
    if not isinstance(tensor, torch.Tensor) or tensor.numel() != 1:
        raise RuntimeError(f"INT4 prefill requires one {name} value")
    return int(tensor.numel())


def _virtualize_table(table: torch.Tensor, context: int, physical_page: int, subpages: int) -> torch.Tensor:
    if table.dtype != torch.int32 or table.ndim != 2 or table.shape[0] < 1:
        raise RuntimeError("INT4 prefill requires int32 block-table rows")
    if not table.is_cuda or not table.is_contiguous():
        raise RuntimeError("INT4 prefill requires a contiguous CUDA block table")
    physical_count = (context + physical_page - 1) // physical_page
    if table.shape[1] < physical_count:
        raise RuntimeError("INT4 prefill block table is shorter than the KV context")
    # clamp_min, not a host-side `any()` read: a negative page would otherwise
    # index before the cache. The kernel only dereferences entries below the
    # device-side seq_lens value, so the padded tail is never touched.
    physical = table[:, :physical_count].clamp_min(0)
    logical_count = (context + VIRTUAL_PAGE - 1) // VIRTUAL_PAGE
    rows = table.shape[0]
    subpage_ids = torch.arange(subpages, dtype=torch.int32, device=table.device).view(1, 1, -1)
    expanded = (physical.reshape(rows, -1, 1) * subpages + subpage_ids).reshape(rows, -1)
    return expanded[:, :logical_count].contiguous()


def _validate_and_build(kwargs: dict[str, Any]) -> tuple[torch.Tensor, torch.Tensor, R4DInt4TiledArgs, int]:
    q, key, value, out = (kwargs.get(name) for name in ("q", "k", "v", "out"))
    if not isinstance(q, torch.Tensor) or q.dtype != torch.bfloat16 or q.ndim != 3:
        raise RuntimeError("INT4 R4D prefill requires BF16 q[Q,24,256]")
    if tuple(q.shape[1:]) != (Q_HEADS, HEAD_DIM) or not q.is_contiguous():
        raise RuntimeError("INT4 R4D prefill requires contiguous q[Q,24,256]")
    if not isinstance(out, torch.Tensor) or out.dtype != q.dtype or out.shape != q.shape or not out.is_contiguous():
        raise RuntimeError("INT4 R4D prefill requires contiguous BF16 output matching q")
    # A one-row batch is a decode step and belongs to the split-K kernel. A
    # multi-sequence batch with one row each is routed to decode by shape in
    # _run_batched_sequences, so anything reaching here has multi-row sequences.
    if q.shape[0] <= 1:
        raise RuntimeError("INT4 R4D kernel serves prefill only")
    if kwargs.get("causal") is not True:
        raise RuntimeError("INT4 R4D prefill requires causal=True")
    if kwargs.get("alibi_slopes") is not None or kwargs.get("sinks") is not None:
        raise RuntimeError("INT4 R4D prefill does not support ALiBi or sinks")
    if kwargs.get("qq_bias") is not None or kwargs.get("output_scale") is not None:
        raise RuntimeError("INT4 R4D prefill does not support query bias or output scaling")
    if kwargs.get("mm_prefix_range") is not None:
        raise RuntimeError("INT4 R4D prefill does not support multimodal prefix ranges")
    if kwargs.get("use_alibi_sqrt") not in (None, False):
        raise RuntimeError("INT4 R4D prefill does not support ALiBi square-root scaling")
    if kwargs.get("softcap") not in (None, 0, 0.0):
        raise RuntimeError("INT4 R4D prefill does not support softcap")
    if kwargs.get("window_size") not in (None, (-1, -1)):
        raise RuntimeError("INT4 R4D prefill does not support sliding windows")
    if kwargs.get("rswa_prefix_lens") is not None or kwargs.get("rswa_window") is not None:
        raise RuntimeError("INT4 R4D prefill does not support RSWA windows")
    if kwargs.get("mm_prefix_clamp_sliding_window", False):
        raise RuntimeError("INT4 R4D prefill does not support multimodal sliding windows")
    if kwargs.get("chunk_lookback", -1) != -1 or kwargs.get("use_td") not in (None, False):
        raise RuntimeError("INT4 R4D prefill does not support chunk lookback or tensor descriptors")
    if kwargs.get("kv_quant_mode") is None or "INT4_PER_TOKEN_HEAD" not in getattr(kwargs["kv_quant_mode"], "name", str(kwargs["kv_quant_mode"])):
        raise RuntimeError("INT4 R4D prefill hook received a non-INT4 cache mode")
    scale = kwargs.get("softmax_scale")
    if not isinstance(scale, (int, float)) or not math.isfinite(float(scale)) or not math.isclose(float(scale), HEAD_DIM ** -0.5, rel_tol=1e-6, abs_tol=1e-8):
        raise RuntimeError("INT4 R4D prefill received an unsupported softmax scale")
    if not isinstance(key, torch.Tensor) or not isinstance(value, torch.Tensor):
        raise RuntimeError("INT4 R4D prefill requires packed K and V tensors")
    if key.dtype != torch.uint8 or value.dtype != torch.uint8 or key.ndim != 4 or value.shape != key.shape:
        raise RuntimeError("INT4 R4D prefill requires matching uint8 K/V[blocks,page,4,132]")
    if key.shape[2:] != (KV_HEADS, HEAD_DIM // 2 + 4) or key.stride(-1) != 1 or value.stride(-1) != 1:
        raise RuntimeError("INT4 R4D prefill received an unsupported packed K/V layout")
    token_bytes = KV_HEADS * 2 * (HEAD_DIM // 2 + 4)
    head_bytes = 2 * (HEAD_DIM // 2 + 4)
    if key.stride(1) != token_bytes or value.stride(1) != token_bytes or key.stride(2) != head_bytes or value.stride(2) != head_bytes:
        raise RuntimeError("INT4 R4D prefill requires the pinned interleaved K/V token layout")
    if key.device != q.device or value.device != q.device or out.device != q.device:
        raise RuntimeError("INT4 R4D tensors must share one CUDA device")
    if key.stride() != value.stride():
        raise RuntimeError("INT4 K/V cache view strides differ")
    if key.untyped_storage().data_ptr() != value.untyped_storage().data_ptr():
        raise RuntimeError("INT4 K/V must remain views of the same packed cache")
    if value.data_ptr() != key.data_ptr() + HEAD_DIM // 2 + 4:
        raise RuntimeError("INT4 V cache view is not the second packed half of the pinned layout")
    physical_page = int(key.shape[1])
    if physical_page <= 0 or physical_page % VIRTUAL_PAGE:
        raise RuntimeError("INT4 physical page size must be divisible by 16")
    subpages = physical_page // VIRTUAL_PAGE

    k_scale, v_scale = kwargs.get("k_scale_cache"), kwargs.get("v_scale_cache")
    for name, tensor in (("k_scale_cache", k_scale), ("v_scale_cache", v_scale)):
        if not isinstance(tensor, torch.Tensor) or tensor.dtype != torch.float32 or tuple(tensor.shape) != (key.shape[0], physical_page, KV_HEADS):
            raise RuntimeError(f"INT4 R4D prefill requires float32 {name}[blocks,page,4]")
        if tensor.device != q.device:
            raise RuntimeError("INT4 scale tensors must share the query device")
        if any(stride % 4 for stride in key.stride()[:3]) or tuple(tensor.stride()) != tuple(stride // 4 for stride in key.stride()[:3]):
            raise RuntimeError("INT4 R4D prefill requires inline FP32 scales matching the packed cache strides")

    cu = kwargs.get("cu_seqlens_q")
    if not isinstance(cu, torch.Tensor) or cu.dtype != torch.int32 or cu.ndim != 1 or cu.numel() < 2 or not cu.is_contiguous():
        raise RuntimeError("INT4 R4D prefill requires a contiguous int32 cu_seqlens_q")
    _check_scalar_tensor(cu.reshape(-1)[0], "cu_seqlens_q[0]")
    _check_scalar_tensor(cu.reshape(-1)[1], "cu_seqlens_q[1]")
    seq_lens = kwargs.get("seqused_k")
    if not isinstance(seq_lens, torch.Tensor) or seq_lens.dtype != torch.int32 or seq_lens.ndim != 1 or seq_lens.numel() < 1 or not seq_lens.is_contiguous():
        raise RuntimeError("INT4 R4D prefill requires a contiguous int32 KV sequence length")
    context = int(kwargs.get("max_seqlen_k", 0))
    # Every bound below becomes a launch dimension or an address width, so it has
    # to be host-known before replay. max_seqlen_k/q are the batch-wide upper
    # bounds vLLM has already committed to for the captured shape, and the kernel
    # re-reads the true seqused_k and cu_seqlens from device memory per block, so
    # a padded bound costs addressing slack and nothing else. Calling .item() on
    # either tensor here is what failed capture with hipErrorStreamCaptureUnsupported.
    if context <= 0:
        raise RuntimeError("INT4 R4D prefill requires a positive max_seqlen_k")

    block_table = kwargs.get("block_table")
    if not isinstance(block_table, torch.Tensor):
        raise RuntimeError("INT4 R4D prefill requires a block table")
    virtual_table = _virtualize_table(block_table, context, physical_page, subpages)

    # max_seqlen_q is the per-sequence query bound, not the packed total, so it
    # only has to size grid.x. Comparing it against q.shape[0] would reject any
    # batch with more than one sequence; the true split comes from cu_seqlens_q
    # on the device.
    q_rows_bound = int(kwargs.get("max_seqlen_q", 0))
    if q_rows_bound <= 0:
        raise RuntimeError("INT4 R4D prefill requires a positive max_seqlen_q")
    sequences = int(cu.numel()) - 1
    if seq_lens.numel() != sequences or block_table.shape[0] != sequences:
        raise RuntimeError("INT4 R4D prefill sequence counts must match cu_seqlens_q")
    q_rht = _single_rht(q.float()).to(q.dtype)
    args = R4DInt4TiledArgs(
        _ptr(q_rht), _ptr(key), _ptr(value), _ptr(k_scale), _ptr(v_scale),
        _ptr(virtual_table), _ptr(seq_lens), _ptr(out),
        sequences, q_rows_bound, Q_HEADS, KV_HEADS, HEAD_DIM, VIRTUAL_PAGE,
        subpages, virtual_table.shape[1], physical_page,
        int(key.stride(0)), int(value.stride(0)), int(key.stride(1)), int(value.stride(1)),
        int(key.stride(2)), int(value.stride(2)),
        int(k_scale.stride(0)), int(v_scale.stride(0)), int(k_scale.stride(1)), int(v_scale.stride(1)),
        int(k_scale.stride(2)), int(v_scale.stride(2)),
        float(scale) / HEAD_DIM,
        _ptr(cu),
    )
    return q_rht, virtual_table, args, context


def _single_rht(tensor: torch.Tensor, *, inverse: bool = False) -> torch.Tensor:
    from vllm.v1.attention.ops.int4_per_token_head import single_rht

    return single_rht(tensor, inverse=inverse)


def run_int4_prefill(
    kwargs: dict[str, Any],
    fn: Any,
    *,
    compare_reference: bool = False,
    original: Any = None,
) -> dict[str, Any]:
    q_rht, virtual_table, args, context = _validate_and_build(kwargs)
    out = kwargs["out"]
    reference = None
    if compare_reference:
        if original is None:
            raise ValueError("compare_reference requires the upstream unified_attention callable")
        reference = torch.empty_like(out)
        ref_kwargs = dict(kwargs)
        ref_kwargs["out"] = reference
        original(**ref_kwargs)
    stream = torch.cuda.current_stream(device=out.device).cuda_stream
    status = int(fn(ctypes.byref(args), ctypes.c_void_p(stream)))
    if status != 0:
        raise RuntimeError(f"R4D INT4 prefill kernel returned {status}")
    out.copy_((_single_rht(out.float(), inverse=True) / HEAD_DIM).to(out.dtype))
    result: dict[str, Any] = {
        "output": out,
        "q_tokens": int(out.shape[0]),
        "context_tokens": context,
        "physical_page_size": args.physical_page_size,
        "virtual_subpages": args.virtual_subpages,
        "compare_reference": compare_reference,
    }
    if reference is not None:
        delta = out.float() - reference.float()
        result["reference_max_abs_error"] = float(delta.abs().max().item())
        result["reference_rms_error"] = float(delta.square().mean().sqrt().item())
        result["reference_cosine_similarity"] = float(torch.nn.functional.cosine_similarity(out.float().flatten(), reference.float().flatten(), dim=0).item())
    return result


def run_int4_decode(kwargs: dict[str, Any], fn: Any, *, query_tokens: int = 1) -> dict[str, Any]:
    q, key, value, out = (kwargs.get(name) for name in ("q", "k", "v", "out"))
    if query_tokens < 1 or query_tokens > 4:
        raise RuntimeError("INT4 R4D decode supports one to four query tokens")
    if not isinstance(q, torch.Tensor) or q.dtype != torch.bfloat16 or tuple(q.shape) != (query_tokens, Q_HEADS, HEAD_DIM) or not q.is_contiguous():
        raise RuntimeError("INT4 R4D decode requires contiguous BF16 q[query_tokens,24,256]")
    if not isinstance(out, torch.Tensor) or out.dtype != q.dtype or out.shape != q.shape or not out.is_contiguous():
        raise RuntimeError("INT4 R4D decode requires contiguous BF16 output matching q")
    if kwargs.get("max_seqlen_q") != query_tokens or kwargs.get("causal") is not True:
        raise RuntimeError("INT4 R4D decode requires matching query length and causal=True")
    cu_seqlens_q = kwargs.get("cu_seqlens_q")
    if not isinstance(cu_seqlens_q, torch.Tensor) or cu_seqlens_q.dtype != torch.int32 or cu_seqlens_q.numel() != 2 or not cu_seqlens_q.is_cuda or not cu_seqlens_q.is_contiguous():
        raise RuntimeError("INT4 R4D decode requires one contiguous CUDA int32 query sequence")
    if kwargs.get("alibi_slopes") is not None or kwargs.get("sinks") is not None:
        raise RuntimeError("INT4 R4D decode does not support ALiBi or sinks")
    if kwargs.get("qq_bias") is not None or kwargs.get("output_scale") is not None or kwargs.get("mm_prefix_range") is not None:
        raise RuntimeError("INT4 R4D decode does not support query bias, output scaling, or multimodal prefix ranges")
    if kwargs.get("use_alibi_sqrt") not in (None, False) or kwargs.get("softcap") not in (None, 0, 0.0):
        raise RuntimeError("INT4 R4D decode does not support ALiBi square-root scaling or softcap")
    if kwargs.get("window_size") not in (None, (-1, -1)) or kwargs.get("rswa_prefix_lens") is not None or kwargs.get("rswa_window") is not None:
        raise RuntimeError("INT4 R4D decode does not support sliding or RSWA windows")
    if kwargs.get("mm_prefix_clamp_sliding_window", False) or kwargs.get("chunk_lookback", -1) != -1 or kwargs.get("use_td") not in (None, False):
        raise RuntimeError("INT4 R4D decode does not support multimodal window clamping, chunk lookback, or tensor descriptors")
    if not isinstance(key, torch.Tensor) or not isinstance(value, torch.Tensor) or key.dtype != torch.uint8 or value.dtype != torch.uint8 or key.ndim != 4 or value.shape != key.shape:
        raise RuntimeError("INT4 R4D decode requires matching uint8 K/V[blocks,page,4,132]")
    if tuple(key.shape[2:]) != (KV_HEADS, HEAD_DIM // 2 + 4) or key.stride(-1) != 1 or value.stride(-1) != 1:
        raise RuntimeError("INT4 R4D decode received an unsupported packed K/V layout")
    if key.stride(1) != KV_HEADS * 2 * (HEAD_DIM // 2 + 4) or key.stride(2) != 2 * (HEAD_DIM // 2 + 4) or key.stride() != value.stride():
        raise RuntimeError("INT4 R4D decode requires the pinned interleaved K/V token layout")
    if key.untyped_storage().data_ptr() != value.untyped_storage().data_ptr() or value.data_ptr() != key.data_ptr() + HEAD_DIM // 2 + 4:
        raise RuntimeError("INT4 R4D decode requires K/V views of the pinned packed cache")
    if any(t.device != q.device for t in (key, value, out)):
        raise RuntimeError("INT4 R4D decode tensors must share one CUDA device")
    page_size = int(key.shape[1])
    context = kwargs.get("max_seqlen_k")
    if not isinstance(context, int) or context <= 0 or page_size <= 0:
        raise RuntimeError("INT4 R4D decode requires positive integer max_seqlen_k and page size")
    block_table = kwargs.get("block_table")
    seq_lens = kwargs.get("seqused_k")
    if not isinstance(block_table, torch.Tensor) or block_table.dtype != torch.int32 or block_table.ndim != 2 or block_table.shape[0] != 1 or not block_table.is_cuda or not block_table.is_contiguous():
        raise RuntimeError("INT4 R4D decode requires one contiguous CUDA int32 block-table row")
    max_blocks = (context + page_size - 1) // page_size
    if block_table.shape[1] < max_blocks:
        raise RuntimeError("INT4 R4D decode block table is shorter than max_seqlen_k")
    if not isinstance(seq_lens, torch.Tensor) or seq_lens.dtype != torch.int32 or seq_lens.numel() != 1 or not seq_lens.is_cuda or not seq_lens.is_contiguous():
        raise RuntimeError("INT4 R4D decode requires one contiguous CUDA int32 KV sequence length")
    scales = (kwargs.get("k_scale_cache"), kwargs.get("v_scale_cache"))
    for name, tensor in zip(("k_scale_cache", "v_scale_cache"), scales):
        if not isinstance(tensor, torch.Tensor) or tensor.dtype != torch.float32 or tuple(tensor.shape) != (key.shape[0], page_size, KV_HEADS) or tensor.device != q.device:
            raise RuntimeError(f"INT4 R4D decode requires float32 {name}[blocks,page,4] on the query device")
        if tuple(tensor.stride()) != tuple(stride // 4 for stride in key.stride()[:3]):
            raise RuntimeError("INT4 R4D decode requires inline FP32 scales matching packed cache strides")
    scale = kwargs.get("softmax_scale")
    if not isinstance(scale, (int, float)) or not math.isfinite(float(scale)) or not math.isclose(float(scale), HEAD_DIM ** -0.5, rel_tol=1e-6, abs_tol=1e-8):
        raise RuntimeError("INT4 R4D decode received an unsupported softmax scale")
    splits = 256
    partial_o = torch.empty((query_tokens * Q_HEADS, splits, HEAD_DIM), dtype=torch.bfloat16, device=q.device)
    partial_m = torch.empty((query_tokens * Q_HEADS, splits), dtype=torch.float32, device=q.device)
    partial_l = torch.empty_like(partial_m)
    q_rht = _single_rht(q.float()).to(q.dtype)
    out_rht = torch.empty_like(out)
    k_scale, v_scale = scales
    args = R4DInt4DecodeArgs(
        _ptr(q_rht), _ptr(key), _ptr(value), _ptr(k_scale), _ptr(v_scale),
        _ptr(block_table), _ptr(seq_lens), _ptr(partial_o), _ptr(partial_m), _ptr(partial_l), _ptr(out_rht),
        Q_HEADS, KV_HEADS, HEAD_DIM, context, page_size, key.shape[0], splits,
        int(key.stride(0)), int(value.stride(0)), int(key.stride(1)), int(value.stride(1)),
        int(key.stride(2)), int(value.stride(2)), int(k_scale.stride(0)), int(k_scale.stride(1)),
        int(k_scale.stride(2)), float(scale) / HEAD_DIM,
    )
    stream = torch.cuda.current_stream(device=out.device).cuda_stream
    status = int(fn(ctypes.byref(args), ctypes.c_void_p(stream))) if query_tokens == 1 else int(fn(ctypes.byref(args), query_tokens, ctypes.c_void_p(stream)))
    if status != 0:
        raise RuntimeError(f"R4D INT4 decode kernel returned {status}")
    out.copy_((_single_rht(out_rht.float(), inverse=True) / HEAD_DIM).to(out.dtype))
    return {"output": out, "q_tokens": query_tokens, "context_tokens": context, "physical_page_size": page_size, "splits": splits}


def _run_batched_sequences(
    kwargs: dict[str, Any],
    prefill: Any,
    decode: Any,
) -> list[dict[str, Any]]:
    """Route a packed vLLM batch to the native kernels without reading the host.

    Every branch here keys off tensor shapes only. The per-sequence query split
    and KV lengths live in cu_seqlens_q and seqused_k, which stay on the device:
    the prefill kernel reads its own row range, and the decode loop only needs
    tensor views. Reading either with .item() is what made this path fail CUDA
    graph capture with hipErrorStreamCaptureUnsupported.
    """
    q, out = kwargs.get("q"), kwargs.get("out")
    cu = kwargs.get("cu_seqlens_q")
    seq_lens = kwargs.get("seqused_k")
    block_table = kwargs.get("block_table")
    if not isinstance(q, torch.Tensor) or not isinstance(out, torch.Tensor) or q.ndim != 3 or out.shape != q.shape:
        raise RuntimeError("INT4 R4D batched routing requires matching packed q/out tensors")
    if not isinstance(cu, torch.Tensor) or cu.dtype != torch.int32 or cu.ndim != 1 or not cu.is_contiguous() or cu.numel() < 3:
        raise RuntimeError("INT4 R4D batched routing requires contiguous int32 cu_seqlens_q for multiple sequences")
    if not isinstance(seq_lens, torch.Tensor) or seq_lens.dtype != torch.int32 or seq_lens.ndim != 1 or not seq_lens.is_contiguous():
        raise RuntimeError("INT4 R4D batched routing requires contiguous int32 seqused_k")
    sequences = cu.numel() - 1
    if seq_lens.numel() != sequences or not isinstance(block_table, torch.Tensor) or block_table.ndim != 2 or block_table.shape[0] != sequences:
        raise RuntimeError("INT4 R4D query offsets, KV lengths, and block-table rows must have matching sequence counts")
    if q.shape[0] < sequences:
        raise RuntimeError("INT4 R4D batched routing requires at least one query row per sequence")
    # One packed row per sequence means every sequence is a decode step, which is
    # the shape a captured uniform-decode batch has. A shape test rather than a
    # cu_seqlens read is what keeps this branch capture-safe.
    if q.shape[0] == sequences:
        if decode is None:
            raise RuntimeError("INT4 R4D batched decode requires the native decode kernel")
        return [_decode_sequence(kwargs, decode, index) for index in range(sequences)]
    # Mixed or multi-row batch: the prefill kernel walks cu_seqlens_q itself, so
    # one launch covers a ragged split that used to need a host loop.
    return [prefill(kwargs)]


def _decode_sequence(kwargs: dict[str, Any], decode: Any, index: int) -> dict[str, Any]:
    """Decode one row of a packed batch using device-side views only."""
    q, out = kwargs["q"], kwargs["out"]
    cu, seq_lens, block_table = kwargs["cu_seqlens_q"], kwargs["seqused_k"], kwargs["block_table"]
    sequence_kwargs = dict(kwargs)
    sequence_kwargs["q"] = q[index:index + 1]
    sequence_kwargs["out"] = out[index:index + 1]
    sequence_kwargs["cu_seqlens_q"] = cu[index:index + 2]
    sequence_kwargs["seqused_k"] = seq_lens[index:index + 1]
    sequence_kwargs["block_table"] = block_table[index:index + 1]
    sequence_kwargs["max_seqlen_q"] = 1
    # The decode kernel takes its true length from seqused_k on the device, so
    # the batch-wide bound is a correct and capture-stable stand-in here.
    sequence_kwargs["max_seqlen_k"] = int(kwargs.get("max_seqlen_k", 0))
    return decode(sequence_kwargs)


@contextmanager
def install_int4_prefill_overlay(
    dll_path: Path,
    expected_dll_sha256: str,
    *,
    compare_reference: bool = False,
    decode_dll_path: Path | None = None,
    expected_decode_dll_sha256: str | None = None,
    decode_fallback_to_upstream: bool = False,
) -> Iterator[dict[str, Any]]:
    dll_path = Path(dll_path).resolve()
    actual_hash = sha256_file(dll_path)
    if actual_hash.lower() != expected_dll_sha256.lower():
        raise RuntimeError(f"R4D INT4 DLL hash mismatch: expected {expected_dll_sha256}, got {actual_hash}")
    if ctypes.sizeof(R4DInt4TiledArgs) != 216 or R4DInt4TiledArgs.softmax_scale.offset != 200:
        raise RuntimeError("R4D INT4 ctypes declaration does not match the header ABI")
    library = ctypes.CDLL(str(dll_path))
    fn = library.r4d_int4_tiled_prefill_h256_gqa6
    fn.argtypes = [ctypes.POINTER(R4DInt4TiledArgs), ctypes.c_void_p]
    fn.restype = ctypes.c_int
    decode_fn = None
    verify_fn = None
    decode_hash = None
    if (decode_dll_path is None) != (expected_decode_dll_sha256 is None):
        raise ValueError("decode_dll_path and expected_decode_dll_sha256 must be supplied together")
    if decode_dll_path is not None:
        decode_dll_path = Path(decode_dll_path).resolve()
        decode_hash = sha256_file(decode_dll_path)
        if decode_hash.lower() != expected_decode_dll_sha256.lower():
            raise RuntimeError(f"R4D INT4 decode DLL hash mismatch: expected {expected_decode_dll_sha256}, got {decode_hash}")
        if ctypes.sizeof(R4DInt4DecodeArgs) != 200 or R4DInt4DecodeArgs.scale.offset != 192:
            raise RuntimeError("R4D INT4 decode ctypes declaration does not match the header ABI")
        decode_library = ctypes.CDLL(str(decode_dll_path))
        decode_fn = decode_library.r4d_int4_decode_splitk
        decode_fn.argtypes = [ctypes.POINTER(R4DInt4DecodeArgs), ctypes.c_void_p]
        decode_fn.restype = ctypes.c_int
        verify_fn = getattr(decode_library, "r4d_int4_verify_splitk", None)
        if verify_fn is not None:
            verify_fn.argtypes = [ctypes.POINTER(R4DInt4DecodeArgs), ctypes.c_int32, ctypes.c_void_p]
            verify_fn.restype = ctypes.c_int
    module = __import__("vllm.v1.attention.backends.triton_attn", fromlist=["unified_attention"])
    original = module.unified_attention
    mode_type = __import__("vllm.v1.kv_cache_interface", fromlist=["KVQuantMode"]).KVQuantMode
    int4_mode = mode_type.INT4_PER_TOKEN_HEAD
    artifact_root = Path(__file__).resolve().parents[1]
    source_path = artifact_root / "kernel" / "r4d_int4_tiled_prefill.hip"
    header_path = artifact_root / "kernel" / "r4d_int4_tiled.h"
    info: dict[str, Any] = {
        "dll": str(dll_path),
        "dll_sha256": actual_hash,
        "symbol": "r4d_int4_tiled_prefill_h256_gqa6",
        "abi_size": ctypes.sizeof(R4DInt4TiledArgs),
        "kernel_source": str(source_path),
        "kernel_source_sha256": sha256_file(source_path),
        "kernel_header": str(header_path),
        "kernel_header_sha256": sha256_file(header_path),
        "vllm_source_revision": VLLM_SOURCE_REVISION,
        "dispatch_calls": 0,
        "decode_dispatch_calls": 0,
        "decode_enabled": decode_fn is not None,
        "verify_enabled": verify_fn is not None,
        "verify_dispatch_calls": 0,
        "decode_dll": str(decode_dll_path) if decode_dll_path is not None else None,
        "decode_dll_sha256": decode_hash,
        "decode_symbol": "r4d_int4_decode_splitk" if decode_fn is not None else None,
        "decode_fallback_to_upstream": decode_fallback_to_upstream,
        "reference_comparisons": 0,
        "restored": False,
        "physical_page_sizes": [],
    }

    def wrapped(**kwargs: Any) -> Any:
        mode = kwargs.get("kv_quant_mode")
        q = kwargs.get("q")
        if mode != int4_mode or not isinstance(q, torch.Tensor):
            return original(**kwargs)
        cu = kwargs.get("cu_seqlens_q")
        if isinstance(cu, torch.Tensor) and cu.numel() > 2:
            try:
                sequence_results = _run_batched_sequences(
                    kwargs,
                    lambda sequence_kwargs: run_int4_prefill(
                        sequence_kwargs, fn, compare_reference=compare_reference, original=original
                    ),
                    (lambda sequence_kwargs: run_int4_decode(sequence_kwargs, decode_fn)) if decode_fn is not None else None,
                )
            except (RuntimeError, ValueError) as exc:
                raise RuntimeError(f"R4D INT4 batched dispatch rejected request: {exc}") from exc
            info["dispatch_calls"] += sum(result["q_tokens"] > 1 for result in sequence_results)
            info["decode_dispatch_calls"] += sum(result["q_tokens"] == 1 for result in sequence_results)
            info["reference_comparisons"] += sum(bool(result.get("compare_reference")) for result in sequence_results)
            info["physical_page_sizes"].extend(result["physical_page_size"] for result in sequence_results)
            info["last_batched_call"] = [
                {k: v for k, v in result.items() if k != "output"} for result in sequence_results
            ]
            return kwargs["out"]
        query_tokens = int(kwargs.get("max_seqlen_q", 0))
        if verify_fn is not None and 2 <= query_tokens <= 4 and q.shape[0] == query_tokens:
            result = run_int4_decode(kwargs, verify_fn, query_tokens=query_tokens)
            info["verify_dispatch_calls"] += 1
            info["verify_last_call"] = {k: v for k, v in result.items() if k != "output"}
            return result["output"]
        if query_tokens == 1 and decode_fn is not None:
            try:
                result = run_int4_decode(kwargs, decode_fn)
            except (RuntimeError, ValueError) as exc:
                if decode_fallback_to_upstream:
                    info["decode_fallback_calls"] = info.get("decode_fallback_calls", 0) + 1
                    info["last_decode_fallback_reason"] = str(exc)
                    return original(**kwargs)
                raise RuntimeError(f"R4D INT4 decode dispatch rejected request: {exc}") from exc
            info["decode_dispatch_calls"] += 1
            info["decode_last_call"] = {k: v for k, v in result.items() if k != "output"}
            return result["output"]
        if int(kwargs.get("max_seqlen_q", 0)) <= 1:
            raise RuntimeError("R4D INT4 native decode kernel is unavailable")
        result = run_int4_prefill(kwargs, fn, compare_reference=compare_reference, original=original)
        info["dispatch_calls"] += 1
        info["reference_comparisons"] += int(compare_reference)
        info["physical_page_sizes"].append(result["physical_page_size"])
        info["last_call"] = {k: v for k, v in result.items() if k != "output"}
        return result["output"]

    module.unified_attention = wrapped
    print(f"event=int4_mtp_verify enabled={str(verify_fn is not None).lower()} decode_sha256={decode_hash}", file=sys.stderr, flush=True)
    try:
        yield info
    finally:
        module.unified_attention = original
        info["restored"] = True
