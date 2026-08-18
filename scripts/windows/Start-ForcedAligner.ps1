param(
    [string]$RuntimeRoot = 'C:\InferDeck\runtime\forced-aligner',
    [string]$ModelCache = 'C:\InferDeck\models\cache',
    [string]$ListenAddress = '192.168.0.168',
    [string]$TokenFile = 'C:\InferDeck\config\forced-aligner-token.txt'
)

$ErrorActionPreference = 'Stop'
$executable = Join-Path $RuntimeRoot '.venv\Scripts\inferdeck-forced-aligner.exe'
$logs = 'C:\InferDeck\logs'
$lock = Join-Path $RuntimeRoot 'aligner.pid'
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) { throw "Missing aligner executable: $executable" }
if (-not (Test-Path -LiteralPath $ModelCache -PathType Container)) { throw "Missing model cache: $ModelCache" }
if (-not (Test-Path -LiteralPath $TokenFile -PathType Leaf)) { throw "Missing aligner token file: $TokenFile" }
$token = (Get-Content -LiteralPath $TokenFile -Raw).Trim()
if ($token.Length -lt 43) { throw 'Aligner Bearer token must contain at least 256 bits of encoded entropy' }
Remove-Variable token
$healthUrl = "http://${ListenAddress}:11436/health"
try {
    $health = Invoke-RestMethod -Uri $healthUrl -TimeoutSec 3
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
$env:ALIGNER_AUTH_TOKEN_FILE = $TokenFile
$env:ALIGNER_HOST = $ListenAddress
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
        $health = Invoke-RestMethod -Uri $healthUrl -TimeoutSec 5
        if ($health.modelLoaded) { $health | ConvertTo-Json; exit 0 }
    } catch {
    }
} while ([DateTime]::UtcNow -lt $deadline)
throw 'Forced aligner did not become healthy within five minutes'
