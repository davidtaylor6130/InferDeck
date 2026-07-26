param(
    [string]$Revision = "080bbbe85230f624f0b52127f1ae1218247989f9",
    [string]$Model = "base.en",
    [string]$ModelDirectory = "C:\Users\david\Documents\00_Models\Whisper",
    [string]$RuntimeRoot = "runtime",
    [switch]$SkipModel
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$runtimeRootPath = Join-Path $repoRoot $RuntimeRoot
$sourceDir = Join-Path $runtimeRootPath "whisper.cpp-src"

New-Item -ItemType Directory -Force -Path $runtimeRootPath | Out-Null
if (!$SkipModel) {
    New-Item -ItemType Directory -Force -Path $ModelDirectory | Out-Null
}

if (!(Test-Path $sourceDir)) {
    git clone --filter=blob:none https://github.com/ggml-org/whisper.cpp.git $sourceDir
    if ($LASTEXITCODE -ne 0) { throw "Failed to clone whisper.cpp" }
}
if (!(Test-Path (Join-Path $sourceDir ".git"))) {
    throw "Whisper source path is not a Git checkout: $sourceDir"
}
git -C $sourceDir fetch --depth 1 origin $Revision
if ($LASTEXITCODE -ne 0) { throw "Failed to fetch whisper.cpp revision $Revision" }
git -C $sourceDir checkout --detach $Revision
if ($LASTEXITCODE -ne 0) { throw "Failed to check out whisper.cpp revision $Revision" }
$resolvedRevision = (git -C $sourceDir rev-parse HEAD).Trim()
if ($resolvedRevision -ne $Revision) {
    throw "Whisper revision mismatch: expected $Revision, got $resolvedRevision"
}

$modelFile = $null
if (!$SkipModel) {
    $modelFile = Join-Path $ModelDirectory "ggml-$Model.bin"
    if (!(Test-Path $modelFile)) {
        $url = "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-$Model.bin"
        Invoke-WebRequest -Uri $url -OutFile $modelFile
    }
}

[pscustomobject]@{
    source = $sourceDir
    revision = $resolvedRevision
    modelDirectory = if ($modelFile) { $ModelDirectory } else { $null }
    model = if ($modelFile) { Split-Path $modelFile -Leaf } else { $null }
    cmakeOption = "-DINFERDECK_WHISPER_ROOT=$($sourceDir -replace '\\','/')"
}
