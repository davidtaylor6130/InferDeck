param(
    [Parameter(Mandatory = $true)] [string]$DistDir,
    [Parameter(Mandatory = $true)] [string]$OutputDir,
    [string]$VcpkgInstalledDir,
    [string]$Archive,
    [string]$RepoRoot
)
$ErrorActionPreference = 'Stop'
if (!$RepoRoot) { $RepoRoot = Split-Path -Parent $PSScriptRoot }
$repoRoot = (Resolve-Path -LiteralPath $RepoRoot).Path
$resolvedDist = (Resolve-Path -LiteralPath $DistDir).Path
$resolvedOutput = [System.IO.Path]::GetFullPath($OutputDir)
New-Item -ItemType Directory -Path $resolvedOutput -Force | Out-Null
$version = (Get-Content -LiteralPath (Join-Path $repoRoot 'VERSION') -Raw).Trim()
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw 'VERSION is not semantic' }
$commit = (& git -C $repoRoot rev-parse HEAD | Out-String).Trim()
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
    } | Where-Object { $_ } | Sort-Object name, version -Unique)
}
$submoduleLines = @(& git -C $repoRoot submodule status)
if ($LASTEXITCODE -ne 0) { throw 'Unable to resolve submodule revisions' }
$submodules = @($submoduleLines | ForEach-Object {
    if ($_ -notmatch '^[ +-]?([0-9a-f]{40})\s+(\S+)') { throw "Invalid submodule status: $_" }
    [ordered]@{ path = $Matches[2]; commit = $Matches[1] }
})
$inputs = @('VERSION', 'vcpkg.json', 'pnpm-lock.yaml', '.gitmodules') | ForEach-Object {
    $path = Join-Path $repoRoot $_
    [ordered]@{ path = $_; sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }
}
$noticeComponents = @()
$missingNotices = @()
$noticeManifest = Join-Path $resolvedDist 'THIRD_PARTY_NOTICES.json'
if (Test-Path -LiteralPath $noticeManifest) {
    $noticeDocument = Get-Content -LiteralPath $noticeManifest -Raw | ConvertFrom-Json
    if ($noticeDocument.components) {
        $noticeComponents = @($noticeDocument.components)
        $missingNotices = @($noticeDocument.missing_notices)
    } else {
        $noticeComponents = @($noticeDocument)
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
    notice_components = @($noticeComponents | ForEach-Object {
        [ordered]@{ name = [string]$_.component; version = [string]$_.version; source = [string]$_.source }
    })
    missing_notices = $missingNotices
    locked_inputs = @($inputs)
}
$dependencies | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $resolvedOutput 'DEPENDENCIES.json') -Encoding utf8

$packages = [System.Collections.Generic.List[object]]::new()
$packageKeys = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
$index = 0
function Add-SpdxPackage([string]$Name, [string]$ComponentVersion, [string]$Kind, [string]$License) {
    if (!$Name) { return }
    if (!$ComponentVersion) { $ComponentVersion = 'unknown' }
    if (!$packageKeys.Add("$Name|$ComponentVersion")) { return }
    $script:index++
    $packages.Add([ordered]@{
        SPDXID = "SPDXRef-Package-$($Kind -replace '[^A-Za-z0-9.-]', '-')-$($script:index)"
        name = $Name
        versionInfo = $ComponentVersion
        downloadLocation = 'NOASSERTION'
        filesAnalyzed = $false
        licenseConcluded = 'NOASSERTION'
        licenseDeclared = $License
        supplier = 'NOASSERTION'
    })
}
Add-SpdxPackage 'InferDeck' $version 'product' 'MIT'
if ($resolvedPackages.Count -gt 0) {
    foreach ($dependency in $resolvedPackages) {
        Add-SpdxPackage ([string]$dependency.name) ([string]$dependency.version) 'vcpkg' 'NOASSERTION'
    }
} else {
    foreach ($dependency in $vcpkg.dependencies) {
        $name = if ($dependency -is [string]) { $dependency } else { $dependency.name }
        Add-SpdxPackage ([string]$name) 'unknown' 'vcpkg' 'NOASSERTION'
    }
}
foreach ($submodule in $submodules) {
    Add-SpdxPackage ([System.IO.Path]::GetFileName([string]$submodule.path)) ([string]$submodule.commit) 'submodule' 'NOASSERTION'
}
foreach ($notice in $noticeComponents) {
    if ([string]$notice.component -eq 'InferDeck') { continue }
    Add-SpdxPackage ([string]$notice.component) ([string]$notice.version) 'notice' 'NOASSERTION'
}
$relationships = @($packages | Select-Object -Skip 1 | ForEach-Object {
    [ordered]@{
        spdxElementId = 'SPDXRef-Package-product-1'
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
    packages = @($packages)
    relationships = $relationships
}
$sbom | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $resolvedOutput 'SBOM.spdx.json') -Encoding utf8
$artifactRows = @(Get-ChildItem -LiteralPath $resolvedDist -File -Recurse | Where-Object Name -NotIn @('release-manifest.json', 'SHA256SUMS.txt') | Sort-Object FullName | ForEach-Object {
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
