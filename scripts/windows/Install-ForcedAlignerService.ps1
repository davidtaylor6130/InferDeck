param(
    [string]$RuntimeRoot = 'C:\InferDeck\runtime\forced-aligner',
    [string]$ModelCache = 'C:\InferDeck\models\cache',
    [string]$NssmPath = 'C:\InferDeck\nssm.exe',
    [string]$ServiceName = 'InferDeckForcedAligner',
    [string]$ListenAddress = '192.168.0.168',
    [string]$TokenFile = 'C:\InferDeck\config\forced-aligner-token.txt'
)

$ErrorActionPreference = 'Stop'
$executable = Join-Path $RuntimeRoot '.venv\Scripts\inferdeck-forced-aligner.exe'
$logDirectory = 'C:\InferDeck\logs'
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) { throw "Missing aligner executable: $executable" }
if (-not (Test-Path -LiteralPath $ModelCache -PathType Container)) { throw "Missing model cache: $ModelCache" }
if (-not (Test-Path -LiteralPath $NssmPath -PathType Leaf)) { throw "Missing NSSM executable: $NssmPath" }
if (-not (Test-Path -LiteralPath $TokenFile -PathType Leaf)) { throw "Missing aligner token file: $TokenFile" }
$token = (Get-Content -LiteralPath $TokenFile -Raw).Trim()
if ($token.Length -lt 43) { throw 'Aligner Bearer token must contain at least 256 bits of encoded entropy' }
Remove-Variable token
New-Item -ItemType Directory -Path $logDirectory -Force | Out-Null

$existing = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
if ($existing) {
    if ($existing.Status -ne 'Stopped') { Stop-Service -Name $ServiceName -Force }
} else {
    & $NssmPath install $ServiceName $executable
}
& $NssmPath set $ServiceName DisplayName 'InferDeck Qwen Forced Aligner'
& $NssmPath set $ServiceName Description 'Local Qwen3 word-level forced-alignment API'
& $NssmPath set $ServiceName AppDirectory $RuntimeRoot
& $NssmPath set $ServiceName AppEnvironmentExtra `
    'ALIGNER_MODEL=Qwen/Qwen3-ForcedAligner-0.6B' `
    'ALIGNER_MODEL_REVISION=c7cbfc2048c462b0d63a45797104fc9db3ad62b7' `
    'ALIGNER_DEVICE=rocm' `
    'ALIGNER_CPU_FALLBACK=true' `
    'ALIGNER_MAX_AUDIO_SECONDS=300' `
    'ALIGNER_MIN_GPU_FREE_MB=3072' `
    'ALIGNER_AUTH_TOKEN=' `
    "ALIGNER_AUTH_TOKEN_FILE=$TokenFile" `
    "HF_HOME=$ModelCache" `
    "HF_HUB_CACHE=$ModelCache" `
    'HF_HUB_OFFLINE=1' `
    'TRANSFORMERS_OFFLINE=1' `
    "ALIGNER_HOST=$ListenAddress" `
    'ALIGNER_PORT=11436'
& $NssmPath set $ServiceName AppStdout (Join-Path $logDirectory 'forced-aligner.log')
& $NssmPath set $ServiceName AppStderr (Join-Path $logDirectory 'forced-aligner-error.log')
& $NssmPath set $ServiceName AppRotateFiles 1
& $NssmPath set $ServiceName AppRotateBytes 10485760
& $NssmPath set $ServiceName AppExit Default Restart
& $NssmPath set $ServiceName Start SERVICE_DELAYED_AUTO_START
Start-Service -Name $ServiceName

$deadline = [DateTime]::UtcNow.AddMinutes(5)
do {
    Start-Sleep -Seconds 2
    try {
        $health = Invoke-RestMethod -Uri "http://${ListenAddress}:11436/health" -TimeoutSec 5
        if ($health.modelLoaded) { $health | ConvertTo-Json; exit 0 }
    } catch {
    }
} while ([DateTime]::UtcNow -lt $deadline)
throw 'Forced aligner did not become healthy within five minutes'
