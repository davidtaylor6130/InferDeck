[CmdletBinding()]
param(
    [string] $ManifestPath = (Join-Path $PSScriptRoot 'ltx-2.3-manifest.json'),
    [string] $DestinationRoot,
    [ValidateRange(1, 2)] [int] $MaxParallel = 2
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Test-Artifact {
    param([object] $Artifact, [string] $Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
    $item = Get-Item -LiteralPath $Path
    if ($item.Length -ne [int64]$Artifact.size) { return $false }
    return ((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -eq $Artifact.sha256.ToUpperInvariant())
}

function Install-Artifact {
    param([object] $Artifact, [string] $Root)
    $finalPath = Join-Path $Root $Artifact.relativePath
    $partPath = "$finalPath.part"
    $parent = Split-Path -Parent $finalPath
    New-Item -ItemType Directory -Path $parent -Force | Out-Null

    if (Test-Artifact $Artifact $finalPath) {
        Write-Output "VALID $($Artifact.name)"
        return
    }
    if (Test-Path -LiteralPath $finalPath -PathType Leaf) {
        throw "Refusing to overwrite invalid existing file: $finalPath"
    }

    Write-Output "DOWNLOAD $($Artifact.name)"
    & curl.exe --fail --location --continue-at - --retry 5 --retry-all-errors --connect-timeout 30 --speed-time 60 --speed-limit 1024 --silent --show-error --output $partPath $Artifact.url
    if ($LASTEXITCODE -ne 0) { throw "curl failed for $($Artifact.name) with exit code $LASTEXITCODE" }

    $partItem = Get-Item -LiteralPath $partPath -ErrorAction Stop
    if ($partItem.Length -ne [int64]$Artifact.size) {
        throw "Size mismatch for $($Artifact.name): expected $($Artifact.size), got $($partItem.Length). The .part file was retained."
    }
    $hash = (Get-FileHash -LiteralPath $partPath -Algorithm SHA256).Hash
    if ($hash -ne $Artifact.sha256.ToUpperInvariant()) {
        throw "SHA256 mismatch for $($Artifact.name): expected $($Artifact.sha256), got $($hash). The .part file was retained."
    }
    Move-Item -LiteralPath $partPath -Destination $finalPath
    Write-Output "INSTALLED $($Artifact.name) sha256=$hash size=$($partItem.Length)"
}

if (-not (Get-Command curl.exe -ErrorAction SilentlyContinue)) { throw 'curl.exe is required.' }
$manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
if ($manifest.artifacts.Count -ne 5) { throw "Manifest must contain exactly five artifacts." }
if (-not $DestinationRoot) { $DestinationRoot = $manifest.destination }
$DestinationRoot = [IO.Path]::GetFullPath($DestinationRoot)
New-Item -ItemType Directory -Path $DestinationRoot -Force | Out-Null
Write-Output "TARGET $DestinationRoot artifacts=$($manifest.artifacts.Count) maxParallel=$MaxParallel"

# Use thread jobs when available; the fallback remains bounded at one download.
if ($false) {
    $jobs = @()
    foreach ($artifact in $manifest.artifacts) {
        while (($jobs | Where-Object State -eq 'Running').Count -ge $MaxParallel) {
            $done = Wait-Job -Job $jobs -Any
            Receive-Job -Job $done -ErrorAction Stop
            Remove-Job -Job $done
            $jobs = @($jobs | Where-Object Id -ne $done.Id)
        }
        $jobs += Start-ThreadJob -ScriptBlock ${function:Install-Artifact} -ArgumentList $artifact, $DestinationRoot
    }
    while ($jobs.Count) {
        $done = Wait-Job -Job $jobs -Any
        Receive-Job -Job $done -ErrorAction Stop
        Remove-Job -Job $done
        $jobs = @($jobs | Where-Object Id -ne $done.Id)
    }
} else {
    foreach ($artifact in $manifest.artifacts) { Install-Artifact $artifact $DestinationRoot }
}
Write-Output 'COMPLETE 5/5 artifacts verified and installed.'
