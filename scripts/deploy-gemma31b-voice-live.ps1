param(
    [string]$ResultPath = "F:\InferDeckBuild\gemma31b-voice-v2\deploy-result-20260809-2320.json"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$liveRoot = "C:\InferDeck"
$buildRoot = "F:\InferDeckBuild\gemma31b-voice-v2\bin\Release"
$repoRoot = "C:\Users\david\Documents\GitHub\InferDeck"
$backupRoot = "F:\InferDeckBackups\gemma31b-voice-predeploy-20260809-2301"
$activeBackup = "F:\InferDeckBackups\gemma31b-voice-active-20260809-2318\gateway.active.yml"
$activeConfig = "$liveRoot\config\gateway.active.yml"
$serviceName = "InferDeck"
$deploymentStarted = $false
$rollbackSucceeded = $false

function Assert-Admin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw "Administrator elevation is required"
    }
}

function Assert-Directory([string]$Path) {
    $item = Get-Item -LiteralPath $Path -Force
    if (-not $item.PSIsContainer) {
        throw "Expected directory: $Path"
    }
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Refusing reparse-point directory: $Path"
    }
}

function Assert-File([string]$Path) {
    $item = Get-Item -LiteralPath $Path -Force
    if ($item.PSIsContainer) {
        throw "Expected file: $Path"
    }
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Refusing reparse-point file: $Path"
    }
}

function Assert-MatchingFile([string]$Current, [string]$Backup) {
    Assert-File $Current
    Assert-File $Backup
    $currentHash = (Get-FileHash -LiteralPath $Current -Algorithm SHA256).Hash
    $backupHash = (Get-FileHash -LiteralPath $Backup -Algorithm SHA256).Hash
    if ($currentHash -ne $backupHash) {
        throw "Live file changed after backup: $Current"
    }
}

