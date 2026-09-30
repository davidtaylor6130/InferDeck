[CmdletBinding()]
param(
    [string]$RouteTests = 'build/bin/Release/route_tests.exe',
    [string]$ModelPath = '',
    [string]$Output = '',
    [switch]$PrepareOnly,
    [ValidatePattern('^[0-9a-fA-F]{64}$')]
    [string]$ExpectedModelSha256 =
        '61b50d457809a5194818fd22e6724b456cd7bb9a6264c52c8110684c53f3704a'
)

$ErrorActionPreference = 'Stop'
[System.Net.ServicePointManager]::Expect100Continue = $false

$testsPath = [IO.Path]::GetFullPath($RouteTests)
if (![IO.File]::Exists($testsPath)) {
    throw "Route test executable not found: $testsPath"
}

$cacheDirectory = Join-Path ([IO.Path]::GetTempPath()) 'inferdeck-post-training-validation'
[IO.Directory]::CreateDirectory($cacheDirectory) | Out-Null
if ([string]::IsNullOrWhiteSpace($ModelPath)) {
    $ModelPath = Join-Path $cacheDirectory 'stories15M-f32-61b50d457809.gguf'
    if (![IO.File]::Exists($ModelPath)) {
        $partial = $ModelPath + '.' + [Guid]::NewGuid().ToString('N') + '.partial'
        try {
            Invoke-WebRequest `
                -UseBasicParsing `
                -Uri 'https://huggingface.co/ggml-org/models-moved/resolve/688f41ddf8f3622d08c6954a061ff12999b6c8d8/tinyllamas/stories15M.gguf' `
                -OutFile $partial
            $downloadHash = (Get-FileHash -LiteralPath $partial -Algorithm SHA256).Hash
            if ($downloadHash -ne $ExpectedModelSha256) {
                throw "Downloaded model SHA-256 mismatch: $downloadHash"
            }
            [IO.File]::Move($partial, $ModelPath)
        } finally {
            if ([IO.File]::Exists($partial)) {
                [IO.File]::Delete($partial)
            }
        }
    }
}
$sourcePath = [IO.Path]::GetFullPath($ModelPath)
if (![IO.File]::Exists($sourcePath)) {
    throw "F32 source model not found: $sourcePath"
}
$sourceHash = (Get-FileHash -LiteralPath $sourcePath -Algorithm SHA256).Hash
if ($sourceHash -ne $ExpectedModelSha256) {
    throw "Source model SHA-256 mismatch: $sourceHash"
}
if ($PrepareOnly) {
    [pscustomobject]@{
        Status = 'prepared'
        Source = $sourcePath
        SourceBytes = ([IO.FileInfo]$sourcePath).Length
        SourceSha256 = $sourceHash.ToLowerInvariant()
    }
    return
}

if ([string]::IsNullOrWhiteSpace($Output)) {
    $Output = Join-Path $cacheDirectory (
        'stories15M-q8_0-' + $PID + '-' + [DateTime]::UtcNow.Ticks + '.gguf')
}
$outputPath = [IO.Path]::GetFullPath($Output)
if ([IO.File]::Exists($outputPath)) {
    throw "Output already exists: $outputPath"
}
[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($outputPath)) | Out-Null

$hadSource = Test-Path Env:INFERDECK_QUANTIZATION_SOURCE
$hadOutput = Test-Path Env:INFERDECK_QUANTIZATION_OUTPUT
$oldSource = $env:INFERDECK_QUANTIZATION_SOURCE
$oldOutput = $env:INFERDECK_QUANTIZATION_OUTPUT
$watch = [Diagnostics.Stopwatch]::StartNew()
$testPassed = $false
try {
    $env:INFERDECK_QUANTIZATION_SOURCE = $sourcePath
    $env:INFERDECK_QUANTIZATION_OUTPUT = $outputPath
    & $testsPath '[native-quantizer]' '--reporter' 'compact'
    if ($LASTEXITCODE -ne 0) {
        throw "Native quantizer test failed with exit code $LASTEXITCODE"
    }
    $testPassed = $true
} finally {
    $watch.Stop()
    if ($hadSource) {
        $env:INFERDECK_QUANTIZATION_SOURCE = $oldSource
    } else {
        Remove-Item Env:INFERDECK_QUANTIZATION_SOURCE -ErrorAction SilentlyContinue
    }
    if ($hadOutput) {
        $env:INFERDECK_QUANTIZATION_OUTPUT = $oldOutput
    } else {
        Remove-Item Env:INFERDECK_QUANTIZATION_OUTPUT -ErrorAction SilentlyContinue
    }
    if (!$testPassed -and [IO.File]::Exists($outputPath)) {
        [IO.File]::Delete($outputPath)
    }
}

if (![IO.File]::Exists($outputPath)) {
    throw 'Native quantizer did not produce an output file'
}
$stream = [IO.File]::OpenRead($outputPath)
try {
    $signature = New-Object byte[] 4
    if ($stream.Read($signature, 0, 4) -ne 4 -or
        [Text.Encoding]::ASCII.GetString($signature) -ne 'GGUF') {
        throw 'Quantized output is not a GGUF file'
    }
} finally {
    $stream.Dispose()
}
$outputHash = (Get-FileHash -LiteralPath $outputPath -Algorithm SHA256).Hash.ToLowerInvariant()
$outputBytes = ([IO.FileInfo]$outputPath).Length

[pscustomobject]@{
    Status = 'passed'
    Source = $sourcePath
    SourceSha256 = $sourceHash.ToLowerInvariant()
    Quantization = 'Q8_0'
    DurationSeconds = [math]::Round($watch.Elapsed.TotalSeconds, 3)
    OutputBytes = $outputBytes
    OutputSha256 = $outputHash
    Output = $outputPath
    Compute = 'CPU'
}
