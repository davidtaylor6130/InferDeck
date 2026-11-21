import gc
import importlib.util
import sys
import types
import unittest
import weakref
from pathlib import Path
from unittest.mock import patch


LIFECYCLE_PATH = Path(__file__).resolve().parents[1] / "python" / "inferdeck_vllm_radiance_lifecycle.py"
SPEC = importlib.util.spec_from_file_location("inferdeck_lifecycle_test", LIFECYCLE_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError("cannot load lifecycle helper")
LIFECYCLE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(LIFECYCLE)


class Model:
    pass


class Engine:
    pass


class EngineFinalizerTests(unittest.TestCase):
    def test_target_and_draft_hooks_release_only_retired_model_weights(self):
        hooks = {}
        class CompiledModel:
            def __init__(self):
                hooks[id(self)] = self.callback
            def callback(self):
                pass
            def modules(self):
                return [self]
            def cleanup(self):
                hooks.pop(id(self), None)
        target, draft, unrelated = CompiledModel(), CompiledModel(), CompiledModel()
        draft_ref = weakref.ref(draft)
        runner = types.SimpleNamespace(model=target, drafter=types.SimpleNamespace(model=draft))
        engine = Engine()
        engine.engine_core = types.SimpleNamespace(engine_core=types.SimpleNamespace(
            model_executor=types.SimpleNamespace(driver_worker=types.SimpleNamespace(model_runner=runner))))
        with patch.dict(sys.modules, {"vllm.compilation.wrapper": types.SimpleNamespace(TorchCompileWithNoGuardsWrapper=CompiledModel)}):
            self.assertEqual(LIFECYCLE.release_model_compilation_hooks(engine), 2)
            self.assertEqual(list(hooks), [id(unrelated)])
        runner.model = None
        runner.drafter.model = None
        del draft
        gc.collect()
        self.assertIsNone(draft_ref())

    def test_explicit_finalizer_releases_model_and_is_idempotent(self):
        released_ids = []
        engine = Engine()
        model = Model()
        model_ref = weakref.ref(model)
        engine._finalizer = weakref.finalize(engine, lambda value: released_ids.append(id(value)), model)
        del model
        gc.collect()
        self.assertIsNotNone(model_ref())

        self.assertTrue(LIFECYCLE.finalize_engine_caches(engine))
        gc.collect()
        self.assertIsNone(model_ref())
        self.assertEqual(len(released_ids), 1)
        self.assertFalse(LIFECYCLE.finalize_engine_caches(engine))
        self.assertEqual(len(released_ids), 1)

    def test_unrelated_finalizer_is_untouched(self):
        engine = Engine()
        engine._finalizer = weakref.finalize(engine, lambda: None)
        unrelated = Engine()
        unrelated_finalizer = weakref.finalize(unrelated, lambda: None)

        self.assertTrue(LIFECYCLE.finalize_engine_caches(engine))
        self.assertTrue(unrelated_finalizer.alive)

    def test_kernel_scratch_is_released_and_rearmed(self):
        class Tensor:
            def __init__(self, count, size):
                self.count, self.size = count, size
            def numel(self):
                return self.count
            def element_size(self):
                return self.size
        calls = []
        module = types.ModuleType("radiance_mxfp4")
        module._decode_scratch = [Tensor(8, 4), Tensor(2, 4)]
        module._decode_scratch_ready = [True]
        module._ext = types.SimpleNamespace(set_decode_scratch=lambda *args: calls.append(args))
        with patch.dict(sys.modules, {"radiance_mxfp4": module}):
            self.assertEqual(LIFECYCLE.release_kernel_scratch(), 40)
            self.assertEqual(module._decode_scratch, [None, None])
            self.assertFalse(module._decode_scratch_ready[0])
            self.assertEqual(calls, [(0, 0, 0)])
            self.assertEqual(LIFECYCLE.release_kernel_scratch(), 0)
            self.assertEqual(len(calls), 1)
        self.assertEqual(LIFECYCLE.release_kernel_scratch(), 0)

    def test_failed_load_frames_release_partial_model(self):
        def load():
            partial = Model()
            load.ref = weakref.ref(partial)
            raise OSError("paging file too small")
        def wrapped():
            try:
                load()
            except OSError as error:
                raise RuntimeError("load failed") from error
        try:
            wrapped()
        except RuntimeError as error:
            self.assertIsNotNone(load.ref())
            self.assertEqual(LIFECYCLE.release_exception_frames(error), 2)
            gc.collect()
            self.assertIsNone(load.ref())
        self.assertEqual(LIFECYCLE.release_exception_frames(None), 0)

    def test_engine_without_finalizer_is_a_noop(self):
        self.assertFalse(LIFECYCLE.finalize_engine_caches(Engine()))


if __name__ == "__main__":
    unittest.main()