function Copy-Verified([string]$Source, [string]$Destination) {
    Assert-File $Source
    Copy-Item -LiteralPath $Source -Destination $Destination -Force
    $sourceHash = (Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash
    $destinationHash = (Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash
    if ($sourceHash -ne $destinationHash) {
        throw "Hash verification failed: $Destination"
    }
}

function Replace-OneLiteral([string]$Content, [string]$Old, [string]$New) {
    $count = ([regex]::Matches($Content, [regex]::Escape($Old))).Count
    if ($count -ne 1) {
        throw "Expected exactly one active-profile match for: $Old"
    }
    return $Content.Replace($Old, $New)
}

function Replace-OneRegex([string]$Content, [string]$Pattern, [string]$Replacement) {
    $regex = [regex]::new($Pattern)
    if ($regex.Matches($Content).Count -ne 1) {
        throw "Expected exactly one active-profile regex match"
    }
    return $regex.Replace($Content, $Replacement, 1)
}

function Stop-InferDeck {
    $service = Get-Service -Name $serviceName
    if ($service.Status -ne "Stopped") {
        Stop-Service -Name $serviceName -Force
        $service.WaitForStatus("Stopped", [TimeSpan]::FromSeconds(180))
    }
}

function Start-InferDeck {
    $service = Get-Service -Name $serviceName
    if ($service.Status -ne "Running") {
        Start-Service -Name $serviceName
        $service.WaitForStatus("Running", [TimeSpan]::FromSeconds(60))
    }
}

function Wait-ForHealth {
    $deadline = [DateTime]::UtcNow.AddSeconds(120)
    do {
        try {
            $health = Invoke-RestMethod -Uri "http://127.0.0.1:11434/v1/health" -TimeoutSec 5
            if ($health.ok -eq $true) {
                return $health
            }
        } catch {
        }
        Start-Sleep -Milliseconds 500
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Live health endpoint did not become ready"
}

function Write-Result([hashtable]$Result) {
    $json = $Result | ConvertTo-Json -Depth 8
    [IO.File]::WriteAllText($ResultPath, $json, [Text.UTF8Encoding]::new($false))
}

Assert-Admin

$checkedDirectories = @(
    $liveRoot,
    "$liveRoot\bin",
    "$liveRoot\config",
    "$liveRoot\static",
    "$liveRoot\static\assets",
    "$liveRoot\bin\static",
    "$liveRoot\bin\static\assets",
    $buildRoot,
    "$repoRoot\config",
    "$repoRoot\apps\inferdeck-gateway\static",
    "$repoRoot\apps\inferdeck-gateway\static\assets",
    $backupRoot
)
foreach ($directory in $checkedDirectories) {
    Assert-Directory $directory
}

$backupMappings = @(
    [pscustomobject]@{ Current = "$liveRoot\inferdeck-gateway.exe"; Backup = "$backupRoot\root\inferdeck-gateway.exe" },
    [pscustomobject]@{ Current = "$liveRoot\ggml.dll"; Backup = "$backupRoot\root\ggml.dll" },
    [pscustomobject]@{ Current = "$liveRoot\ggml-base.dll"; Backup = "$backupRoot\root\ggml-base.dll" },
    [pscustomobject]@{ Current = "$liveRoot\ggml-cpu.dll"; Backup = "$backupRoot\root\ggml-cpu.dll" },
    [pscustomobject]@{ Current = "$liveRoot\ggml-vulkan.dll"; Backup = "$backupRoot\root\ggml-vulkan.dll" },
    [pscustomobject]@{ Current = "$liveRoot\llama.dll"; Backup = "$backupRoot\root\llama.dll" },
    [pscustomobject]@{ Current = "$liveRoot\llama-common.dll"; Backup = "$backupRoot\root\llama-common.dll" },
    [pscustomobject]@{ Current = "$liveRoot\mtmd.dll"; Backup = "$backupRoot\root\mtmd.dll" },
    [pscustomobject]@{ Current = "$liveRoot\whisper.dll"; Backup = "$backupRoot\root\whisper.dll" },
    [pscustomobject]@{ Current = "$liveRoot\bin\gateway-service.exe"; Backup = "$backupRoot\bin\gateway-service.exe" },
    [pscustomobject]@{ Current = "$liveRoot\bin\ggml.dll"; Backup = "$backupRoot\bin\ggml.dll" },
    [pscustomobject]@{ Current = "$liveRoot\bin\ggml-base.dll"; Backup = "$backupRoot\bin\ggml-base.dll" },
    [pscustomobject]@{ Current = "$liveRoot\bin\ggml-cpu.dll"; Backup = "$backupRoot\bin\ggml-cpu.dll" },
    [pscustomobject]@{ Current = "$liveRoot\bin\ggml-vulkan.dll"; Backup = "$backupRoot\bin\ggml-vulkan.dll" },
    [pscustomobject]@{ Current = "$liveRoot\bin\llama.dll"; Backup = "$backupRoot\bin\llama.dll" },
    [pscustomobject]@{ Current = "$liveRoot\bin\llama-common.dll"; Backup = "$backupRoot\bin\llama-common.dll" },
    [pscustomobject]@{ Current = "$liveRoot\bin\mtmd.dll"; Backup = "$backupRoot\bin\mtmd.dll" },
    [pscustomobject]@{ Current = "$liveRoot\config\gateway.yml"; Backup = "$backupRoot\config\gateway.yml" },
    [pscustomobject]@{ Current = $activeConfig; Backup = $activeBackup },
    [pscustomobject]@{ Current = "$liveRoot\static\index.html"; Backup = "$backupRoot\static-root\index.html" },
    [pscustomobject]@{ Current = "$liveRoot\static\assets\index-BqY_Bxxq.css"; Backup = "$backupRoot\static-root\assets\index-BqY_Bxxq.css" },
    [pscustomobject]@{ Current = "$liveRoot\static\assets\index-CbYcjiA3.js"; Backup = "$backupRoot\static-root\assets\index-CbYcjiA3.js" },
    [pscustomobject]@{ Current = "$liveRoot\bin\static\index.html"; Backup = "$backupRoot\static-bin\index.html" },
    [pscustomobject]@{ Current = "$liveRoot\bin\static\assets\index-8ZIS3MYD.css"; Backup = "$backupRoot\static-bin\assets\index-8ZIS3MYD.css" },
    [pscustomobject]@{ Current = "$liveRoot\bin\static\assets\index-B45EBCSg.css"; Backup = "$backupRoot\static-bin\assets\index-B45EBCSg.css" },
    [pscustomobject]@{ Current = "$liveRoot\bin\static\assets\index-B4EZ0xRA.js"; Backup = "$backupRoot\static-bin\assets\index-B4EZ0xRA.js" },
    [pscustomobject]@{ Current = "$liveRoot\bin\static\assets\index-sjsdnA4m.js"; Backup = "$backupRoot\static-bin\assets\index-sjsdnA4m.js" }
)
foreach ($mapping in $backupMappings) {
    Assert-MatchingFile $mapping.Current $mapping.Backup
}

$activeProfileText = [IO.File]::ReadAllText($activeConfig)
if ($activeProfileText.Contains('name: "whisper-base-en"')) {
    throw "Active profile already contains whisper-base-en"
}
$activeProfileText = Replace-OneLiteral $activeProfileText 'default_model: "qwen3.6-27b"' 'default_model: "gemma-4-31b"'
$activeProfileText = Replace-OneRegex $activeProfileText '(?ms)(^  - name: "gemma-4-31b"\r?\n.*?^    context_size: )32768(?=\r?$)' '${1}262144'
$newline = if ($activeProfileText.Contains("`r`n")) { "`r`n" } else { "`n" }
$whisperBlock = @(
    '  - name: "whisper-base-en"',
    '    family: "whisper"',
    '    runtime: "whisper_cpp"',
    '    modality: "audio_transcription"',
    '    capabilities:',
    '      - "audio_transcription"',
    '    n_slots: 1',
    '    min_slots: 1',
    '    vram_required_mb: 0',
    '    artifacts:',
    '      model: "E:/InferDeck/models/stt/whisper/ggml-base.en.bin"'
) -join $newline
$activeProfileText = Replace-OneLiteral $activeProfileText '  - name: "qwen3.6-27b"' ($whisperBlock + $newline + $newline + '  - name: "qwen3.6-27b"')

$buildCopies = @(
    [pscustomobject]@{ Source = "$buildRoot\inferdeck-gateway.exe"; Destination = "$liveRoot\inferdeck-gateway.exe" },
    [pscustomobject]@{ Source = "$buildRoot\ggml.dll"; Destination = "$liveRoot\ggml.dll" },
    [pscustomobject]@{ Source = "$buildRoot\ggml-base.dll"; Destination = "$liveRoot\ggml-base.dll" },
    [pscustomobject]@{ Source = "$buildRoot\ggml-cpu.dll"; Destination = "$liveRoot\ggml-cpu.dll" },
    [pscustomobject]@{ Source = "$buildRoot\ggml-vulkan.dll"; Destination = "$liveRoot\ggml-vulkan.dll" },
    [pscustomobject]@{ Source = "$buildRoot\llama.dll"; Destination = "$liveRoot\llama.dll" },
    [pscustomobject]@{ Source = "$buildRoot\llama-common.dll"; Destination = "$liveRoot\llama-common.dll" },
    [pscustomobject]@{ Source = "$buildRoot\mtmd.dll"; Destination = "$liveRoot\mtmd.dll" },
    [pscustomobject]@{ Source = "$buildRoot\whisper.dll"; Destination = "$liveRoot\whisper.dll" },
    [pscustomobject]@{ Source = "$buildRoot\inferdeck-gateway.exe"; Destination = "$liveRoot\bin\gateway-service.exe" },
    [pscustomobject]@{ Source = "$buildRoot\ggml.dll"; Destination = "$liveRoot\bin\ggml.dll" },
    [pscustomobject]@{ Source = "$buildRoot\ggml-base.dll"; Destination = "$liveRoot\bin\ggml-base.dll" },
    [pscustomobject]@{ Source = "$buildRoot\ggml-cpu.dll"; Destination = "$liveRoot\bin\ggml-cpu.dll" },
    [pscustomobject]@{ Source = "$buildRoot\ggml-vulkan.dll"; Destination = "$liveRoot\bin\ggml-vulkan.dll" },
    [pscustomobject]@{ Source = "$buildRoot\llama.dll"; Destination = "$liveRoot\bin\llama.dll" },
    [pscustomobject]@{ Source = "$buildRoot\llama-common.dll"; Destination = "$liveRoot\bin\llama-common.dll" },
    [pscustomobject]@{ Source = "$buildRoot\mtmd.dll"; Destination = "$liveRoot\bin\mtmd.dll" },
    [pscustomobject]@{ Source = "$buildRoot\whisper.dll"; Destination = "$liveRoot\bin\whisper.dll" },
    [pscustomobject]@{ Source = "$buildRoot\sherpa-onnx-c-api.dll"; Destination = "$liveRoot\bin\sherpa-onnx-c-api.dll" },
    [pscustomobject]@{ Source = "$buildRoot\onnxruntime.dll"; Destination = "$liveRoot\bin\onnxruntime.dll" },
    [pscustomobject]@{ Source = "$buildRoot\onnxruntime_providers_shared.dll"; Destination = "$liveRoot\bin\onnxruntime_providers_shared.dll" },
    [pscustomobject]@{ Source = "$repoRoot\config\gateway.yml"; Destination = "$liveRoot\config\gateway.yml" },
    [pscustomobject]@{ Source = "$repoRoot\apps\inferdeck-gateway\static\index.html"; Destination = "$liveRoot\static\index.html" },
    [pscustomobject]@{ Source = "$repoRoot\apps\inferdeck-gateway\static\assets\index-BqY_Bxxq.css"; Destination = "$liveRoot\static\assets\index-BqY_Bxxq.css" },
    [pscustomobject]@{ Source = "$repoRoot\apps\inferdeck-gateway\static\assets\index-Cp4lx3xz.js"; Destination = "$liveRoot\static\assets\index-Cp4lx3xz.js" },
    [pscustomobject]@{ Source = "$repoRoot\apps\inferdeck-gateway\static\index.html"; Destination = "$liveRoot\bin\static\index.html" },
    [pscustomobject]@{ Source = "$repoRoot\apps\inferdeck-gateway\static\assets\index-BqY_Bxxq.css"; Destination = "$liveRoot\bin\static\assets\index-BqY_Bxxq.css" },
    [pscustomobject]@{ Source = "$repoRoot\apps\inferdeck-gateway\static\assets\index-Cp4lx3xz.js"; Destination = "$liveRoot\bin\static\assets\index-Cp4lx3xz.js" }
)
foreach ($copy in $buildCopies) {
    Assert-File $copy.Source
}

$staleTargets = @(
    "$liveRoot\static\assets\index-CbYcjiA3.js",
    "$liveRoot\bin\static\assets\index-8ZIS3MYD.css",
    "$liveRoot\bin\static\assets\index-B45EBCSg.css",
    "$liveRoot\bin\static\assets\index-B4EZ0xRA.js",
    "$liveRoot\bin\static\assets\index-sjsdnA4m.js"
)
$newTargets = @(
    "$liveRoot\bin\whisper.dll",
    "$liveRoot\bin\sherpa-onnx-c-api.dll",
    "$liveRoot\bin\onnxruntime.dll",
    "$liveRoot\bin\onnxruntime_providers_shared.dll",
    "$liveRoot\static\assets\index-Cp4lx3xz.js",
    "$liveRoot\bin\static\assets\index-BqY_Bxxq.css",
    "$liveRoot\bin\static\assets\index-Cp4lx3xz.js"
)
foreach ($target in $newTargets) {
    if (Test-Path -LiteralPath $target) {
        throw "Expected new deployment target to be absent: $target"
    }
}

$rootSherpaDependencies = @(
    "sherpa-onnx-c-api.dll",
    "onnxruntime.dll",
    "onnxruntime_providers_shared.dll"
)
foreach ($name in $rootSherpaDependencies) {
    $current = "$liveRoot\$name"
    $source = "$buildRoot\$name"
    Assert-File $current
    Assert-File $source
    if ((Get-FileHash -LiteralPath $current -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash) {
        throw "Active root dependency does not match tested release: $current"
    }
}

$swap = Invoke-RestMethod -Uri "http://127.0.0.1:11434/v1/swap/status" -TimeoutSec 10
if ($swap.swapping -eq $true -or [int]$swap.active_requests -ne 0) {
    throw "InferDeck is busy; refusing live replacement"
}

try {
    $deploymentStarted = $true
    Stop-InferDeck

    foreach ($copy in $buildCopies) {
        Copy-Verified $copy.Source $copy.Destination
    }
    [IO.File]::WriteAllText($activeConfig, $activeProfileText, [Text.UTF8Encoding]::new($false))
    if ([IO.File]::ReadAllText($activeConfig) -ne $activeProfileText) {
        throw "Active profile write verification failed"
    }
    foreach ($target in $staleTargets) {
        Assert-File $target
        Remove-Item -LiteralPath $target -Force
    }

    Start-InferDeck
    $health = Wait-ForHealth
    $models = Invoke-RestMethod -Uri "http://127.0.0.1:11434/v1/models" -TimeoutSec 15
    $gemma = $models.data | Where-Object { $_.id -eq "gemma-4-31b" }
    $whisper = $models.data | Where-Object { $_.id -eq "whisper-base-en" }
    $supertonic = $models.data | Where-Object { $_.id -eq "supertonic-3" }
    if ($null -eq $gemma -or [int64]$gemma.context_length -ne 262144) {
        throw "Gemma full context validation failed"
    }
    if ($null -eq $whisper -or $whisper.runtime_available -ne $true) {
        throw "Whisper runtime validation failed"
    }
    if ($null -eq $supertonic -or $supertonic.runtime_available -ne $true) {
        throw "Supertonic runtime validation failed"
    }
    $dashboard = Invoke-WebRequest -UseBasicParsing -Uri "http://127.0.0.1:11434/" -TimeoutSec 15
    if ($dashboard.Content -notmatch "index-Cp4lx3xz.js") {
        throw "Dashboard bundle validation failed"
    }

    Write-Result @{
        status = "deployed"
        timestamp_utc = [DateTime]::UtcNow.ToString("o")
        service = (Get-Service -Name $serviceName).Status.ToString()
        health_ok = $health.ok
        gemma_context_length = [int64]$gemma.context_length
        whisper_runtime_available = [bool]$whisper.runtime_available
        supertonic_runtime_available = [bool]$supertonic.runtime_available
        backup = $backupRoot
    }
} catch {
    $failure = $_.Exception.Message
    if ($deploymentStarted) {
        try {
            Stop-InferDeck
            foreach ($target in $newTargets) {
                if (Test-Path -LiteralPath $target) {
                    Assert-File $target
                    Remove-Item -LiteralPath $target -Force
                }
            }
            foreach ($mapping in $backupMappings) {
                Copy-Verified $mapping.Backup $mapping.Current
            }
            Start-InferDeck
            Wait-ForHealth | Out-Null
            $rollbackSucceeded = $true
        } catch {
            $failure = "$failure; rollback failed: $($_.Exception.Message)"
        }
    }
    Write-Result @{
        status = "failed"
        timestamp_utc = [DateTime]::UtcNow.ToString("o")
        error = $failure
        rollback_succeeded = $rollbackSucceeded
        backup = $backupRoot
    }
    throw $failure
}
