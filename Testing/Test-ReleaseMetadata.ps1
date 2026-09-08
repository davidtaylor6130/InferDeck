$ErrorActionPreference = 'Stop'
$root = Join-Path ([System.IO.Path]::GetTempPath()) ('inferdeck-release-test-' + [guid]::NewGuid())
$repo = Join-Path $root 'repo'
$dist = Join-Path $root 'dist'
$metadata = Join-Path $root 'metadata'
try {
    foreach ($path in @(
        'libs/third_party/llama.cpp/vendor/unused',
        'runtime/whisper.cpp-src',
        'runtime/sherpa-onnx-v1.13.2-win-x64-shared-MD-Release-lib/lib',
        'node_modules/@scope/widget',
        'node_modules/transitive',
        'node_modules/missing',
        'vcpkg_installed/vcpkg',
        'vcpkg_installed/x64-windows/share/fmt'
    )) { New-Item -ItemType Directory -Path (Join-Path $repo $path) -Force | Out-Null }
    New-Item -ItemType Directory -Path $dist, $metadata -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $repo 'VERSION') '1.2.3'
    Set-Content -LiteralPath (Join-Path $repo 'LICENSE') 'MIT'
    Set-Content -LiteralPath (Join-Path $repo 'libs/third_party/llama.cpp/LICENSE') 'llama'
    Set-Content -LiteralPath (Join-Path $repo 'libs/third_party/llama.cpp/NOTICE.txt') 'llama notice'
    Set-Content -LiteralPath (Join-Path $repo 'libs/third_party/llama.cpp/vendor/unused/LICENSE') 'must not collect'
    Set-Content -LiteralPath (Join-Path $repo 'runtime/whisper.cpp-src/LICENSE') 'whisper'
    Set-Content -LiteralPath (Join-Path $repo 'runtime/sherpa-onnx-v1.13.2-win-x64-shared-MD-Release-lib/LICENSE') 'sherpa'
    Set-Content -LiteralPath (Join-Path $repo 'runtime/sherpa-onnx-v1.13.2-win-x64-shared-MD-Release-lib/lib/onnxruntime.dll') 'fixture'
    Set-Content -LiteralPath (Join-Path $repo 'node_modules/@scope/widget/LICENSE.md') 'scoped'
    Set-Content -LiteralPath (Join-Path $repo 'node_modules/transitive/NOTICE') 'transitive'
    Set-Content -LiteralPath (Join-Path $repo 'vcpkg_installed/x64-windows/share/fmt/copyright') 'fmt'
    Set-Content -LiteralPath (Join-Path $repo 'vcpkg_installed/vcpkg/status') "Package: fmt`nVersion: 11.1.4`n"
    Set-Content -LiteralPath (Join-Path $repo 'vcpkg.json') '{"name":"fixture","version":"1.2.3","builtin-baseline":"0000000000000000000000000000000000000000","dependencies":["fmt"]}'
    Set-Content -LiteralPath (Join-Path $repo 'package.json') '{"name":"fixture","version":"1.2.3"}'
    Set-Content -LiteralPath (Join-Path $repo 'pnpm-lock.yaml') 'lockfileVersion: 9'
    Set-Content -LiteralPath (Join-Path $repo '.gitmodules') ''
    $graphPath = Join-Path $root 'dependencies.json'
    @(@{
        name = 'dashboard'
        dependencies = @{
            '@scope/widget' = @{
                from = '@scope/widget'; version = '2.0.0'; path = (Join-Path $repo 'node_modules/@scope/widget')
                dependencies = @{ transitive = @{ from = 'transitive'; version = '3.1.0'; path = (Join-Path $repo 'node_modules/transitive') } }
            }
            missing = @{ from = 'missing'; version = '4.0.0'; path = (Join-Path $repo 'node_modules/missing') }
        }
    }) | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $graphPath -Encoding utf8
    & (Join-Path $PSScriptRoot '..\scripts\Collect-ThirdPartyNotices.ps1') `
        -RepoRoot $repo -OutputDir $dist -DashboardDependencyJson $graphPath `
        -VcpkgInstalledDir (Join-Path $repo 'vcpkg_installed') `
        -SherpaRoot (Join-Path $repo 'runtime/sherpa-onnx-v1.13.2-win-x64-shared-MD-Release-lib')
    $manifest = Get-Content -LiteralPath (Join-Path $dist 'THIRD_PARTY_NOTICES.json') -Raw | ConvertFrom-Json
    foreach ($expected in @('InferDeck', 'llama.cpp', 'whisper.cpp', 'sherpa-onnx', 'onnxruntime', 'fmt', '@scope/widget', 'transitive', 'missing')) {
        if (!($manifest.components | Where-Object component -eq $expected)) { throw "component missing: $expected" }
    }
    $llamaNotices = @(($manifest.components | Where-Object component -eq 'llama.cpp').notices)
    if (!($llamaNotices | Where-Object { $_ -like '*_LICENSE' })) { throw 'native license was not collected' }
    if (!($llamaNotices | Where-Object { $_ -like '*_NOTICE.txt' })) { throw 'native notice was not collected' }
    if (($manifest.components | Where-Object component -eq '@scope/widget').version -ne '2.0.0') { throw 'scoped npm version is wrong' }
    if (($manifest.components | Where-Object component -eq 'transitive').version -ne '3.1.0') { throw 'transitive npm version is wrong' }
    if (($manifest.components | Where-Object component -eq 'fmt').version -ne '11.1.4') { throw 'vcpkg version is wrong' }
    if (!($manifest.missing_notices | Where-Object component -eq 'missing')) { throw 'missing npm notice was not reported' }
    if (!($manifest.missing_notices | Where-Object component -eq 'onnxruntime')) { throw 'missing native notice was not reported' }
    if (Get-ChildItem -LiteralPath (Join-Path $dist 'THIRD_PARTY_NOTICES') -File | Where-Object { (Get-Content -LiteralPath $_.FullName -Raw) -eq 'must not collect' }) {
        throw 'nested unrelated license was collected'
    }
    & git -C $repo init --quiet
    & git -C $repo config user.email 'fixture@example.invalid'
    & git -C $repo config user.name 'Release Fixture'
    & git -C $repo add VERSION LICENSE vcpkg.json package.json pnpm-lock.yaml .gitmodules
    & git -C $repo commit --quiet -m fixture
    if ($LASTEXITCODE -ne 0) { throw 'unable to create fixture revision' }
    & (Join-Path $PSScriptRoot '..\scripts\New-ReleaseMetadata.ps1') `
        -RepoRoot $repo -DistDir $dist -OutputDir $metadata `
        -VcpkgInstalledDir (Join-Path $repo 'vcpkg_installed')
    $sbom = Get-Content -LiteralPath (Join-Path $metadata 'SBOM.spdx.json') -Raw | ConvertFrom-Json
    $keys = @($sbom.packages | ForEach-Object { "$($_.name)|$($_.versionInfo)" })
    if (($keys | Sort-Object -Unique).Count -ne $keys.Count) { throw 'SBOM contains duplicate component/version packages' }
    if (@($sbom.packages | Where-Object { $_.name -eq 'fmt' -and $_.versionInfo -eq '11.1.4' }).Count -ne 1) { throw 'vcpkg notice package was not deduplicated' }
    if (!($sbom.packages | Where-Object name -eq '@scope/widget')) { throw 'scoped npm package missing from SBOM' }
    if (!($sbom.packages | Where-Object name -eq 'transitive')) { throw 'transitive npm package missing from SBOM' }
    'RELEASE_METADATA_OK'
} finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}