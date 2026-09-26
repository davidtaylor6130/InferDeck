import gc
import importlib.util
import unittest
import weakref
from pathlib import Path


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

    def test_engine_without_finalizer_is_a_noop(self):
        self.assertFalse(LIFECYCLE.finalize_engine_caches(Engine()))


if __name__ == "__main__":
    unittest.main()