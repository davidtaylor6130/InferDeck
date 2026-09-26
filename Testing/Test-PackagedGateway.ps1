param(
    [Parameter(Mandatory = $true)]
    [string]$Gateway,
    [int]$Port = 11446,
    [int]$StartupTimeoutSeconds = 30
)

$ErrorActionPreference = 'Stop'
function Get-StreamHash([IO.Stream]$Stream) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $Stream.Position = 0; return (( $sha.ComputeHash($Stream) | ForEach-Object { $_.ToString('x2') }) -join '') }
    finally { $sha.Dispose() }
}
function Get-FileHashHex([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    try { return Get-StreamHash $stream } finally { $stream.Dispose() }
}

if ($Port -lt 1024 -or $Port -gt 65535) { throw "Invalid isolated port: $Port" }
if (Get-NetTCPConnection -LocalPort $Port -ErrorAction SilentlyContinue) { throw "Port $Port is already occupied" }
$gatewayPath = (Resolve-Path -LiteralPath $Gateway).Path
$packageRoot = Split-Path -Parent $gatewayPath
$staticRoot = Join-Path $packageRoot 'static'
$indexPath = Join-Path $staticRoot 'index.html'
if (!(Test-Path -LiteralPath $staticRoot -PathType Container)) { throw "Package static directory is missing: $staticRoot" }
if (!(Test-Path -LiteralPath $indexPath -PathType Leaf)) { throw "Package dashboard index is missing: $indexPath" }
$indexBytes = [IO.File]::ReadAllBytes($indexPath)
$assetName = [regex]::Match([Text.Encoding]::UTF8.GetString($indexBytes), 'assets/(index-[^"'' ]+\.js)').Groups[1].Value
if (!$assetName) { throw 'Package dashboard index does not reference a hashed JavaScript asset' }
$assetPath = Join-Path (Join-Path $staticRoot 'assets') $assetName
if (!(Test-Path -LiteralPath $assetPath -PathType Leaf)) { throw "Referenced dashboard asset is missing: $assetPath" }
$tempBase = (Resolve-Path -LiteralPath ([IO.Path]::GetTempPath())).Path.TrimEnd('\') + '\'
$tempRoot = Join-Path ([IO.Path]::GetTempPath()) ('inferdeck packaged smoke ' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tempRoot | Out-Null
$tempResolved = (Resolve-Path -LiteralPath $tempRoot).Path
if (!$tempResolved.StartsWith($tempBase, [StringComparison]::OrdinalIgnoreCase)) { throw "Refusing cleanup outside temporary root: $tempResolved" }
$configPath = Join-Path $tempResolved 'gateway.yml'
$logPath = Join-Path $tempResolved 'gateway.log'
$statePath = Join-Path $tempResolved 'state.json'
@"
schema_version: 1
server:
  host: "127.0.0.1"
  port: $Port
logging:
  level: "info"
  file: "$($logPath -replace '\\','/')"
auth:
  required: false
  token: ""
cors:
  origins:
    - "*"
state:
  file: "$($statePath -replace '\\','/')"
default_model: ""
observability:
  stats_db: ""
  adlx_helper: ""
  telemetry_poll_ms: 1000
gateway:
  auto_swap: false
model_registry: []
"@ | Set-Content -LiteralPath $configPath -Encoding utf8
$process = $null
try {
    $process = Start-Process -FilePath $gatewayPath -ArgumentList @('-c', ('"{0}"' -f $configPath)) -WorkingDirectory $tempResolved -PassThru -WindowStyle Hidden
    $deadline = [DateTime]::UtcNow.AddSeconds($StartupTimeoutSeconds)
    $health = $null
    while ([DateTime]::UtcNow -lt $deadline) {
        if ($process.HasExited) { throw "Packaged gateway exited during startup: $($process.ExitCode)" }
        $connection = Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($connection -and $connection.OwningProcess -ne $process.Id) { throw "Port $Port is served by unexpected PID $($connection.OwningProcess), expected $($process.Id)" }
        try { $health = Invoke-RestMethod -Uri "http://127.0.0.1:$Port/api/inferdeck/v1/health" -TimeoutSec 2; break } catch { Start-Sleep -Milliseconds 250 }
    }
    if ($null -eq $health) { throw "Packaged gateway health endpoint did not respond within $StartupTimeoutSeconds seconds" }
    if ($health.ok -ne $true) { throw 'Packaged gateway health endpoint did not report ok=true' }
    $connection = Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction Stop | Select-Object -First 1
    if ($connection.OwningProcess -ne $process.Id) { throw "Health succeeded but listener PID $($connection.OwningProcess) is not child PID $($process.Id)" }
    $dashboard = Invoke-WebRequest -UseBasicParsing -Uri "http://127.0.0.1:$Port/" -TimeoutSec 5
    if ($dashboard.StatusCode -ne 200) { throw 'Packaged gateway did not serve dashboard HTML at /' }
    $servedIndexHash = Get-StreamHash $dashboard.RawContentStream
    $expectedIndexHash = Get-FileHashHex $indexPath
    if ($servedIndexHash -ne $expectedIndexHash) { throw "Served index hash $servedIndexHash does not match package hash $expectedIndexHash" }
    $servedAsset = Invoke-WebRequest -UseBasicParsing -Uri "http://127.0.0.1:$Port/assets/$assetName" -TimeoutSec 5
    $servedAssetHash = Get-StreamHash $servedAsset.RawContentStream
    $expectedAssetHash = Get-FileHashHex $assetPath
    if ($servedAssetHash -ne $expectedAssetHash) { throw "Served dashboard asset hash $servedAssetHash does not match package hash $expectedAssetHash" }
    [pscustomobject]@{ pid = $process.Id; port = $Port; health = $health.ok; index_sha256 = $expectedIndexHash; asset = $assetName; asset_sha256 = $expectedAssetHash } | ConvertTo-Json -Compress
} catch {
    if (Test-Path -LiteralPath $logPath) { Write-Output 'Packaged gateway log:'; Get-Content -LiteralPath $logPath }
    throw
} finally {
    if ($process -and !$process.HasExited) { Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue; $process.WaitForExit(5000) }
    if ((Test-Path -LiteralPath $tempResolved) -and $tempResolved.StartsWith($tempBase, [StringComparison]::OrdinalIgnoreCase)) { Remove-Item -LiteralPath $tempResolved -Recurse -Force -ErrorAction SilentlyContinue }
}