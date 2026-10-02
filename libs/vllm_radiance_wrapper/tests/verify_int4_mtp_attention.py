import argparse
import ctypes
import importlib.util
import json
import os
from pathlib import Path
import statistics
import sys
import time

repo = Path(__file__).resolve().parents[3]
parser = argparse.ArgumentParser(description='Check INT4 MTP attention against vLLM, including graph replay.')
parser.add_argument('--prefill-dll', type=Path, required=True)
parser.add_argument('--decode-dll', type=Path, required=True)
parser.add_argument('--baseline-decode-dll', type=Path)
parser.add_argument('--runtime-root', type=Path, default=repo / 'build/runtime-radiance-probe')
parser.add_argument('--output', type=Path)
args = parser.parse_args()
runtime = args.runtime_root.resolve()
site = runtime / '.venv-rocm10/Lib/site-packages'
sdk = site / '_rocm_sdk_devel'
os.environ.update(VLLM_TARGET_DEVICE='rocm', VLLM_ENABLE_V1_MULTIPROCESSING='0', ROCM_PATH=str(sdk), HIP_PATH=str(sdk))
sys.path[:0] = [str(site), str(runtime / 'vLLM_for_AMD')]
handles = [os.add_dll_directory(str(path)) for path in (site, sdk)]
import torch
from vllm.v1.attention.ops.int4_per_token_head import reshape_and_cache_int4, unified_attention_int4
from vllm.v1.kv_cache_interface import KVQuantMode

overlay_path = repo / 'libs/vllm_radiance_wrapper/native/int4/r4d-feasibility/int4-fast/overlay/r4d_int4_prefill_overlay_wave64.py'
spec = importlib.util.spec_from_file_location('mtp_overlay', overlay_path)
overlay = importlib.util.module_from_spec(spec)
spec.loader.exec_module(overlay)
prefill_lib = ctypes.CDLL(str(args.prefill_dll.resolve()))
prefill = prefill_lib.r4d_int4_tiled_prefill_h256_gqa6
prefill.argtypes = [ctypes.POINTER(overlay.R4DInt4TiledArgs), ctypes.c_void_p]
prefill.restype = ctypes.c_int
verify_lib = ctypes.CDLL(str(args.decode_dll.resolve()))
verify = verify_lib.r4d_int4_verify_splitk
verify.argtypes = [ctypes.POINTER(overlay.R4DInt4DecodeArgs), ctypes.c_int32, ctypes.c_void_p]
verify.restype = ctypes.c_int
decode = verify_lib.r4d_int4_decode_splitk
decode.argtypes = [ctypes.POINTER(overlay.R4DInt4DecodeArgs), ctypes.c_void_p]
decode.restype = ctypes.c_int
baseline_decode = baseline_verify = None
if args.baseline_decode_dll:
    baseline_lib = ctypes.CDLL(str(args.baseline_decode_dll.resolve()))
    baseline_decode = baseline_lib.r4d_int4_decode_splitk
    baseline_decode.argtypes = decode.argtypes
    baseline_decode.restype = ctypes.c_int
    baseline_verify = baseline_lib.r4d_int4_verify_splitk
    baseline_verify.argtypes = verify.argtypes
    baseline_verify.restype = ctypes.c_int

def measure(fn):
    fn()
    torch.cuda.synchronize()
    times = []
    for _ in range(3):
        start = time.perf_counter()
        fn()
        torch.cuda.synchronize()
        times.append((time.perf_counter() - start) * 1000)
    return statistics.median(times)

def measure_graphs(graphs):
    samples = [[] for _ in graphs]
    for graph in graphs:
        graph.replay()
    torch.cuda.synchronize()
    for trial in range(5):
        order = range(len(graphs)) if trial % 2 == 0 else reversed(range(len(graphs)))
        for index in order:
            start = torch.cuda.Event(enable_timing=True)
            end = torch.cuda.Event(enable_timing=True)
            start.record()
            for _ in range(10):
                graphs[index].replay()
            end.record()
            end.synchronize()
            samples[index].append(start.elapsed_time(end) / 10)
    return [statistics.median(values) for values in samples]

