[CmdletBinding()]
param(
    [string]$BaseUrl = 'http://127.0.0.1:11434',
    [Parameter(Mandatory = $true)]
    [string]$Model,
    [string]$Prompt = 'Warm analog synths, steady electronic drums, no vocals',
    [string]$Lyrics = '',
    [ValidateRange(10.0, 600.0)]
    [double]$DurationSeconds = 10.0,
    [long]$Seed = 12345,
    [ValidateRange(0, 100)]
    [int]$Steps = 0,
    [ValidateRange(0.0, 50.0)]
    [double]$GuidanceScale = 0.0,
    [string]$Output = 'audio-generation-validation.wav',
    [string]$ApiKey = '',
    [ValidateRange(1, 7200)]
    [int]$TimeoutSeconds = 1800,
    [ValidateRange(0.01, 10.0)]
    [double]$DurationToleranceSeconds = 1.0
)

$ErrorActionPreference = 'Stop'
[System.Net.ServicePointManager]::Expect100Continue = $false

if ([string]::IsNullOrWhiteSpace($Model) -or
    [string]::IsNullOrWhiteSpace($Prompt)) {
    throw 'Model and prompt are required'
}
if ($Seed -lt -1 -or $Seed -gt [uint32]::MaxValue) {
    throw "Seed must be -1 or between 0 and $([uint32]::MaxValue)"
}

Add-Type -AssemblyName System.Net.Http

$payload = @{
    model = $Model
    prompt = $Prompt
    lyrics = $Lyrics
    duration = $DurationSeconds
    seed = $Seed
    steps = $Steps
    guidance_scale = $GuidanceScale
} | ConvertTo-Json -Compress

$handler = New-Object System.Net.Http.HttpClientHandler
$client = New-Object System.Net.Http.HttpClient($handler)
$client.Timeout = [TimeSpan]::FromSeconds($TimeoutSeconds)
$message = New-Object System.Net.Http.HttpRequestMessage(
    [System.Net.Http.HttpMethod]::Post,
    ($BaseUrl.TrimEnd('/') + '/api/inferdeck/v1/audio/generations'))
$message.Content = New-Object System.Net.Http.StringContent(
    $payload,
    [Text.Encoding]::UTF8,
    'application/json')
if (![string]::IsNullOrWhiteSpace($ApiKey)) {
    $message.Headers.Authorization =
        New-Object System.Net.Http.Headers.AuthenticationHeaderValue(
            'Bearer', $ApiKey)
}

$response = $null
$watch = [System.Diagnostics.Stopwatch]::StartNew()
try {
    $response = $client.SendAsync($message).GetAwaiter().GetResult()
    $bytes = $response.Content.ReadAsByteArrayAsync().GetAwaiter().GetResult()
    $statusCode = [int]$response.StatusCode
    $contentType = $response.Content.Headers.ContentType.MediaType

    $headerValues = [System.Collections.Generic.IEnumerable[string]]$null
    $jobId = ''
    if ($response.Headers.TryGetValues(
            'X-InferDeck-Job-Id', [ref]$headerValues)) {
        $jobId = [string]::Join(',', $headerValues)
    }
    $headerValues = [System.Collections.Generic.IEnumerable[string]]$null
    $resolvedSeedText = ''
    if ($response.Headers.TryGetValues(
            'X-InferDeck-Seed', [ref]$headerValues)) {
        $resolvedSeedText = [string]::Join(',', $headerValues)
    }
    $headerValues = [System.Collections.Generic.IEnumerable[string]]$null
    $headerDurationText = ''
    if ($response.Headers.TryGetValues(
            'X-InferDeck-Audio-Duration-Seconds', [ref]$headerValues)) {
        $headerDurationText = [string]::Join(',', $headerValues)
    }
} finally {
    $watch.Stop()
    if ($null -ne $response) {
        $response.Dispose()
    }
    $message.Dispose()
    $client.Dispose()
    $handler.Dispose()
}

if ($statusCode -ne 200) {
    $errorBody = [Text.Encoding]::UTF8.GetString($bytes)
    throw "Unexpected audio response status ${statusCode}: $errorBody"
}
if ($contentType -ne 'audio/wav') {
    throw "Unexpected audio content type: $contentType"
}
if ($bytes.Length -lt 44 -or
    [Text.Encoding]::ASCII.GetString($bytes, 0, 4) -ne 'RIFF' -or
    [Text.Encoding]::ASCII.GetString($bytes, 8, 4) -ne 'WAVE') {
    throw 'Returned audio is not a RIFF/WAVE file'
}
$riffSize = [BitConverter]::ToUInt32($bytes, 4)
if ($riffSize -ne $bytes.Length - 8) {
    throw "RIFF size does not match the response length: $riffSize"
}

