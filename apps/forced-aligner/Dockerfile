FROM rocm/pytorch:rocm7.2.1_ubuntu24.04_py3.12_pytorch_release_2.9.1

WORKDIR /app
COPY pyproject.toml /app/pyproject.toml
COPY src /app/src
RUN python -m pip install --no-cache-dir .

ENV ALIGNER_MODEL=Qwen/Qwen3-ForcedAligner-0.6B \
    ALIGNER_MODEL_REVISION=c7cbfc2048c462b0d63a45797104fc9db3ad62b7 \
    ALIGNER_DEVICE=rocm \
    ALIGNER_CPU_FALLBACK=true \
    ALIGNER_MAX_AUDIO_SECONDS=300 \
    ALIGNER_AUTH_TOKEN= \
    HF_HOME=/models/cache \
    HF_HUB_CACHE=/models/cache/hub \
    HF_HUB_OFFLINE=1 \
    TRANSFORMERS_OFFLINE=1 \
    ALIGNER_HOST=0.0.0.0 \
    ALIGNER_PORT=11436

EXPOSE 11436
HEALTHCHECK --interval=30s --timeout=5s --start-period=180s --retries=3 CMD python -c "import json,urllib.request; d=json.load(urllib.request.urlopen('http://127.0.0.1:11436/health',timeout=3)); raise SystemExit(0 if d.get('modelLoaded') else 1)"
CMD ["inferdeck-forced-aligner"]
