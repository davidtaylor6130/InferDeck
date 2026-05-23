param(
    [string]$Version = "v1.8.4",
    [string]$Model = "base.en",
    [string]$ModelDirectory = "C:\Users\david\Documents\00_Models\Whisper",
    [string]$RuntimeRoot = "runtime",
    [switch]$SkipVulkan
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$runtimeRootPath = Join-Path $repoRoot $RuntimeRoot
$sourceDir = Join-Path $runtimeRootPath "whisper.cpp-$Version-src"
$vulkanBuildDir = Join-Path $runtimeRootPath "whisper.cpp-$Version-build"
$cpuBuildDir = Join-Path $runtimeRootPath "whisper.cpp-$Version-cpu-build"

New-Item -ItemType Directory -Force -Path $runtimeRootPath | Out-Null
New-Item -ItemType Directory -Force -Path $ModelDirectory | Out-Null

if (!(Test-Path $sourceDir)) {
    git clone --depth 1 --branch $Version https://github.com/ggml-org/whisper.cpp.git $sourceDir
}

function Build-Whisper {
    param(
        [string]$BuildDir,
        [bool]$UseVulkan
    )

    $args = @(
        "-S", $sourceDir,
        "-B", $BuildDir,
        "-DWHISPER_BUILD_TESTS=OFF",
        "-DWHISPER_BUILD_EXAMPLES=ON",
        "-DWHISPER_BUILD_SERVER=OFF"
    )
    if ($UseVulkan) {
        $args += "-DGGML_VULKAN=ON"
    }

    cmake @args
    cmake --build $BuildDir --target whisper-cli --config Release
}

$backend = "cpu"
$buildDir = $cpuBuildDir
if (!$SkipVulkan) {
    try {
        Build-Whisper -BuildDir $vulkanBuildDir -UseVulkan $true
        $backend = "vulkan"
        $buildDir = $vulkanBuildDir
    } catch {
        Write-Warning "Vulkan whisper.cpp build failed; falling back to CPU. $($_.Exception.Message)"
        Build-Whisper -BuildDir $cpuBuildDir -UseVulkan $false
    }
} else {
    Build-Whisper -BuildDir $cpuBuildDir -UseVulkan $false
}

$modelFile = Join-Path $ModelDirectory "ggml-$Model.bin"
if (!(Test-Path $modelFile)) {
    $url = "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-$Model.bin"
    Invoke-WebRequest -Uri $url -OutFile $modelFile
}

$exe = Join-Path $buildDir "bin\Release\whisper-cli.exe"
if (!(Test-Path $exe)) {
    throw "whisper-cli.exe was not produced at $exe"
}

[pscustomobject]@{
    backend = $backend
    executable = $exe
    modelDirectory = $ModelDirectory
    model = Split-Path $modelFile -Leaf
}
