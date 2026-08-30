param(
    [Parameter(Mandatory = $true)]
    [string]$Gateway,
    [string]$ExpectedRevision = ''
)

$ErrorActionPreference = 'Stop'
$gatewayPath = (Resolve-Path -LiteralPath $Gateway).Path
if (-not $ExpectedRevision) {
    $repository = Split-Path -Parent $PSScriptRoot
    if (Test-Path -LiteralPath (Join-Path $repository '.git')) {
        $ExpectedRevision = (& git -C $repository rev-parse HEAD | Out-String).Trim()
        if ($LASTEXITCODE -ne 0) {
            throw "Unable to resolve the expected source revision: $LASTEXITCODE"
        }
    }
}
$version = (& $gatewayPath --version | Out-String).Trim()
if ($LASTEXITCODE -ne 0) {
    throw "Gateway version command failed with exit code $LASTEXITCODE"
}
if ($version -notmatch '^inferdeck-gateway (?<version>\d+\.\d+\.\d+) revision=(?<revision>[0-9a-f]{40}) dirty=(?<dirty>true|false)$') {
    throw "Gateway version output has no trustworthy build provenance: $version"
}
if ($ExpectedRevision -and $Matches.revision -ne $ExpectedRevision.ToLowerInvariant()) {
    throw "Gateway revision $($Matches.revision) does not match expected revision $ExpectedRevision"
}

[pscustomobject]@{
    version = $Matches.version
    revision = $Matches.revision
    dirty = [bool]::Parse($Matches.dirty)
} | ConvertTo-Json -Compress
