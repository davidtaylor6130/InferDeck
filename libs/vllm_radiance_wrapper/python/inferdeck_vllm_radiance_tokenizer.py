"""Reuse immutable tokenizer pools through vLLM's official tokenizer registry."""
from __future__ import annotations
import hashlib
from pathlib import Path

MODE = "inferdeck_hf_pool"
COPIES = 2


def artifact_revision(model: str) -> str:
    root = Path(model).resolve(strict=True)
    if not (root / "tokenizer.json").is_file():
        raise RuntimeError("native tokenizer requires a local tokenizer.json")
    digest = hashlib.sha256(b"inferdeck-hf-pool-v1:fast:left:copies2")
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
        from vllm.tokenizers.hf import CachedHfTokenizer, ThreadSafeHFTokenizerMixin, maybe_make_thread_pool
        from transformers import TokenizersBackend
        if revision != artifact_revision(str(path_or_repo_id)):
            raise RuntimeError("tokenizer artifacts changed after native runtime admission")
        if args or trust_remote_code or kwargs.get("use_fast", True) is not True:
            raise RuntimeError("native tokenizer requires the pinned local fast tokenizer")
        if kwargs.get("truncation_side", "left") != "left":
            raise RuntimeError("native tokenizer requires left truncation semantics")
        kwargs.update(local_files_only=True, use_fast=True, truncation_side="left")
        tokenizer = CachedHfTokenizer.from_pretrained(
            path_or_repo_id, trust_remote_code=False, download_dir=download_dir, **kwargs)
        pooled = maybe_make_thread_pool(tokenizer, COPIES)
        if not isinstance(pooled, (TokenizersBackend,)) or not isinstance(pooled, ThreadSafeHFTokenizerMixin):
            raise RuntimeError("native tokenizer did not construct a thread-safe fast pool")

        class ImmutablePool(type(pooled)):
            def __copy__(self):
                return self

            def add_tokens(self, *args, **kwargs):
                raise RuntimeError("native pooled tokenizer vocabulary is immutable")

            def add_special_tokens(self, *args, **kwargs):
                raise RuntimeError("native pooled tokenizer vocabulary is immutable")

            def __setattr__(self, name, value):
                if name in {"chat_template", "_tokenizer", "truncation_side", "padding_side"}:
                    raise RuntimeError("native pooled tokenizer configuration is immutable")
                super().__setattr__(name, value)

        pooled.__class__ = ImmutablePool
        return pooled
