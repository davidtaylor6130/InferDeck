param(
    [string]$BuildRoot = "C:\tmp\idw\bin\Release",
    [string]$LiveRoot = "C:\InferDeck",
    [string]$BackupRoot = "C:\InferDeck\backups\voice-mp3-20260810-2100",
    [switch]$CreateBackup,
    [switch]$AllowActiveRequests
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Assert-Admin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw "Administrator elevation is required"
    }
}

function Assert-Directory([string]$Path) {
    $item = Get-Item -LiteralPath $Path -Force
    if (-not $item.PSIsContainer -or
        (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
        throw "Unsafe directory: $Path"
    }
}

function Assert-File([string]$Path) {
    $item = Get-Item -LiteralPath $Path -Force
    if ($item.PSIsContainer -or
        (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
        throw "Unsafe file: $Path"
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

function Ensure-BackupDirectory([string]$Path, [string]$Root) {
    $resolvedPath = [IO.Path]::GetFullPath($Path)
    if ($resolvedPath -ne $Root -and $resolvedPath -notlike "$Root\*") {
        throw "Unexpected backup directory: $resolvedPath"
    }
    if (-not (Test-Path -LiteralPath $resolvedPath)) {
        New-Item -ItemType Directory -Path $resolvedPath | Out-Null
    }
    Assert-Directory $resolvedPath
}

function Wait-ForHealth {
    $deadline = [DateTime]::UtcNow.AddSeconds(120)
    $health = $null
    do {
        try {
            $health = Invoke-RestMethod -Uri "http://127.0.0.1:11434/v1/health" -TimeoutSec 5
            if ($null -ne $health -and $health.ok -eq $true) {
                return $health
            }
        } catch {
        }
        Start-Sleep -Milliseconds 500
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Live health endpoint did not recover"
}

function Wait-ForProcessExit([int]$ProcessId) {
    if ($ProcessId -le 0) {
        return
    }
    try {
        Wait-Process -Id $ProcessId -Timeout 180 -ErrorAction Stop
    } catch {
        if ($null -ne (Get-Process -Id $ProcessId -ErrorAction SilentlyContinue)) {
            throw "InferDeck process $ProcessId did not exit after service stop"
        }
    }
}

Assert-Admin

$resolvedLiveRoot = [IO.Path]::GetFullPath($LiveRoot)
$resolvedBuildRoot = [IO.Path]::GetFullPath($BuildRoot)
$resolvedBackupRoot = [IO.Path]::GetFullPath($BackupRoot)
if ($resolvedBackupRoot -notlike "$resolvedLiveRoot\backups\*") {
    throw "Unexpected backup path: $resolvedBackupRoot"
}

$resolvedBackupsRoot = "$resolvedLiveRoot\backups"

foreach ($directory in @(
    $resolvedLiveRoot,
    "$resolvedLiveRoot\bin",
    "$resolvedLiveRoot\static",
    "$resolvedLiveRoot\static\assets",
    "$resolvedLiveRoot\bin\static",
    "$resolvedLiveRoot\bin\static\assets",
    $resolvedBuildRoot,
    "$resolvedBuildRoot\static",
    "$resolvedBuildRoot\static\assets",
    $resolvedBackupsRoot
)) {
    Assert-Directory $directory
}


if ($CreateBackup) {
    if (Test-Path -LiteralPath $resolvedBackupRoot) {
        throw "Backup path already exists: $resolvedBackupRoot"
    }
    Ensure-BackupDirectory $resolvedBackupRoot $resolvedBackupRoot
} else {
    Assert-Directory $resolvedBackupRoot
}

$mappings = @(
    [pscustomobject]@{ Source = "$resolvedBuildRoot\inferdeck-gateway.exe"; Destination = "$resolvedLiveRoot\inferdeck-gateway.exe"; Backup = "root\inferdeck-gateway.exe" },
    [pscustomobject]@{ Source = "$resolvedBuildRoot\inferdeck-gateway.exe"; Destination = "$resolvedLiveRoot\bin\gateway-service.exe"; Backup = "bin\gateway-service.exe" }
)

$runtimeNames = @(
    "ggml.dll",
    "ggml-base.dll",
    "ggml-cpu.dll",
    "ggml-vulkan.dll",
    "llama.dll",
    "llama-common.dll",
    "mtmd.dll",
    "whisper.dll",
    "sherpa-onnx-c-api.dll",
    "onnxruntime.dll",
    "onnxruntime_providers_shared.dll"
)
foreach ($name in $runtimeNames) {
    $mappings += [pscustomobject]@{ Source = "$resolvedBuildRoot\$name"; Destination = "$resolvedLiveRoot\$name"; Backup = "root\$name" }
    $mappings += [pscustomobject]@{ Source = "$resolvedBuildRoot\$name"; Destination = "$resolvedLiveRoot\bin\$name"; Backup = "bin\$name" }
}

foreach ($name in @(
    "index.html",
    "assets\index-BqY_Bxxq.css",
    "assets\index-Cp4lx3xz.js"
)) {
    $mappings += [pscustomobject]@{ Source = "$resolvedBuildRoot\static\$name"; Destination = "$resolvedLiveRoot\static\$name"; Backup = "static-root\$name" }
    $mappings += [pscustomobject]@{ Source = "$resolvedBuildRoot\static\$name"; Destination = "$resolvedLiveRoot\bin\static\$name"; Backup = "static-bin\$name" }
}

foreach ($mapping in $mappings) {
    Assert-File $mapping.Source
    Assert-File $mapping.Destination
    $backupPath = Join-Path $resolvedBackupRoot $mapping.Backup
    if ($CreateBackup) {
        Ensure-BackupDirectory (Split-Path -Parent $backupPath) $resolvedBackupRoot
        Copy-Verified $mapping.Destination $backupPath
    } else {
        Assert-File $backupPath
        $currentHash = (Get-FileHash -LiteralPath $mapping.Destination -Algorithm SHA256).Hash
        $backupHash = (Get-FileHash -LiteralPath $backupPath -Algorithm SHA256).Hash
        if ($currentHash -ne $backupHash) {
            throw "Live file changed after backup: $($mapping.Destination)"
        }
    }
}

$service = Get-Service -Name "InferDeck"
if ($service.Status -eq "Stopped") {
    Start-Service -Name "InferDeck"
    $service.WaitForStatus("Running", [TimeSpan]::FromSeconds(60))
    Wait-ForHealth | Out-Null
}

$swap = Invoke-RestMethod -Uri "http://127.0.0.1:11434/v1/swap/status" -TimeoutSec 10
if ($swap.swapping -eq $true) {
    throw "InferDeck is swapping models"
}
if ([int]$swap.active_requests -ne 0 -and -not $AllowActiveRequests) {
    throw "InferDeck has active requests"
}

$replacementStarted = $false
$rollbackSucceeded = $false
try {
    $listener = Get-NetTCPConnection -LocalPort 11434 -State Listen -ErrorAction SilentlyContinue |
        Select-Object -First 1
    $gatewayProcessId = if ($null -ne $listener) { [int]$listener.OwningProcess } else { 0 }
    Stop-Service -Name "InferDeck" -Force
    (Get-Service -Name "InferDeck").WaitForStatus("Stopped", [TimeSpan]::FromSeconds(180))
    Wait-ForProcessExit $gatewayProcessId
    $replacementStarted = $true

    foreach ($mapping in $mappings) {
        Copy-Verified $mapping.Source $mapping.Destination
    }

    Start-Service -Name "InferDeck"
    (Get-Service -Name "InferDeck").WaitForStatus("Running", [TimeSpan]::FromSeconds(60))
    $health = Wait-ForHealth
    $models = Invoke-RestMethod -Uri "http://127.0.0.1:11434/v1/models" -TimeoutSec 15
    $whisper = $models.data | Where-Object { $_.id -eq "whisper-base-en" }
    $gemma = $models.data | Where-Object { $_.id -eq "gemma-4-31b" }
    if ($null -eq $whisper -or $whisper.runtime_available -ne $true) {
        throw "Whisper runtime validation failed"
    }
    if ($null -eq $gemma -or [int64]$gemma.context_length -ne 262144) {
        throw "Gemma full-context validation failed"
    }

    $result = [ordered]@{
        status = "deployed"
        timestamp_utc = [DateTime]::UtcNow.ToString("o")
        backup = $resolvedBackupRoot
        prior_active_requests = [int]$swap.active_requests
        service = (Get-Service -Name "InferDeck").Status.ToString()
        health_ok = [bool]$health.ok
        whisper_runtime_available = [bool]$whisper.runtime_available
        gemma_context_length = [int64]$gemma.context_length
        executable_sha256 = (Get-FileHash -LiteralPath "$resolvedLiveRoot\inferdeck-gateway.exe" -Algorithm SHA256).Hash
    }
    $result | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$resolvedBackupRoot\deploy-result.json" -Encoding UTF8
    $result
} catch {
    $failure = $_.Exception.Message
    if ($replacementStarted) {
        try {
            $service = Get-Service -Name "InferDeck"
            if ($service.Status -ne "Stopped") {
                $listener = Get-NetTCPConnection -LocalPort 11434 -State Listen -ErrorAction SilentlyContinue |
                    Select-Object -First 1
                $gatewayProcessId = if ($null -ne $listener) { [int]$listener.OwningProcess } else { 0 }
                Stop-Service -Name "InferDeck" -Force
                $service.WaitForStatus("Stopped", [TimeSpan]::FromSeconds(180))
                Wait-ForProcessExit $gatewayProcessId
            }
            foreach ($mapping in $mappings) {
                Copy-Verified (Join-Path $resolvedBackupRoot $mapping.Backup) $mapping.Destination
            }
            Start-Service -Name "InferDeck"
            (Get-Service -Name "InferDeck").WaitForStatus("Running", [TimeSpan]::FromSeconds(60))
            Wait-ForHealth | Out-Null
            $rollbackSucceeded = $true
        } catch {
            $failure = "$failure; rollback failed: $($_.Exception.Message)"
        }
    }
    [ordered]@{
        status = "failed"
        timestamp_utc = [DateTime]::UtcNow.ToString("o")
        backup = $resolvedBackupRoot
        error = $failure
        rollback_succeeded = $rollbackSucceeded
    } | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$resolvedBackupRoot\deploy-result.json" -Encoding UTF8
    throw $failure
}
