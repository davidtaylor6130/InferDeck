param(
    [Parameter(Mandatory = $true)]
    [string]$SourceAudio,
    [double]$StartSeconds = 0,
    [string]$GatewayUrl = 'http://127.0.0.1:11434',
    [string]$AlignerUrl = 'http://192.168.0.168:11436',
    [string]$TokenFile = 'C:\InferDeck\config\forced-aligner-token.txt',
    [string]$Ffmpeg = 'C:\InferDeck\runtime\forced-aligner\.venv\Lib\site-packages\imageio_ffmpeg\binaries\ffmpeg-win-x86_64-v7.1.exe'
)

$ErrorActionPreference = 'Stop'
$source = (Resolve-Path -LiteralPath $SourceAudio).Path
if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Missing real-audio source: $source" }
if (-not (Test-Path -LiteralPath $TokenFile -PathType Leaf)) { throw "Missing token file: $TokenFile" }
if (-not (Test-Path -LiteralPath $Ffmpeg -PathType Leaf)) { throw "Missing FFmpeg: $Ffmpeg" }
$token = (Get-Content -LiteralPath $TokenFile -Raw).Trim()
if ($token.Length -lt 43) { throw 'Aligner Bearer token is invalid' }
$tempBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$workdir = Join-Path $tempBase ("inferdeck-aligner-real-audio-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $workdir | Out-Null

try {
    $results = foreach ($seconds in 4, 8, 30, 73, 300) {
        $clip = Join-Path $workdir "speech-${seconds}s.wav"
        & $Ffmpeg -hide_banner -loglevel error -nostdin -ss $StartSeconds -i $source -t $seconds -vn -ac 1 -ar 16000 -c:a pcm_s16le -y $clip
        if ($LASTEXITCODE -ne 0) { throw "FFmpeg failed for ${seconds}s" }
        $expectedBytes = 44 + (16000 * 2 * $seconds)
        if ((Get-Item -LiteralPath $clip).Length -lt ($expectedBytes - 4096)) { throw "Source is shorter than ${seconds}s after the requested start" }

        $asrRaw = & curl.exe -sS --max-time 360 -X POST -F "file=@$clip;type=audio/wav" -F 'model=parakeet-tdt-0.6b-v3' -F 'language=en' "$GatewayUrl/v1/audio/transcriptions"
        if ($LASTEXITCODE -ne 0) { throw "Parakeet request failed for ${seconds}s" }
        $asr = $asrRaw | ConvertFrom-Json
        if (-not $asr.text) { throw "Parakeet returned no transcript for ${seconds}s" }

        $responseFile = Join-Path $workdir "alignment-${seconds}s.json"
        $status = & curl.exe -sS --max-time 3600 -o $responseFile -w '%{http_code}' -X POST -H "Authorization: Bearer $token" -F "file=@$clip;type=audio/wav" --form-string "text=$($asr.text)" -F 'language=en' "$AlignerUrl/v1/audio/alignments"
        if ($LASTEXITCODE -ne 0) { throw "Alignment request failed for ${seconds}s" }
        $payload = Get-Content -LiteralPath $responseFile -Raw | ConvertFrom-Json
        if ([int]$status -ne 200) { throw "Alignment returned HTTP $status for ${seconds}s: $($payload.error.message)" }
        if (-not $payload.words -or $payload.words.Count -ne ($asr.text -split '\s+').Count) { throw "Word-count mismatch for ${seconds}s" }
        $previousEnd = 0.0
        foreach ($word in $payload.words) {
            if ($word.start -lt $previousEnd -or $word.end -le $word.start -or $word.end -gt $payload.duration) { throw "Invalid timestamp ordering or bounds for ${seconds}s" }
            $previousEnd = $word.end
        }
        [pscustomobject]@{ Seconds = $seconds; Status = [int]$status; Words = $payload.words.Count; Duration = $payload.duration }
    }
    $results
} finally {
    Remove-Variable token -ErrorAction SilentlyContinue
    $resolvedWorkdir = [IO.Path]::GetFullPath($workdir)
    if (-not $resolvedWorkdir.StartsWith($tempBase, [StringComparison]::OrdinalIgnoreCase) -or -not (Split-Path $resolvedWorkdir -Leaf).StartsWith('inferdeck-aligner-real-audio-')) {
        throw "Refusing to remove unexpected temporary path: $resolvedWorkdir"
    }
    if (Test-Path -LiteralPath $resolvedWorkdir) { Remove-Item -LiteralPath $resolvedWorkdir -Recurse -Force }
}
