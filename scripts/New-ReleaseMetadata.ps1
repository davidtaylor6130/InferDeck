param(
    [Parameter(Mandatory = $true)]
    [string]$DistDir,
    [Parameter(Mandatory = $true)]
    [string]$OutputDir,
    [string]$VcpkgInstalledDir,
    [string]$Archive
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$resolvedDist = (Resolve-Path -LiteralPath $DistDir).Path
$resolvedOutput = [System.IO.Path]::GetFullPath($OutputDir)
New-Item -ItemType Directory -Path $resolvedOutput -Force | Out-Null

$version = (Get-Content -LiteralPath (Join-Path $repoRoot 'VERSION') -Raw).Trim()
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw 'VERSION is not semantic' }
$commit = (& git -C $repoRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $commit -notmatch '^[0-9a-f]{40}$') { throw 'Unable to resolve release commit' }
$vcpkg = Get-Content -LiteralPath (Join-Path $repoRoot 'vcpkg.json') -Raw | ConvertFrom-Json
$resolvedPackages = @()
if ($VcpkgInstalledDir) {
    $statusPath = Join-Path $VcpkgInstalledDir 'vcpkg/status'
    if (!(Test-Path -LiteralPath $statusPath)) { throw "vcpkg status is missing: $statusPath" }
    $records = (Get-Content -LiteralPath $statusPath -Raw) -split "(?:`r?`n){2,}"
    $resolvedPackages = @($records | ForEach-Object {
        $packageMatch = [regex]::Match($_, '(?m)^Package:\s*(\S+)')
        $versionMatch = [regex]::Match($_, '(?m)^Version:\s*(\S+)')
        if ($packageMatch.Success -and $versionMatch.Success) {
            [ordered]@{ name = $packageMatch.Groups[1].Value; version = $versionMatch.Groups[1].Value }
        }
    } | Where-Object { $_ } | Sort-Object name)
}
$submoduleLines = @(& git -C $repoRoot submodule status)
if ($LASTEXITCODE -ne 0) { throw 'Unable to resolve submodule revisions' }
$submodules = @($submoduleLines | ForEach-Object {
    if ($_ -notmatch '^[ +-]?([0-9a-f]{40})\s+(\S+)') { throw "Invalid submodule status: $_" }
    [ordered]@{ path = $Matches[2]; commit = $Matches[1] }
})
$inputs = @('VERSION', 'vcpkg.json', 'pnpm-lock.yaml', '.gitmodules') | ForEach-Object {
    $path = Join-Path $repoRoot $_
    [ordered]@{
        path = $_
        sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}
$dependencies = [ordered]@{
    product = 'InferDeck'
    version = $version
    commit = $commit
    vcpkg_baseline = $vcpkg.'builtin-baseline'
    vcpkg_dependencies = @($vcpkg.dependencies)
    resolved_vcpkg_packages = $resolvedPackages
    submodules = $submodules
    locked_inputs = @($inputs)
}
$dependencies | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $resolvedOutput 'DEPENDENCIES.json') -Encoding utf8

$packages = @(
    [ordered]@{
        SPDXID = 'SPDXRef-Package-InferDeck'
        name = 'InferDeck'
        versionInfo = $version
        downloadLocation = 'NOASSERTION'
        filesAnalyzed = $false
        licenseConcluded = 'NOASSERTION'
        licenseDeclared = 'MIT'
        supplier = 'Organization: InferDeck'
    }
)
$index = 0
foreach ($dependency in $vcpkg.dependencies) {
    $name = if ($dependency -is [string]) { $dependency } else { $dependency.name }
    $resolvedPackage = $resolvedPackages | Where-Object name -eq $name | Select-Object -First 1
    $resolvedVersion = if ($resolvedPackage) { $resolvedPackage.version } else { "vcpkg-baseline-$($vcpkg.'builtin-baseline')" }
    $index++
    $packages += [ordered]@{
        SPDXID = "SPDXRef-Package-vcpkg-$index"
        name = $name
        versionInfo = $resolvedVersion
        downloadLocation = 'NOASSERTION'
        filesAnalyzed = $false
        licenseConcluded = 'NOASSERTION'
        licenseDeclared = 'NOASSERTION'
        supplier = 'NOASSERTION'
    }
}
foreach ($submodule in $submodules) {
    $index++
    $packages += [ordered]@{
        SPDXID = "SPDXRef-Package-submodule-$index"
        name = $submodule.path
        versionInfo = $submodule.commit
        downloadLocation = 'NOASSERTION'
        filesAnalyzed = $false
        licenseConcluded = 'NOASSERTION'
        licenseDeclared = 'NOASSERTION'
        supplier = 'NOASSERTION'
    }
}
$relationships = @($packages | Select-Object -Skip 1 | ForEach-Object {
    [ordered]@{
        spdxElementId = 'SPDXRef-Package-InferDeck'
        relationshipType = 'DEPENDS_ON'
        relatedSpdxElement = $_.SPDXID
    }
})
$sbom = [ordered]@{
    spdxVersion = 'SPDX-2.3'
    dataLicense = 'CC0-1.0'
    SPDXID = 'SPDXRef-DOCUMENT'
    name = "InferDeck-$version"
    documentNamespace = "https://github.com/davidtaylor6130/InferDeck/releases/download/v$version/sbom-$commit"
    creationInfo = [ordered]@{
        created = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
        creators = @('Tool: InferDeck-New-ReleaseMetadata.ps1')
    }
    packages = $packages
    relationships = $relationships
}
$sbom | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $resolvedOutput 'SBOM.spdx.json') -Encoding utf8

$artifactRows = @(Get-ChildItem -LiteralPath $resolvedDist -File -Recurse | Sort-Object FullName | ForEach-Object {
    [ordered]@{
        path = $_.FullName.Substring($resolvedDist.Length).TrimStart('\', '/').Replace('\', '/')
        bytes = $_.Length
        sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
})
$release = [ordered]@{
    product = 'InferDeck'
    version = $version
    commit = $commit
    generated_at = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    artifacts = $artifactRows
}
if ($Archive) {
    $resolvedArchive = (Resolve-Path -LiteralPath $Archive).Path
    $release.archive = [ordered]@{
        name = [System.IO.Path]::GetFileName($resolvedArchive)
        bytes = (Get-Item -LiteralPath $resolvedArchive).Length
        sha256 = (Get-FileHash -LiteralPath $resolvedArchive -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    "$($release.archive.sha256)  $($release.archive.name)" | Set-Content -LiteralPath (Join-Path $resolvedOutput 'SHA256SUMS.txt') -Encoding ascii
}
$release | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $resolvedOutput 'release-manifest.json') -Encoding utf8
