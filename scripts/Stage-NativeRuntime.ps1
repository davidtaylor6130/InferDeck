[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Destination, [string]$Configuration = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build/bin/Release'
$allowed = [IO.Path]::GetFullPath((Join-Path $root 'build/private-releases')) + [IO.Path]::DirectorySeparatorChar
$target = [IO.Path]::GetFullPath($(if([IO.Path]::IsPathRooted($Destination)){$Destination}else{Join-Path $root $Destination}))
if (!$target.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase)) { throw 'Destination must be a new directory under build/private-releases' }
if (Test-Path -LiteralPath $target) { throw 'Destination already exists; preserve it and choose a new candidate directory' }
$configPath = ''
if (-not [string]::IsNullOrWhiteSpace($Configuration)) {
    $configPath = [IO.Path]::GetFullPath($Configuration)
    if (!(Test-Path -LiteralPath $configPath -PathType Leaf)) { throw 'Configuration must be an existing file' }
}
$exe = Join-Path $build 'inferdeck-gateway.exe'
$version = (& $exe --version | Out-String).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Gateway version check failed' }
if ($version -notmatch 'revision=([0-9a-f]{40}) dirty=(true|false)') { throw 'Gateway version lacks build provenance' }
$revision = $Matches[1]
$dirty = $Matches[2] -eq 'true'
foreach ($required in @('python312.dll','python/inferdeck_vllm_radiance_profile.py','python/inferdeck_vllm_radiance_lifecycle.py','python/inferdeck_vllm_radiance_tokenizer.py','static/index.html')) {
    if (!(Test-Path -LiteralPath (Join-Path $build $required) -PathType Leaf)) { throw ('Missing candidate artifact: ' + $required) }
}
New-Item -ItemType Directory -Path $target | Out-Null
Copy-Item -LiteralPath $exe -Destination $target
Get-ChildItem -LiteralPath $build -Filter '*.dll' -File | Where-Object Name -NotIn @('fmtd.dll','spdlogd.dll') | Copy-Item -Destination $target
Copy-Item -LiteralPath (Join-Path $build 'static') -Destination $target -Recurse
New-Item -ItemType Directory -Path (Join-Path $target 'python') | Out-Null
foreach ($module in @('inferdeck_vllm_radiance_profile.py','inferdeck_vllm_radiance_lifecycle.py','inferdeck_vllm_radiance_tokenizer.py')) {
    Copy-Item -LiteralPath (Join-Path $build ('python/' + $module)) -Destination (Join-Path $target 'python')
}
if ($configPath) { New-Item -ItemType Directory -Path (Join-Path $target 'config') | Out-Null; Copy-Item -LiteralPath $configPath -Destination (Join-Path $target 'config/gateway.yml') }
$hashes = [ordered]@{}
Get-ChildItem -LiteralPath $target -File -Recurse | ForEach-Object {
    $relative = $_.FullName.Substring($target.Length + 1)
    $hashes[$relative] = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
}
[ordered]@{artifactRoot='.';version=$version;revision=$revision;dirty=$dirty;utc=[DateTime]::UtcNow.ToString('o');status='STAGED_NOT_DEPLOYED';hashes=$hashes;configuration=$(if ($configPath) { @{sourceSha256=(Get-FileHash -LiteralPath $configPath -Algorithm SHA256).Hash.ToLowerInvariant();stagedPath='config/gateway.yml'} } else { $null });externalDependencies='Pinned Python root/site, vLLM/Radiance sources and binaries, ROCm packages, model weights and artifact tree remain external. A configuration is copied only when explicitly selected with -Configuration.'} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $target 'candidate-manifest.json') -Encoding utf8
Write-Output ('STAGED ' + $target)
