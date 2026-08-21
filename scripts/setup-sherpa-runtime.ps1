param(
    [string]$RuntimeRoot = "runtime",
    [string]$Version = "1.13.2",
    [string]$Sha256 = "e1f9b7e6b17aec5a56ea1180ffd915a42eef86c7abf7a0749ddc530e0e0831e4",
    [string]$HeaderSha256 = "437b1279047877167d8fadc74a60d47f3df514d703fdac1c1b6851da9bc2fdb4"
)

$ErrorActionPreference = "Stop"

$archiveName = "sherpa-onnx-v$Version-win-x64-shared-MD-Release-lib.tar.bz2"
$installName = "sherpa-onnx-v$Version-win-x64-shared-MD-Release-lib"
$installRoot = Join-Path $RuntimeRoot $installName
$required = @(
    (Join-Path $installRoot "include\sherpa-onnx\c-api\c-api.h"),
    (Join-Path $installRoot "lib\sherpa-onnx-c-api.lib"),
    (Join-Path $installRoot "lib\sherpa-onnx-c-api.dll"),
    (Join-Path $installRoot "lib\onnxruntime.dll"),
    (Join-Path $installRoot "lib\onnxruntime_providers_shared.dll")
)

if ($required | Where-Object { !(Test-Path -LiteralPath $_ -PathType Leaf) }) {
    New-Item -ItemType Directory -Path $RuntimeRoot -Force | Out-Null
    $archive = Join-Path ([System.IO.Path]::GetTempPath()) $archiveName
    $url = "https://github.com/k2-fsa/sherpa-onnx/releases/download/v$Version/$archiveName"
    Invoke-WebRequest -Uri $url -OutFile $archive
    $actual = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $Sha256) { throw "sherpa-onnx checksum mismatch: $actual" }
    tar -xf $archive -C $RuntimeRoot
    if ($LASTEXITCODE -ne 0) { throw "Unable to extract $archiveName" }
}

$header = Join-Path $installRoot "include\sherpa-onnx\c-api\c-api.h"
if (!(Test-Path -LiteralPath $header -PathType Leaf)) {
    $headerDownload = Join-Path ([System.IO.Path]::GetTempPath()) "sherpa-onnx-v$Version-c-api.h"
    $headerUrl = "https://raw.githubusercontent.com/k2-fsa/sherpa-onnx/v$Version/sherpa-onnx/c-api/c-api.h"
    Invoke-WebRequest -Uri $headerUrl -OutFile $headerDownload
    $actualHeader = (Get-FileHash -LiteralPath $headerDownload -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHeader -ne $HeaderSha256) { throw "sherpa-onnx header checksum mismatch: $actualHeader" }
    New-Item -ItemType Directory -Path (Split-Path -Parent $header) -Force | Out-Null
    Copy-Item -LiteralPath $headerDownload -Destination $header
}

$missing = @($required | Where-Object { !(Test-Path -LiteralPath $_ -PathType Leaf) })
if ($missing.Count -ne 0) { throw "sherpa-onnx runtime is incomplete: $($missing -join ', ')" }

(Resolve-Path -LiteralPath $installRoot).Path
