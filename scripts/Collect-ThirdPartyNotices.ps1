param(
    [Parameter(Mandatory = $true)] [string]$OutputDir,
    [string]$RepoRoot,
    [string]$RuntimeRoot = "runtime",
    [string]$VcpkgInstalledDir,
    [string]$SherpaRoot,
    [string]$DashboardDependencyJson
)
$ErrorActionPreference = 'Stop'
if (!$RepoRoot) { $RepoRoot = Split-Path -Parent $PSScriptRoot }
$resolvedRepo = (Resolve-Path -LiteralPath $RepoRoot).Path
$resolvedOutput = [System.IO.Path]::GetFullPath($OutputDir)
$noticeDir = [System.IO.Path]::GetFullPath((Join-Path $resolvedOutput 'THIRD_PARTY_NOTICES'))
if (!(Split-Path -Parent $noticeDir).Equals($resolvedOutput.TrimEnd('\', '/'), [System.StringComparison]::OrdinalIgnoreCase)) { throw 'Notice output escaped the requested directory' }
New-Item -ItemType Directory -Path $resolvedOutput -Force | Out-Null
if (Test-Path -LiteralPath $noticeDir) { Remove-Item -LiteralPath $noticeDir -Recurse -Force }
New-Item -ItemType Directory -Path $noticeDir | Out-Null
$components = [ordered]@{}
function Resolve-RepoPath([string]$Path) {
    if (!$Path) { return $null }
    if ([System.IO.Path]::IsPathRooted($Path)) { return [System.IO.Path]::GetFullPath($Path) }
    [System.IO.Path]::GetFullPath((Join-Path $resolvedRepo $Path))
}
function Get-NoticeFiles([string]$Root) {
    if (!$Root -or !(Test-Path -LiteralPath $Root -PathType Container)) { return @() }
    @(Get-ChildItem -LiteralPath $Root -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^(LICENSE|LICENCE|NOTICE|COPYING)([._-].*)?$' } |
        Sort-Object Name)
}
function Add-Component([string]$Name, [string]$Version, [string]$Source, [object[]]$NoticePaths) {
    if (!$Name) { return }
    if (!$Version) { $Version = 'unknown' }
    $key = "$($Name.ToLowerInvariant())|$Version"
    if (!$components.Contains($key)) {
        $components[$key] = [ordered]@{ component = $Name; version = $Version; source = $Source; notices = [System.Collections.Generic.List[string]]::new(); notice_paths = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase) }
    }
    $record = $components[$key]
    foreach ($path in @($NoticePaths)) {
        if (!(Test-Path -LiteralPath $path -PathType Leaf)) { continue }
        $resolvedNotice = (Resolve-Path -LiteralPath $path).Path
        if (!$record.notice_paths.Add($resolvedNotice)) { continue }
        $safe = ($Name -replace '[^A-Za-z0-9._-]', '_')
        $fileName = "${safe}_$([System.IO.Path]::GetFileName($path))"
        $destination = Join-Path $noticeDir $fileName
        $suffix = 1
        while (Test-Path -LiteralPath $destination) {
            $fileName = "${safe}_${suffix}_$([System.IO.Path]::GetFileName($path))"
            $destination = Join-Path $noticeDir $fileName
            $suffix++
        }
        Copy-Item -LiteralPath $path -Destination $destination
        $record.notices.Add("THIRD_PARTY_NOTICES/$fileName")
    }
}
$productVersion = (Get-Content -LiteralPath (Join-Path $resolvedRepo 'VERSION') -Raw).Trim()
Add-Component 'InferDeck' $productVersion 'repository' @((Join-Path $resolvedRepo 'LICENSE'))
foreach ($native in @(
    @{ Name = 'llama.cpp'; Path = 'libs/third_party/llama.cpp' },
    @{ Name = 'Vulkan-Headers'; Path = 'libs/third_party/Vulkan-Headers' },
    @{ Name = 'stable-diffusion.cpp'; Path = 'libs/third_party/stable-diffusion.cpp' },
    @{ Name = 'acestep.cpp'; Path = 'libs/third_party/acestep.cpp' }
)) {
    $root = Resolve-RepoPath $native.Path
    if (!(Test-Path -LiteralPath $root -PathType Container)) { continue }
    $revision = 'unknown'
    if (Test-Path -LiteralPath (Join-Path $root '.git')) {
        $revision = (& git -C $root rev-parse HEAD | Out-String).Trim()
        if ($LASTEXITCODE -ne 0 -or $revision -notmatch '^[0-9a-f]{40}$') { throw "Unable to resolve revision: $root" }
    }
    $nativeNotices = @('LICENSE', 'LICENSE.md', 'NOTICE', 'NOTICE.txt', 'COPYING') |
        ForEach-Object { Join-Path $root $_ } |
        Where-Object { Test-Path -LiteralPath $_ -PathType Leaf }
    Add-Component $native.Name $revision $native.Path @($nativeNotices)
}
$runtimePath = Resolve-RepoPath $RuntimeRoot
$whisperRoot = if ($runtimePath) { Join-Path $runtimePath 'whisper.cpp-src' } else { $null }
if ($whisperRoot -and (Test-Path -LiteralPath $whisperRoot -PathType Container)) {
    $revision = 'unknown'
    if (Test-Path -LiteralPath (Join-Path $whisperRoot '.git')) {
        $revision = (& git -C $whisperRoot rev-parse HEAD | Out-String).Trim()
        if ($LASTEXITCODE -ne 0 -or $revision -notmatch '^[0-9a-f]{40}$') { throw "Unable to resolve revision: $whisperRoot" }
    }
    Add-Component 'whisper.cpp' $revision 'runtime/whisper.cpp-src' (Get-NoticeFiles $whisperRoot)
}
$resolvedSherpa = Resolve-RepoPath $SherpaRoot
if ($SherpaRoot -and !(Test-Path -LiteralPath $resolvedSherpa -PathType Container)) { throw 'Requested sherpa runtime is missing' }
if ($resolvedSherpa -and (Test-Path -LiteralPath $resolvedSherpa -PathType Container)) {
    $sherpaVersion = 'unknown'
    if ([System.IO.Path]::GetFileName($resolvedSherpa) -match '^sherpa-onnx-v([0-9.]+)(?:-|$)') { $sherpaVersion = $Matches[1] }
    $sherpaNotices = @(Get-NoticeFiles $resolvedSherpa) + @(Get-NoticeFiles (Join-Path $resolvedSherpa 'lib'))
    $sherpaNotices += @(Get-NoticeFiles (Join-Path $resolvedRepo "licenses/sherpa-onnx-$sherpaVersion"))
    Add-Component 'sherpa-onnx' $sherpaVersion "runtime/sherpa-onnx-$sherpaVersion" $sherpaNotices
    $onnxDll = Join-Path $resolvedSherpa 'lib/onnxruntime.dll'
    if (Test-Path -LiteralPath $onnxDll -PathType Leaf) {
        $onnxVersion = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($onnxDll).ProductVersion
        Add-Component 'onnxruntime' $onnxVersion 'runtime/onnxruntime.dll' (Get-NoticeFiles (Join-Path $resolvedRepo "licenses/onnxruntime-$onnxVersion"))
    }
}
$resolvedVcpkg = Resolve-RepoPath $VcpkgInstalledDir
if ($resolvedVcpkg) {
    $statusPath = Join-Path $resolvedVcpkg 'vcpkg/status'
    if (!(Test-Path -LiteralPath $statusPath -PathType Leaf)) { throw "vcpkg status is missing: $statusPath" }
    foreach ($status in ((Get-Content -LiteralPath $statusPath -Raw) -split "(?:`r?`n){2,}")) {
        $nameMatch = [regex]::Match($status, '(?m)^Package:\s*(\S+)')
        $versionMatch = [regex]::Match($status, '(?m)^Version:\s*(\S+)')
        if (!$nameMatch.Success -or !$versionMatch.Success) { continue }
        $name = $nameMatch.Groups[1].Value
        Add-Component $name $versionMatch.Groups[1].Value "vcpkg/x64-windows/share/$name" @((Join-Path $resolvedVcpkg "x64-windows/share/$name/copyright"))
    }
}
function Add-NpmDependencies($Dependencies) {
    if (!$Dependencies) { return }
    foreach ($property in $Dependencies.PSObject.Properties) {
        $dependency = $property.Value
        $name = if ($dependency.from) { [string]$dependency.from } else { [string]$property.Name }
        $version = if ($dependency.version) { [string]$dependency.version } else { 'unknown' }
        $path = if ($dependency.path) { [string]$dependency.path } else { $null }
        Add-Component $name $version 'pnpm/dashboard-production' (Get-NoticeFiles $path)
        Add-NpmDependencies $dependency.dependencies
    }
}
if ($DashboardDependencyJson) {
    $dependencyJson = Get-Content -LiteralPath $DashboardDependencyJson -Raw
} else {
    Push-Location $resolvedRepo
    try {
        $dependencyJson = (& pnpm --filter dashboard list --prod --depth Infinity --json | Out-String)
        if ($LASTEXITCODE -ne 0) { throw 'Unable to resolve dashboard production dependencies with pnpm' }
    } finally { Pop-Location }
}
$dependencyGraph = $dependencyJson | ConvertFrom-Json
foreach ($workspace in @($dependencyGraph)) { Add-NpmDependencies $workspace.dependencies }
$componentRows = @($components.Values | ForEach-Object {
    [ordered]@{ component = $_.component; version = $_.version; source = $_.source; notices = @($_.notices) }
})
$manifest = [ordered]@{
    schema_version = 1
    components = $componentRows
    missing_notices = @($componentRows | Where-Object { $_.notices.Count -eq 0 } | ForEach-Object {
        [ordered]@{ component = $_.component; version = $_.version; source = $_.source }
    })
}
$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $resolvedOutput 'THIRD_PARTY_NOTICES.json') -Encoding utf8