$format = 0
$channels = 0
$sampleRate = 0
$byteRate = 0
$blockAlign = 0
$bitsPerSample = 0
$dataSize = 0
$position = 12
while ($position + 8 -le $bytes.Length) {
    $chunkId = [Text.Encoding]::ASCII.GetString($bytes, $position, 4)
    $chunkSize = [BitConverter]::ToUInt32($bytes, $position + 4)
    $chunkEnd = [long]$position + 8 + $chunkSize
    if ($chunkEnd -gt $bytes.Length) {
        throw "WAVE chunk exceeds the response length: $chunkId"
    }
    if ($chunkId -eq 'fmt ' -and $chunkSize -ge 16) {
        $format = [BitConverter]::ToUInt16($bytes, $position + 8)
        $channels = [BitConverter]::ToUInt16($bytes, $position + 10)
        $sampleRate = [BitConverter]::ToUInt32($bytes, $position + 12)
        $byteRate = [BitConverter]::ToUInt32($bytes, $position + 16)
        $blockAlign = [BitConverter]::ToUInt16($bytes, $position + 20)
        $bitsPerSample = [BitConverter]::ToUInt16($bytes, $position + 22)
    } elseif ($chunkId -eq 'data') {
        $dataSize = $chunkSize
    }
    $position = [int]($chunkEnd + ($chunkSize % 2))
}

if ($format -ne 1 -or $channels -ne 2 -or $sampleRate -ne 48000 -or
    $bitsPerSample -ne 16 -or $blockAlign -ne 4 -or
    $byteRate -ne 192000 -or $dataSize -eq 0 -or
    $dataSize % $blockAlign -ne 0) {
    throw 'WAVE must contain non-empty 48 kHz stereo PCM16 audio'
}

$actualDurationSeconds =
    [double]$dataSize / ([double]$sampleRate * [double]$blockAlign)
if ([math]::Abs($actualDurationSeconds - $DurationSeconds) -gt
    $DurationToleranceSeconds) {
    throw "Encoded duration $actualDurationSeconds differs from requested duration $DurationSeconds"
}

$parsedSeed = [long]0
if ([string]::IsNullOrWhiteSpace($resolvedSeedText) -or
    ![long]::TryParse($resolvedSeedText, [ref]$parsedSeed) -or
    $parsedSeed -lt 0) {
    throw "Invalid resolved seed header: $resolvedSeedText"
}
if ($Seed -ge 0 -and $parsedSeed -ne $Seed) {
    throw "Resolved seed $parsedSeed differs from requested seed $Seed"
}
$parsedJobId = [uint64]0
if ([string]::IsNullOrWhiteSpace($jobId) -or
    ![uint64]::TryParse($jobId, [ref]$parsedJobId) -or
    $parsedJobId -eq 0) {
    throw "Invalid media job header: $jobId"
}
$headerDuration = [double]0
if ([string]::IsNullOrWhiteSpace($headerDurationText) -or
    ![double]::TryParse(
        $headerDurationText,
        [Globalization.NumberStyles]::Float,
        [Globalization.CultureInfo]::InvariantCulture,
        [ref]$headerDuration) -or
    [math]::Abs($headerDuration - $actualDurationSeconds) -gt 0.01) {
    throw "Invalid audio duration header: $headerDurationText"
}

$outputPath = [IO.Path]::GetFullPath($Output)
$outputDirectory = [IO.Path]::GetDirectoryName($outputPath)
[IO.Directory]::CreateDirectory($outputDirectory) | Out-Null
[IO.File]::WriteAllBytes($outputPath, $bytes)
$hash = (Get-FileHash -LiteralPath $outputPath -Algorithm SHA256).Hash.ToLowerInvariant()

[pscustomobject]@{
    Status = 'passed'
    Model = $Model
    RequestSeconds = [math]::Round($watch.Elapsed.TotalSeconds, 2)
    RequestedAudioSeconds = $DurationSeconds
    ActualAudioSeconds = [math]::Round($actualDurationSeconds, 3)
    SampleRate = $sampleRate
    Channels = $channels
    BitsPerSample = $bitsPerSample
    ResolvedSeed = $parsedSeed
    JobId = $parsedJobId
    Bytes = $bytes.Length
    Sha256 = $hash
    Output = $outputPath
}
