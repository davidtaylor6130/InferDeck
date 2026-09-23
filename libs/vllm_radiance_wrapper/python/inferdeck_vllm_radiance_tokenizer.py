"""Register one synchronized local fast tokenizer with vLLM."""
from __future__ import annotations
import hashlib
import copy
import threading
import sys
from pathlib import Path

MODE = "inferdeck_hf_pool"
def _make_locked_tokenizer(tokenizer, path, revision, download_dir, kwargs):
    from vllm.tokenizers.hf import ThreadSafeHFTokenizerMixin

    facade = copy.copy(tokenizer)
    guard = threading.RLock()

    class LockedTokenizer(type(tokenizer), ThreadSafeHFTokenizerMixin):
        def apply_chat_template(self, *args, **kwargs):
            with guard:
                return tokenizer.apply_chat_template(*args, **kwargs)

        def batch_decode(self, *args, **kwargs):
            with guard:
                return tokenizer.batch_decode(*args, **kwargs)

        def batch_encode(self, *args, **kwargs):
            with guard:
                return tokenizer.batch_encode(*args, **kwargs)

        def convert_tokens_to_ids(self, *args, **kwargs):
            with guard:
                return tokenizer.convert_tokens_to_ids(*args, **kwargs)

        def convert_ids_to_tokens(self, *args, **kwargs):
            with guard:
                return tokenizer.convert_ids_to_tokens(*args, **kwargs)

        def convert_tokens_to_string(self, *args, **kwargs):
            with guard:
                return tokenizer.convert_tokens_to_string(*args, **kwargs)

        def decode(self, *args, **kwargs):
            with guard:
                return tokenizer.decode(*args, **kwargs)

        def encode(self, *args, **kwargs):
            with guard:
                return tokenizer.encode(*args, **kwargs)

        def __call__(self, *args, **kwargs):
            with guard:
                return tokenizer.__call__(*args, **kwargs)

        def __reduce__(self):
            return _rebuild_pool, (path, revision, download_dir, kwargs)

    facade.__class__ = LockedTokenizer
    return facade


def _rebuild_pool(path, revision, download_dir, kwargs):
    return ImmutablePooledTokenizer.from_pretrained(
        path, revision=revision, download_dir=download_dir, **kwargs)


def artifact_revision(model: str) -> str:
    root = Path(model).resolve(strict=True)
    if not (root / "tokenizer.json").is_file():
        raise RuntimeError("native tokenizer requires a local tokenizer.json")
    digest = hashlib.sha256(b"inferdeck-hf-pool-v2:fast:left:locked1")
    paths = {root / name for name in (
        "tokenizer.json", "tokenizer_config.json", "special_tokens_map.json",
        "added_tokens.json", "config.json", "vocab.json", "merges.txt",
        "chat_template.jinja", "chat_template.json")}
    paths.update(root.glob("chat_templates/*.jinja"))
    for path in sorted(paths):
        digest.update(str(path.relative_to(root)).encode("utf-8"))
        digest.update(b"\0")
        digest.update(path.read_bytes() if path.is_file() else b"<absent>")
        digest.update(b"\0")
    return digest.hexdigest()


def register_tokenizer(model: str) -> str:
    from vllm.tokenizers.registry import TokenizerRegistry
    expected = (__name__, "ImmutablePooledTokenizer")
    existing = TokenizerRegistry.tokenizers.get(MODE)
    if existing is not None and existing != expected:
        raise RuntimeError("native tokenizer mode is already registered by another module")
    if existing is None:
        TokenizerRegistry.register(MODE, *expected)
    from vllm.renderers.registry import RENDERER_REGISTRY
    renderer = ("vllm.renderers.hf", "HfRenderer")
    existing_renderer = RENDERER_REGISTRY.renderers.get(MODE)
    if existing_renderer is not None and existing_renderer != renderer:
        raise RuntimeError("native renderer mode is already registered by another module")
    if existing_renderer is None:
        RENDERER_REGISTRY.register(MODE, *renderer)
    return artifact_revision(model)


class ImmutablePooledTokenizer:
    @classmethod
    def from_pretrained(cls, path_or_repo_id, *args, revision=None,
                        trust_remote_code=False, download_dir=None, **kwargs):
        from vllm.tokenizers.hf import CachedHfTokenizer, ThreadSafeHFTokenizerMixin
        from transformers import TokenizersBackend
        if revision != artifact_revision(str(path_or_repo_id)):
            raise RuntimeError("tokenizer artifacts changed after native runtime admission")
        if args or trust_remote_code or kwargs.get("use_fast", True) is not True:
            raise RuntimeError("native tokenizer requires the pinned local fast tokenizer")
        if kwargs.get("truncation_side", "left") != "left":
            raise RuntimeError("native tokenizer requires left truncation semantics")
        kwargs.update(local_files_only=True, use_fast=True, truncation_side="left")
        for attempt in range(2):
            try:
                tokenizer = CachedHfTokenizer.from_pretrained(
                    path_or_repo_id, trust_remote_code=False, download_dir=download_dir, **kwargs)
                pooled = _make_locked_tokenizer(
                    tokenizer, path_or_repo_id, revision, download_dir, kwargs)
                break
            except Exception as error:
                if attempt or not any(marker in str(error) for marker in (
                    "Error while attempting to unpickle Tokenizer",
                    "Error while initializing BPE: Token")):
                    raise
                if artifact_revision(str(path_or_repo_id)) != revision:
                    raise RuntimeError("tokenizer artifacts changed during native runtime load") from error
                print("event=tokenizer_load_retry attempt=1", file=sys.stderr, flush=True)
        if not isinstance(pooled, (TokenizersBackend,)) or not isinstance(pooled, ThreadSafeHFTokenizerMixin):
            raise RuntimeError("native tokenizer did not construct a thread-safe fast tokenizer")

        class ImmutablePool(type(pooled)):
            def __copy__(self):
                return self

            def add_tokens(self, *args, **kwargs):
                raise RuntimeError("native tokenizer vocabulary is immutable")

            def add_special_tokens(self, *args, **kwargs):
                raise RuntimeError("native tokenizer vocabulary is immutable")

            def __setattr__(self, name, value):
                if name in {"chat_template", "_tokenizer", "truncation_side", "padding_side"}:
                    raise RuntimeError("native tokenizer configuration is immutable")
                super().__setattr__(name, value)

        pooled.__class__ = ImmutablePool
        return pooled
