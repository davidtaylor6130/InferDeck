import asyncio
import logging
import os
import secrets
import tempfile
import time
import uuid
from contextlib import asynccontextmanager
from pathlib import Path

from fastapi import FastAPI, File, Form, Header, Request, Response, UploadFile
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse
from fastapi.exceptions import RequestValidationError

from .audio import AudioPreparer
from .backend import Backend, QwenBackend, language_for_model
from .config import Settings
from .errors import AlignmentError
from .words import aggregate_units_to_words, validate_words


logger = logging.getLogger("inferdeck.forced_aligner")


def create_app(settings: Settings | None = None, backend: Backend | None = None) -> FastAPI:
    config = settings or Settings()
    if config.host.lower() not in {"127.0.0.1", "localhost", "::1"} and not config.auth_token:
        raise RuntimeError("ALIGNER_AUTH_TOKEN is required when ALIGNER_HOST is not loopback")
    hub_cache = Path(os.getenv("HF_HUB_CACHE", str(config.hf_home / "hub")))
    selected_backend = backend or QwenBackend(
        config.model,
        config.revision,
        config.device,
        config.cpu_fallback,
        config.min_gpu_free_mb,
        hub_cache,
        config.min_word_confidence,
    )

    @asynccontextmanager
    async def lifespan(app: FastAPI):
        if not selected_backend.loaded:
            try:
                await asyncio.to_thread(selected_backend.load)
                logger.info("forced aligner loaded model=%s backend=%s", config.model, selected_backend.backend_name)
            except Exception as exc:
                logger.exception("forced aligner startup failed model=%s error=%s", config.model, type(exc).__name__)
        yield

    app = FastAPI(title="InferDeck Forced Aligner", version="1.0.11", lifespan=lifespan)
    app.state.backend = selected_backend
    app.state.inference_lock = asyncio.Semaphore(1)
    app.state.audio_preparer = AudioPreparer(config.max_audio_seconds, config.max_upload_bytes)
    app.add_middleware(
        CORSMiddleware,
        allow_origins=["*"],
        allow_methods=["GET", "POST", "OPTIONS"],
        allow_headers=["Authorization", "Content-Type", "X-Request-ID"],
        expose_headers=["X-Request-ID"],
    )

    @app.exception_handler(AlignmentError)
    async def alignment_error_handler(request: Request, exc: AlignmentError):
        return JSONResponse(status_code=exc.status_code, content={"error": {"code": exc.code, "message": exc.message}})

    @app.exception_handler(RequestValidationError)
    async def validation_error_handler(request: Request, exc: RequestValidationError):
        return JSONResponse(status_code=400, content={"error": {"code": "INVALID_REQUEST", "message": "Invalid multipart request"}})

    @app.exception_handler(Exception)
    async def unexpected_error_handler(request: Request, exc: Exception):
        logger.exception("unexpected alignment service failure request_id=%s", getattr(request.state, "request_id", "unknown"))
        return JSONResponse(status_code=500, content={"error": {"code": "PROCESSING_FAILED", "message": "Unexpected processing failure"}})

    @app.get("/health")
    async def health():
        return {
            "ok": selected_backend.loaded,
            "model": config.model,
            "device": selected_backend.backend_name,
            "modelLoaded": selected_backend.loaded,
        }

    @app.options("/v1/audio/alignments")
    async def alignment_options():
        return Response(status_code=204)

    @app.post("/v1/audio/alignments")
    async def align(
        request: Request,
        file: UploadFile | None = File(default=None),
        audio: UploadFile | None = File(default=None),
        text: str | None = Form(default=None),
        language: str = Form(default="en"),
        model: str | None = Form(default=None),
        authorization: str | None = Header(default=None),
        x_request_id: str | None = Header(default=None),
    ):
        supplied_request_id = x_request_id or ""
        request_id = "".join(character for character in supplied_request_id if character.isalnum() or character in "-_.")[:128]
        if not request_id:
            request_id = str(uuid.uuid4())
        request.state.request_id = request_id
        started = time.perf_counter()
        upload = file or audio
        success = False
        duration = 0.0
        try:
            if config.auth_token:
                expected = f"Bearer {config.auth_token}"
                if authorization is None or not secrets.compare_digest(authorization, expected):
                    raise AlignmentError(401, "UNAUTHORIZED", "A valid Bearer token is required")
            if upload is None:
                raise AlignmentError(400, "MISSING_AUDIO", "Multipart field 'file' or 'audio' is required")
            if text is None or not text.strip():
                raise AlignmentError(400, "EMPTY_TRANSCRIPT", "Multipart field 'text' is required and must not be empty")
            requested_model = model or config.model
            if requested_model != config.model:
                raise AlignmentError(400, "INVALID_MODEL", f"Only {config.model} is available")
            if not selected_backend.loaded:
                raise AlignmentError(503, "MODEL_UNAVAILABLE", "The forced-alignment model is unavailable")
            language_for_model(language)
            with tempfile.TemporaryDirectory(dir=config.temp_root) as workdir_name:
                prepared = await asyncio.to_thread(app.state.audio_preparer.prepare, upload, Path(workdir_name))
                duration = prepared.duration
                async with app.state.inference_lock:
                    units = await asyncio.to_thread(selected_backend.align, prepared.path, text, language)
                words = aggregate_units_to_words(units, text)
                words = validate_words(
                    words,
                    duration,
                    config.min_word_confidence,
                    getattr(selected_backend, "timestamp_grid_seconds", 0.0),
                    request_id,
                    config.development_diagnostics,
                )
                success = True
                response = {
                    "model": config.model,
                    "language": language,
                    "duration": round(duration, 6),
                    "words": [
                        {
                            "word": word.text,
                            "start": float(word.start),
                            "end": float(min(word.end, duration)),
                            "confidence": float(word.confidence),
                        }
                        for word in words
                    ],
                    "alignment": {"status": "aligned", "approximate": False},
                }
                return JSONResponse(response, headers={"X-Request-ID": request_id})
        finally:
            elapsed = time.perf_counter() - started
            logger.info(
                "alignment request_id=%s duration_seconds=%.3f processing_seconds=%.3f backend=%s success=%s",
                request_id, duration, elapsed, selected_backend.backend_name, success,
            )

    return app
