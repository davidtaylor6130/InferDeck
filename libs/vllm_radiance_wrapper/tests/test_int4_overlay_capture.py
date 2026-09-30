"""Guards the INT4 overlay's CUDA-graph capture contract.

The prefill dispatch used to read cu_seqlens_q, seqused_k and the virtualised
block table on the host and raise during capture, which is what made MTP and
CUDA graphs mutually exclusive. Nothing in a replayed graph may synchronise with
the device, so these tests assert the property directly instead of relying on a
regression to reproduce it on a GPU.
"""
import ast
import ctypes
import unittest
from pathlib import Path
from types import SimpleNamespace as NS

OVERLAY = (Path(__file__).resolve().parents[1] / "native" / "int4" / "r4d-feasibility"
           / "int4-fast" / "overlay" / "r4d_int4_prefill_overlay_wave64.py")
HEADER = (Path(__file__).resolve().parents[1] / "native" / "int4" / "r4d-feasibility"
          / "int4-fast" / "kernel" / "r4d_int4_tiled.h")


def _functions(path):
    """Map function name -> ast node for the module, ignoring nested defs."""
    tree = ast.parse(path.read_text(encoding="utf8"))
    return {node.name: node for node in tree.body if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))}


def _device_reads(node):
    """Names of calls that would synchronise a CUDA tensor with the host."""
    reads = set()
    for child in ast.walk(node):
        if not isinstance(child, ast.Call):
            continue
        func = child.func
        if isinstance(func, ast.Attribute) and func.attr in ("item", "tolist", "numpy"):
            reads.add(func.attr)
        elif isinstance(func, ast.Name) and func.id in ("int", "float", "bool"):
            for arg in child.args:
                if isinstance(arg, ast.Call) and isinstance(arg.func, ast.Attribute) and arg.func.attr in ("item", "tolist"):
                    reads.add(arg.func.attr)
    return reads


class Int4OverlayCaptureContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = OVERLAY.read_text(encoding="utf8")
        cls.functions = _functions(OVERLAY)

    def test_dispatch_path_never_synchronises_with_the_device(self):
        # These run inside a captured graph. A single .item() here reproduces
        # hipErrorStreamCaptureUnsupported at load and silently disables graphs.
        # run_int4_prefill is excluded because its reference-comparison branch
        # measures on the host; the invariant that keeps that branch off the
        # capture path is asserted separately below.
        for name in ("_validate_and_build", "_virtualize_table", "_run_batched_sequences",
                     "_decode_sequence", "run_int4_decode"):
            with self.subTest(function=name):
                self.assertEqual(_device_reads(self.functions[name]), set(),
                                 f"{name} synchronises with the device and cannot be captured")

    def test_reference_comparison_defaults_off(self):
        # The only host reads left in the overlay are the reference-parity
        # numbers. They cannot run inside a captured graph, so the overlay's
        # install context must default the flag off and only an explicit
        # diagnostic may turn it on.
        node = self.functions["run_int4_prefill"]
        # Keyword-only defaults live in kw_defaults, not args.defaults.
        defaults = {arg.arg: default for arg, default in zip(node.args.kwonlyargs, node.args.kw_defaults)
                    if isinstance(default, ast.Constant)}
        self.assertIn("compare_reference", defaults)
        self.assertIs(defaults["compare_reference"].value, False)

    def test_overlay_never_refuses_capture(self):
        self.assertNotIn("is_current_stream_capturing", self.source)

    def test_batched_routing_does_not_allocate_per_sequence(self):
        # A per-sequence torch.tensor(...) is a host-to-device copy, which is
        # illegal during capture. Slicing existing tensors is not.
        node = self.functions["_run_batched_sequences"]
        self.assertEqual([child for child in ast.walk(node)
                          if isinstance(child, ast.Call) and isinstance(child.func, ast.Attribute)
                          and child.func.attr == "tensor"], [])

    def test_ctypes_declaration_matches_the_kernel_header(self):
        namespace = {"ctypes": ctypes}
        # Evaluate only the struct definitions from the module source.
        tree = ast.parse(self.source)
        wanted = [node for node in tree.body if isinstance(node, ast.ClassDef)
                  and node.name.startswith("R4DInt4")]
        exec(compile(ast.Module(body=wanted, type_ignores=[]), str(OVERLAY), "exec"), namespace)
        struct = namespace["R4DInt4TiledArgs"]
        self.assertEqual(ctypes.sizeof(struct), 216)
        self.assertEqual(struct.softmax_scale.offset, 200)
        self.assertEqual(struct.cu_seqlens_q.offset, 208)
        # The kernel reads its per-sequence query range from device memory.
        self.assertIn(("cu_seqlens_q", ctypes.c_void_p), struct._fields_)

    def test_header_pins_the_same_abi(self):
        header = HEADER.read_text(encoding="utf8")
        self.assertIn("sizeof(R4DInt4TiledArgs) == 216", header)
        self.assertIn("offsetof(R4DInt4TiledArgs, cu_seqlens_q) == 208", header)
        self.assertIn("offsetof(R4DInt4TiledArgs, softmax_scale) == 200", header)


if __name__ == "__main__":
    unittest.main()
