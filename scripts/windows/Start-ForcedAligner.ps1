param(
    [string]$RuntimeRoot = 'C:\InferDeck\runtime\forced-aligner',
    [string]$ModelCache = 'C:\InferDeck\models\cache'
)

$ErrorActionPreference = 'Stop'
$executable = Join-Path $RuntimeRoot '.venv\Scripts\inferdeck-forced-aligner.exe'
$logs = 'C:\InferDeck\logs'
$lock = Join-Path $RuntimeRoot 'aligner.pid'
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) { throw "Missing aligner executable: $executable" }
if (-not (Test-Path -LiteralPath $ModelCache -PathType Container)) { throw "Missing model cache: $ModelCache" }
try {
    $health = Invoke-RestMethod -Uri 'http://127.0.0.1:11436/health' -TimeoutSec 3
    if ($health.modelLoaded) { exit 0 }
} catch {
}
if (Test-Path -LiteralPath $lock) {
    $existing = Get-Content -LiteralPath $lock -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($existing -and (Get-Process -Id ([int]$existing) -ErrorAction SilentlyContinue)) { exit 0 }
}
New-Item -ItemType Directory -Path $logs -Force | Out-Null
$env:ALIGNER_MODEL = 'Qwen/Qwen3-ForcedAligner-0.6B'
$env:ALIGNER_MODEL_REVISION = 'c7cbfc2048c462b0d63a45797104fc9db3ad62b7'
$env:ALIGNER_DEVICE = 'rocm'
$env:ALIGNER_CPU_FALLBACK = 'true'
$env:ALIGNER_MAX_AUDIO_SECONDS = '300'
$env:ALIGNER_MIN_GPU_FREE_MB = '3072'
$env:ALIGNER_AUTH_TOKEN = ''
$env:ALIGNER_HOST = '127.0.0.1'
$env:ALIGNER_PORT = '11436'
$env:HF_HOME = $ModelCache
$env:HF_HUB_CACHE = $ModelCache
$env:HF_HUB_OFFLINE = '1'
$env:TRANSFORMERS_OFFLINE = '1'
$process = Start-Process -FilePath $executable -WorkingDirectory $RuntimeRoot -WindowStyle Hidden -RedirectStandardOutput (Join-Path $logs 'forced-aligner.log') -RedirectStandardError (Join-Path $logs 'forced-aligner-error.log') -PassThru
Set-Content -LiteralPath $lock -Value $process.Id -Encoding ASCII
$deadline = [DateTime]::UtcNow.AddMinutes(5)
do {
    Start-Sleep -Seconds 2
    try {
        $health = Invoke-RestMethod -Uri 'http://127.0.0.1:11436/health' -TimeoutSec 5
        if ($health.modelLoaded) { $health | ConvertTo-Json; exit 0 }
    } catch {
    }
} while ([DateTime]::UtcNow -lt $deadline)
throw 'Forced aligner did not become healthy within five minutes'