results = []
torch.manual_seed(9100)
for context, query_count, page in ((1, 1, 64), (214, 1, 64), (32537, 1, 3104), (50000, 1, 3104), (100000, 1, 3104), (4, 2, 64), (214, 3, 64), (214, 4, 64), (32537, 4, 3104), (100000, 4, 3104)):
    pages = (context + page - 1) // page
    physical = torch.zeros((pages, page, 4, 264), device='cuda', dtype=torch.uint8)
    key, value = physical[..., :132], physical[..., 132:]
    scales = physical.view(torch.float32)
    ks, vs = scales[..., 32], scales[..., 65]
    keys = torch.randn((context, 4, 256), device='cuda', dtype=torch.bfloat16) * .25
    values = torch.randn_like(keys) * .25
    logical = torch.arange(context, device='cuda', dtype=torch.int32)
    slots = (pages - 1 - logical // page) * page + logical % page
    reshape_and_cache_int4(keys, values, key, value, slots, k_scale_cache=ks, v_scale_cache=vs)
    query = torch.randn((query_count, 24, 256), device='cuda', dtype=torch.bfloat16) * .25
    output = torch.empty_like(query)
    candidate_output = torch.empty_like(query)
    reference_output = torch.empty_like(query)
    cu = torch.tensor([0, query_count], device='cuda', dtype=torch.int32)
    lengths = torch.tensor([context], device='cuda', dtype=torch.int32)
    table = torch.arange(pages - 1, -1, -1, device='cuda', dtype=torch.int32).reshape(1, -1)
    kwargs = dict(q=query, k=key, v=value, out=output, cu_seqlens_q=cu, max_seqlen_q=4, seqused_k=lengths, max_seqlen_k=context, softmax_scale=256 ** -.5, window_size=(-1, -1), block_table=table, softcap=0., sinks=None, alibi_slopes=None, use_alibi_sqrt=False, qq_bias=None, output_scale=None, mm_prefix_range=None, k_scale_cache=ks, v_scale_cache=vs, causal=True, kv_quant_mode=KVQuantMode.INT4_PER_TOKEN_HEAD)
    def baseline():
        if baseline_decode is not None:
            overlay.run_int4_decode(kwargs, baseline_decode if query_count == 1 else baseline_verify, query_tokens=query_count)
        else:
            overlay.run_int4_prefill({**kwargs, 'max_seqlen_q': query_count}, prefill)
    def candidate():
        overlay.run_int4_decode({**kwargs, 'out': candidate_output}, decode if query_count == 1 else verify, query_tokens=query_count)
    kwargs['max_seqlen_q'] = query_count
    unified_attention_int4(query, key, value, reference_output, **{k:v for k,v in kwargs.items() if k not in ('q','k','v','out','causal','kv_quant_mode')})
    baseline()
    candidate()
    torch.cuda.synchronize()
    torch.testing.assert_close(output, reference_output, rtol=.02, atol=.02)
    torch.testing.assert_close(candidate_output, reference_output, rtol=.02, atol=.02)
    if baseline_decode is not None:
        torch.testing.assert_close(candidate_output, output, rtol=0, atol=0)
    baseline_ms = measure(baseline)
    candidate_ms = measure(candidate)
    graph = torch.cuda.CUDAGraph()
    with torch.cuda.graph(graph):
        candidate()
    graph.replay()
    torch.cuda.synchronize()
    torch.testing.assert_close(candidate_output, reference_output, rtol=.02, atol=.02)
    if baseline_decode is not None:
        torch.testing.assert_close(candidate_output, output, rtol=0, atol=0)
    candidate_graph_ms = None
    baseline_graph_ms = None
    if baseline_decode is not None:
        baseline_graph = torch.cuda.CUDAGraph()
        with torch.cuda.graph(baseline_graph):
            baseline()
        baseline_graph_ms, candidate_graph_ms = measure_graphs([baseline_graph, graph])
        torch.testing.assert_close(candidate_output, output, rtol=0, atol=0)
    else:
        candidate_graph_ms = measure_graphs([graph])[0]
    results.append(dict(context=context, query_count=query_count, page=page, baseline_ms=baseline_ms, candidate_ms=candidate_ms, speedup=baseline_ms/candidate_ms, candidate_graph_ms=candidate_graph_ms, baseline_graph_ms=baseline_graph_ms, graph_parity=True, bit_identical_baseline=True if baseline_decode is not None else None, max_abs_error=float((candidate_output.float()-reference_output.float()).abs().max().item())))
    del keys, values, physical, key, value, scales, ks, vs, kwargs
    torch.cuda.empty_cache()
print(json.dumps(results, indent=2))
if args.output:
    args.output.write_text(json.dumps(results, indent=2) + '\n', encoding='utf-8')
