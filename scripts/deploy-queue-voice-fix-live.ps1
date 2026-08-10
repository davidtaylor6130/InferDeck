param(
    [Parameter(Mandatory = $true)]
    [string]$RunId
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = "C:\Users\david\Documents\GitHub\InferDeck"
$buildRoot = "C:\tmp\idw\bin\Release"
$liveRoot = "C:\InferDeck"
$serviceName = "InferDeck"
$sourceStatic = "$buildRoot\static"
$resultPath = "$liveRoot\logs\queue-voice-deploy-$RunId.json"
$progressPath = "$liveRoot\logs\queue-voice-deploy-$RunId.log"
$backupRoot = "$liveRoot\backups\queue-voice-$RunId"
$deploymentStarted = $false
$rollbackSucceeded = $false

function Write-Progress([string]$Message) {
    $line = "{0} {1}" -f [DateTime]::UtcNow.ToString("o"), $Message
    [IO.File]::AppendAllText($progressPath, $line + [Environment]::NewLine,
        [Text.UTF8Encoding]::new($false))
}

function Write-Result([hashtable]$Value) {
    [IO.File]::WriteAllText(
        $resultPath,
        ($Value | ConvertTo-Json -Depth 10),
        [Text.UTF8Encoding]::new($false))
}

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

function Copy-Verified([string]$Source, [string]$Destination) {
    Assert-File $Source
    Copy-Item -LiteralPath $Source -Destination $Destination -Force
    $sourceHash = (Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash
    $destinationHash = (Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash
    if ($sourceHash -ne $destinationHash) {
        throw "Hash verification failed: $Destination"
    }
}

function Restore-Verified([string]$Backup, [string]$Destination) {
    Assert-File $Backup
    Assert-File $Destination
    $backupHash = (Get-FileHash -LiteralPath $Backup -Algorithm SHA256).Hash
    $destinationHash = (Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash
    if ($backupHash -ne $destinationHash) {
        Copy-Verified $Backup $Destination
    }
}

function Decode-Nssm([object[]]$Value) {
    return (($Value -join '') -replace "`0", '').Trim()
}

function Get-ListenerPid {
    $line = & "$env:SystemRoot\System32\netstat.exe" -ano -p tcp |
        Select-String -Pattern '^\s*TCP\s+\S+:11434\s+\S+\s+LISTENING\s+\d+\s*$' |
        Select-Object -First 1
    if ($null -eq $line) {
        return 0
    }
    return [int](($line.ToString() -split '\s+')[-1])
}

function Stop-InferDeck {
    $service = Get-Service -Name $serviceName
    $gatewayPid = Get-ListenerPid
    if ($service.Status -eq "Stopped" -and $gatewayPid -ne 0) {
        throw "Port 11434 is owned while the InferDeck service is stopped"
    }
    if ($service.Status -ne "Stopped" -and $gatewayPid -ne 0) {
        $serviceInfo = Get-CimInstance Win32_Service -Filter "Name='$serviceName'"
        $gatewayInfo = Get-CimInstance Win32_Process -Filter "ProcessId=$gatewayPid"
        if ($null -eq $gatewayInfo -or $gatewayInfo.ParentProcessId -ne $serviceInfo.ProcessId) {
            throw "Port 11434 is not owned by the InferDeck service child"
        }
    }
    if ($service.Status -ne "Stopped") {
        Stop-Service -Name $serviceName -Force
        $service.WaitForStatus("Stopped", [TimeSpan]::FromSeconds(180))
    }
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    while ($gatewayPid -ne 0 -and
           (Get-Process -Id $gatewayPid -ErrorAction SilentlyContinue) -and
           [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 250
    }
    if ($gatewayPid -ne 0 -and
        (Get-Process -Id $gatewayPid -ErrorAction SilentlyContinue)) {
        Stop-Process -Id $gatewayPid -Force
        Wait-Process -Id $gatewayPid -Timeout 15 -ErrorAction SilentlyContinue
    }
    if ((Get-ListenerPid) -ne 0) {
        throw "Port 11434 remained active after stopping InferDeck"
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
    $deadline = [DateTime]::UtcNow.AddSeconds(240)
    while ([DateTime]::UtcNow -lt $deadline) {
        try {
            $health = Invoke-RestMethod -Uri "http://127.0.0.1:11434/v1/health" -TimeoutSec 5
            if ($health.ok -eq $true) {
                return $health
            }
        } catch {
        }
        Start-Sleep -Milliseconds 500
    }
    throw "InferDeck health endpoint did not become ready"
}

function Invoke-Transcription {
    Add-Type -AssemblyName System.Net.Http
    $client = [Net.Http.HttpClient]::new()
    $multipart = [Net.Http.MultipartFormDataContent]::new()
    try {
        $audioPath = "$repoRoot\libs\third_party\llama.cpp\tools\mtmd\test-2.mp3"
        Assert-File $audioPath
        $audio = [Net.Http.ByteArrayContent]::new([IO.File]::ReadAllBytes($audioPath))
        $audio.Headers.ContentType = [Net.Http.Headers.MediaTypeHeaderValue]::new("audio/mpeg")
        $multipart.Add($audio, "file", "test-2.mp3")
        $multipart.Add([Net.Http.StringContent]::new("whisper-base-en"), "model")
        $multipart.Add([Net.Http.StringContent]::new("en"), "language")
        $response = $client.PostAsync(
            "http://127.0.0.1:11434/v1/audio/transcriptions", $multipart).GetAwaiter().GetResult()
        $body = $response.Content.ReadAsStringAsync().GetAwaiter().GetResult()
        if (-not $response.IsSuccessStatusCode) {
            throw "Live transcription failed with HTTP $([int]$response.StatusCode): $body"
        }
        $json = $body | ConvertFrom-Json
        if ([string]::IsNullOrWhiteSpace([string]$json.text)) {
            throw "Live transcription returned no text"
        }
        return [string]$json.text
    } finally {
        $multipart.Dispose()
        $client.Dispose()
    }
}

function Invoke-Speech {
    Add-Type -AssemblyName System.Net.Http
    $client = [Net.Http.HttpClient]::new()
    try {
        $payload = @{
            model = "supertonic-3"
            input = "InferDeck deployment ready."
            voice = "default"
            response_format = "wav"
        } | ConvertTo-Json -Compress
        $content = [Net.Http.StringContent]::new(
            $payload, [Text.Encoding]::UTF8, "application/json")
        $response = $client.PostAsync(
            "http://127.0.0.1:11434/v1/audio/speech", $content).GetAwaiter().GetResult()
        $bytes = $response.Content.ReadAsByteArrayAsync().GetAwaiter().GetResult()
        if (-not $response.IsSuccessStatusCode) {
            $body = [Text.Encoding]::UTF8.GetString($bytes)
            throw "Live speech failed with HTTP $([int]$response.StatusCode): $body"
        }
        if ($bytes.Length -le 44) {
            throw "Live speech returned an invalid WAV payload"
        }
        return $bytes.Length
    } finally {
        $client.Dispose()
    }
}

Assert-Admin
Assert-Directory $repoRoot
Assert-Directory $buildRoot
Assert-Directory $liveRoot
Assert-Directory "$liveRoot\bin"
Assert-Directory "$liveRoot\config"
Assert-Directory "$liveRoot\logs"
Assert-Directory "$liveRoot\static"
Assert-Directory "$liveRoot\static\assets"
Assert-Directory "$liveRoot\bin\static"
Assert-Directory "$liveRoot\bin\static\assets"
Assert-Directory $sourceStatic
Assert-Directory "$sourceStatic\assets"

if (Test-Path -LiteralPath $resultPath) {
    throw "Result path already exists: $resultPath"
}
if (Test-Path -LiteralPath $progressPath) {
    throw "Progress path already exists: $progressPath"
}
if (Test-Path -LiteralPath $backupRoot) {
    throw "Backup path already exists: $backupRoot"
}

$nssmApplication = Decode-Nssm (& "$liveRoot\nssm.exe" get $serviceName Application)
$nssmParameters = Decode-Nssm (& "$liveRoot\nssm.exe" get $serviceName AppParameters)
$nssmDirectory = Decode-Nssm (& "$liveRoot\nssm.exe" get $serviceName AppDirectory)
if ($nssmApplication -ne "$liveRoot\inferdeck-gateway.exe") {
    throw "Unexpected NSSM application: $nssmApplication"
}
if ($nssmParameters -ne "-c config\gateway.yml") {
    throw "Unexpected NSSM parameters: $nssmParameters"
}
if ($nssmDirectory -ne $liveRoot) {
    throw "Unexpected NSSM directory: $nssmDirectory"
}

$runtimeFiles = @(
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
foreach ($name in $runtimeFiles) {
    $source = "$buildRoot\$name"
    $rootTarget = "$liveRoot\$name"
    $binTarget = "$liveRoot\bin\$name"
    Assert-File $source
    Assert-File $rootTarget
    Assert-File $binTarget
    $sourceHash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
    if ($sourceHash -ne (Get-FileHash -LiteralPath $rootTarget -Algorithm SHA256).Hash -or
        $sourceHash -ne (Get-FileHash -LiteralPath $binTarget -Algorithm SHA256).Hash) {
        throw "Installed runtime dependency does not match the verified build: $name"
    }
}

$sourceFiles = @(
    "$buildRoot\inferdeck-gateway.exe",
    "$repoRoot\config\gateway.yml",
    "$sourceStatic\index.html",
    "$sourceStatic\assets\index-BqY_Bxxq.css",
    "$sourceStatic\assets\index-Cp4lx3xz.js"
)
$liveFiles = @(
    "$liveRoot\inferdeck-gateway.exe",
    "$liveRoot\bin\gateway-service.exe",
    "$liveRoot\config\gateway.yml",
    "$liveRoot\static\index.html",
    "$liveRoot\static\assets\index-BqY_Bxxq.css",
    "$liveRoot\static\assets\index-Cp4lx3xz.js",
    "$liveRoot\bin\static\index.html",
    "$liveRoot\bin\static\assets\index-BqY_Bxxq.css",
    "$liveRoot\bin\static\assets\index-Cp4lx3xz.js"
)
foreach ($path in $sourceFiles + $liveFiles) {
    Assert-File $path
}

$expectedStatic = @(
    "index.html",
    "assets\index-BqY_Bxxq.css",
    "assets\index-Cp4lx3xz.js"
)
foreach ($staticRoot in @($sourceStatic, "$liveRoot\static", "$liveRoot\bin\static")) {
    $actual = @(Get-ChildItem -LiteralPath $staticRoot -Recurse -File | ForEach-Object {
        $_.FullName.Substring($staticRoot.Length).TrimStart('\')
    } | Sort-Object)
    $expected = @($expectedStatic | Sort-Object)
    if (($actual -join "|") -ne ($expected -join "|")) {
        throw "Unexpected static asset set under $staticRoot`: $($actual -join ', ')"
    }
}

New-Item -ItemType Directory -Path $backupRoot | Out-Null
New-Item -ItemType Directory -Path "$backupRoot\root-static\assets" | Out-Null
New-Item -ItemType Directory -Path "$backupRoot\bin-static\assets" | Out-Null
Write-Progress "validated deployment inputs"

$backupMappings = @(
    [pscustomobject]@{ Current = "$liveRoot\inferdeck-gateway.exe"; Backup = "$backupRoot\inferdeck-gateway.exe" },
    [pscustomobject]@{ Current = "$liveRoot\bin\gateway-service.exe"; Backup = "$backupRoot\gateway-service.exe" },
    [pscustomobject]@{ Current = "$liveRoot\config\gateway.yml"; Backup = "$backupRoot\gateway.yml" },
    [pscustomobject]@{ Current = "$liveRoot\static\index.html"; Backup = "$backupRoot\root-static\index.html" },
    [pscustomobject]@{ Current = "$liveRoot\static\assets\index-BqY_Bxxq.css"; Backup = "$backupRoot\root-static\assets\index-BqY_Bxxq.css" },
    [pscustomobject]@{ Current = "$liveRoot\static\assets\index-Cp4lx3xz.js"; Backup = "$backupRoot\root-static\assets\index-Cp4lx3xz.js" },
    [pscustomobject]@{ Current = "$liveRoot\bin\static\index.html"; Backup = "$backupRoot\bin-static\index.html" },
    [pscustomobject]@{ Current = "$liveRoot\bin\static\assets\index-BqY_Bxxq.css"; Backup = "$backupRoot\bin-static\assets\index-BqY_Bxxq.css" },
    [pscustomobject]@{ Current = "$liveRoot\bin\static\assets\index-Cp4lx3xz.js"; Backup = "$backupRoot\bin-static\assets\index-Cp4lx3xz.js" }
)
foreach ($mapping in $backupMappings) {
    Copy-Verified $mapping.Current $mapping.Backup
}
Write-Progress "backed up live deployment"

try {
    $deploymentStarted = $true
    Stop-InferDeck
    Write-Progress "stopped InferDeck"

    Copy-Verified "$buildRoot\inferdeck-gateway.exe" "$liveRoot\inferdeck-gateway.exe"
    Copy-Verified "$buildRoot\inferdeck-gateway.exe" "$liveRoot\bin\gateway-service.exe"
    Copy-Verified "$repoRoot\config\gateway.yml" "$liveRoot\config\gateway.yml"
    foreach ($relative in $expectedStatic) {
        Copy-Verified "$sourceStatic\$relative" "$liveRoot\static\$relative"
        Copy-Verified "$sourceStatic\$relative" "$liveRoot\bin\static\$relative"
    }
    Write-Progress "installed executable config and static assets"

    Start-InferDeck
    $health = Wait-ForHealth
    Write-Progress "health check passed"

    $models = Invoke-RestMethod -Uri "http://127.0.0.1:11434/v1/models" -TimeoutSec 15
    $gemma = $models.data | Where-Object { $_.id -eq "gemma-4-31b" }
    $qwen = $models.data | Where-Object { $_.id -eq "qwen3.6-27b" }
    $whisper = $models.data | Where-Object { $_.id -eq "whisper-base-en" }
    $supertonic = $models.data | Where-Object { $_.id -eq "supertonic-3" }
    if ($null -eq $gemma -or $null -eq $qwen -or $null -eq $whisper -or $null -eq $supertonic) {
        throw "Required deployed models are missing"
    }
    if ($whisper.runtime_available -ne $true -or $supertonic.runtime_available -ne $true) {
        throw "A deployed voice runtime is unavailable"
    }

    $transcript = Invoke-Transcription
    Write-Progress "live transcription passed"
    $speechBytes = Invoke-Speech
    Write-Progress "live speech passed"

    $status = Invoke-RestMethod -Uri "http://127.0.0.1:11434/v1/swap/status" -TimeoutSec 15
    $gemmaResidency = $status.residency | Where-Object { $_.name -eq "gemma-4-31b" }
    $whisperResidency = $status.residency | Where-Object { $_.name -eq "whisper-base-en" }
    $speechResidency = $status.residency | Where-Object { $_.name -eq "supertonic-3" }
    if ($status.loaded_model -ne "gemma-4-31b" -or $gemmaResidency.primary -ne $true) {
        throw "Gemma was not preserved as the primary GPU model"
    }
    if ([int]$whisperResidency.estimated_vram_mb -ne 0 -or $whisperResidency.primary -eq $true) {
        throw "Whisper residency was incorrectly classified as GPU-primary"
    }
    if ([int]$speechResidency.estimated_vram_mb -ne 0 -or $speechResidency.primary -eq $true) {
        throw "Speech residency was incorrectly classified as GPU-primary"
    }
    $dashboard = Invoke-WebRequest -UseBasicParsing -Uri "http://127.0.0.1:11434/" -TimeoutSec 15
    if ($dashboard.Content -notmatch "index-Cp4lx3xz.js") {
        throw "Dashboard bundle verification failed"
    }

    $liveHash = (Get-FileHash -LiteralPath "$liveRoot\inferdeck-gateway.exe" -Algorithm SHA256).Hash
    $sourceHash = (Get-FileHash -LiteralPath "$buildRoot\inferdeck-gateway.exe" -Algorithm SHA256).Hash
    Write-Result @{
        status = "deployed"
        timestamp_utc = [DateTime]::UtcNow.ToString("o")
        service = (Get-Service -Name $serviceName).Status.ToString()
        health_ok = [bool]$health.ok
        source_hash = $sourceHash
        live_hash = $liveHash
        loaded_model = [string]$status.loaded_model
        loaded_models = @($status.loaded_models)
        active_requests = [int]$status.active_requests
        whisper_vram_mb = [int]$whisperResidency.estimated_vram_mb
        speech_vram_mb = [int]$speechResidency.estimated_vram_mb
        transcript = $transcript
        speech_bytes = [int]$speechBytes
        backup = $backupRoot
        progress_log = $progressPath
    }
    Write-Progress "deployment completed"
} catch {
    $failure = $_.Exception.Message
    if ($deploymentStarted) {
        try {
            Stop-InferDeck
            foreach ($mapping in $backupMappings) {
                Restore-Verified $mapping.Backup $mapping.Current
            }
            Start-InferDeck
            Wait-ForHealth | Out-Null
            $rollbackSucceeded = $true
            Write-Progress "rollback completed"
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
        progress_log = $progressPath
    }
    throw $failure
}
