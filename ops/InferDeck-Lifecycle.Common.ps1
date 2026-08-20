Set-StrictMode -Version 3.0

function ConvertTo-InferDeckPath {
    param(
        [Parameter(Mandatory)]
        [string]$Path
    )

    return [System.IO.Path]::GetFullPath($Path).TrimEnd('\').ToLowerInvariant()
}

function Get-InferDeckProcessIdentity {
    param(
        [Parameter(Mandatory)]
        [int]$ProcessId
    )

    $process = Get-CimInstance Win32_Process -Filter "ProcessId = $ProcessId" -ErrorAction SilentlyContinue
    if (!$process) {
        return $null
    }

    $creationTime = if ($process.CreationDate) {
        ([datetime]$process.CreationDate).ToUniversalTime().ToString("o")
    } else {
        $null
    }

    [pscustomobject]@{
        ProcessId = [int]$process.ProcessId
        ExecutablePath = [string]$process.ExecutablePath
        CreationTimeUtc = $creationTime
    }
}

function Test-InferDeckProcessIdentity {
    param(
        [Parameter(Mandatory)]
        [psobject]$Identity,

        [Parameter(Mandatory)]
        [string]$ExpectedExecutable
    )

    $propertyNames = @($Identity.PSObject.Properties.Name)
    if ($propertyNames -notcontains "ProcessId" -or
        $propertyNames -notcontains "ExecutablePath" -or
        $propertyNames -notcontains "CreationTimeUtc" -or
        !$Identity.ProcessId -or
        !$Identity.ExecutablePath -or
        !$Identity.CreationTimeUtc) {
        return $false
    }

    $current = Get-InferDeckProcessIdentity -ProcessId ([int]$Identity.ProcessId)
    if (!$current -or !$current.ExecutablePath -or !$current.CreationTimeUtc) {
        return $false
    }

    return (
        (ConvertTo-InferDeckPath $current.ExecutablePath) -eq (ConvertTo-InferDeckPath $ExpectedExecutable) -and
        $current.CreationTimeUtc -eq [string]$Identity.CreationTimeUtc
    )
}

function Get-InferDeckListenerIdentity {
    param(
        [int]$Port = 11434
    )

    $ownerIds = @(
        Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue |
            Select-Object -ExpandProperty OwningProcess -Unique
    )
    if ($ownerIds.Count -eq 0) {
        return $null
    }
    if ($ownerIds.Count -ne 1) {
        throw "Port $Port has multiple listener owners: $($ownerIds -join ', ')."
    }

    $identity = Get-InferDeckProcessIdentity -ProcessId ([int]$ownerIds[0])
    if (!$identity -or !$identity.ExecutablePath) {
        throw "Cannot verify the executable that owns port $Port (PID $($ownerIds[0]))."
    }
    return $identity
}

function Assert-InferDeckListenerOwner {
    param(
        [Parameter(Mandatory)]
        [psobject]$Identity,

        [Parameter(Mandatory)]
        [string]$ExpectedExecutable,

        [int]$Port = 11434
    )

    if ((ConvertTo-InferDeckPath $Identity.ExecutablePath) -ne (ConvertTo-InferDeckPath $ExpectedExecutable)) {
        throw "Port $Port is owned by '$($Identity.ExecutablePath)' (PID $($Identity.ProcessId)), not '$ExpectedExecutable'."
    }
}

function Wait-InferDeckHealth {
    param(
        [Parameter(Mandatory)]
        [string]$ExpectedExecutable,

        [int]$Attempts = 20,

        [int]$DelayMilliseconds = 1000,

        [int]$Port = 11434
    )

    $healthUri = "http://127.0.0.1:$Port/api/inferdeck/v1/health"
    $lastFailure = "No response."
    for ($attempt = 0; $attempt -lt $Attempts; $attempt++) {
        try {
            $health = Invoke-RestMethod -Uri $healthUri -TimeoutSec 1
            $okProperty = $health.PSObject.Properties["ok"]
            if ($okProperty -and $okProperty.Value -is [bool] -and $okProperty.Value) {
                $owner = Get-InferDeckListenerIdentity -Port $Port
                if (!$owner) {
                    throw "The health endpoint responded, but no process owns port $Port."
                }
                Assert-InferDeckListenerOwner -Identity $owner -ExpectedExecutable $ExpectedExecutable -Port $Port
                return [pscustomobject]@{
                    Health = $health
                    Owner = $owner
                }
            }
            $reportedOk = if ($okProperty) { $okProperty.Value } else { "<missing>" }
            $lastFailure = "The health endpoint returned ok=$reportedOk."
        } catch {
            $lastFailure = $_.Exception.Message
        }

        $listener = Get-InferDeckListenerIdentity -Port $Port
        if ($listener) {
            Assert-InferDeckListenerOwner -Identity $listener -ExpectedExecutable $ExpectedExecutable -Port $Port
        }
        if ($attempt + 1 -lt $Attempts) {
            Start-Sleep -Milliseconds $DelayMilliseconds
        }
    }

    throw "InferDeck did not become healthy at $healthUri. Last failure: $lastFailure"
}

function Read-InferDeckLockIdentity {
    param(
        [Parameter(Mandatory)]
        [string]$LockPath
    )

    if (!(Test-Path -LiteralPath $LockPath -PathType Leaf)) {
        return $null
    }

    try {
        return Get-Content -LiteralPath $LockPath -Raw | ConvertFrom-Json
    } catch {
        return $null
    }
}

function Write-InferDeckLockIdentity {
    param(
        [Parameter(Mandatory)]
        [string]$LockPath,

        [Parameter(Mandatory)]
        [psobject]$Identity
    )

    $Identity |
        Select-Object ProcessId, ExecutablePath, CreationTimeUtc |
        ConvertTo-Json |
        Set-Content -LiteralPath $LockPath -Encoding UTF8
}

function Remove-InferDeckStaleLock {
    param(
        [Parameter(Mandatory)]
        [string]$LockPath,

        [Parameter(Mandatory)]
        [string]$ExpectedExecutable
    )

    $identity = Read-InferDeckLockIdentity -LockPath $LockPath
    if ($identity -and (Test-InferDeckProcessIdentity -Identity $identity -ExpectedExecutable $ExpectedExecutable)) {
        return $identity
    }

    if (Test-Path -LiteralPath $LockPath) {
        Remove-Item -LiteralPath $LockPath -Force
    }
    return $null
}

function Stop-InferDeckLiveOwner {
    param(
        [Parameter(Mandatory)]
        [string]$ExpectedExecutable,

        [int]$Port = 11434,

        [int]$Attempts = 40
    )

    $owner = Get-InferDeckListenerIdentity -Port $Port
    if ($owner) {
        Assert-InferDeckListenerOwner -Identity $owner -ExpectedExecutable $ExpectedExecutable -Port $Port
    }

    $processes = @(
        Get-CimInstance Win32_Process -ErrorAction Stop |
            Where-Object {
                $_.ExecutablePath -and
                (ConvertTo-InferDeckPath $_.ExecutablePath) -eq (ConvertTo-InferDeckPath $ExpectedExecutable)
            } |
            ForEach-Object {
                Get-InferDeckProcessIdentity -ProcessId ([int]$_.ProcessId)
            }
    )
    $knownProcessIds = @($processes | ForEach-Object { $_.ProcessId })
    if ($owner -and $knownProcessIds -notcontains $owner.ProcessId) {
        $processes += $owner
    }

    foreach ($processIdentity in $processes) {
        $current = Get-InferDeckProcessIdentity -ProcessId ([int]$processIdentity.ProcessId)
        if (!$current) {
            continue
        }
        if (!(Test-InferDeckProcessIdentity -Identity $processIdentity -ExpectedExecutable $ExpectedExecutable)) {
            throw "The process identity for PID $($processIdentity.ProcessId) changed before it could be stopped."
        }
        Stop-Process -Id ([int]$processIdentity.ProcessId) -ErrorAction Stop
    }

    for ($attempt = 0; $attempt -lt $Attempts; $attempt++) {
        $remaining = @(
            $processes | Where-Object {
                Get-InferDeckProcessIdentity -ProcessId ([int]$_.ProcessId)
            }
        )
        $listener = Get-InferDeckListenerIdentity -Port $Port
        if ($remaining.Count -eq 0 -and !$listener) {
            return
        }
        foreach ($processIdentity in $remaining) {
            if (!(Test-InferDeckProcessIdentity -Identity $processIdentity -ExpectedExecutable $ExpectedExecutable)) {
                throw "PID $($processIdentity.ProcessId) was reused while waiting for InferDeck to stop."
            }
        }
        if ($listener) {
            Assert-InferDeckListenerOwner -Identity $listener -ExpectedExecutable $ExpectedExecutable -Port $Port
        }
        Start-Sleep -Milliseconds 250
    }

    $remainingIds = @($processes | ForEach-Object { $_.ProcessId }) -join ", "
    throw "The verified InferDeck process set ($remainingIds) did not release port $Port."
}

function Get-InferDeckWindowsServices {
    $services = foreach ($serviceName in @("InferDeck", "InferDeckGateway")) {
        Get-CimInstance Win32_Service -Filter "Name = '$serviceName'" -ErrorAction SilentlyContinue
    }
    return @($services)
}

function Assert-NoInferDeckWindowsService {
    $services = @(Get-InferDeckWindowsServices)
    if ($services.Count -eq 0) {
        return
    }

    $details = $services | ForEach-Object {
        "$($_.Name) (account=$($_.StartName), state=$($_.State), path=$($_.PathName))"
    }
    throw "An InferDeck Windows service deployment already exists: $($details -join '; '). Refusing to migrate it implicitly to an interactive logon deployment."
}

function New-InferDeckDeploymentStage {
    param(
        [Parameter(Mandatory)]
        [string]$Root,

        [Parameter(Mandatory)]
        [string]$Repository
    )

    $sourceBin = Join-Path $Repository "build\bin\Release"
    $sourceExe = Join-Path $sourceBin "inferdeck-gateway.exe"
    $sourceStatic = Join-Path $Repository "apps\inferdeck-gateway\static"
    $sourceConfig = Join-Path $Repository "config\gateway.yml"
    $sourceStartup = Join-Path $Repository "ops\Start-InferDeck.ps1"
    $sourceCommon = Join-Path $Repository "ops\InferDeck-Lifecycle.Common.ps1"
    if (!(Test-Path -LiteralPath $sourceExe -PathType Leaf)) {
        throw "Missing gateway build artifact: $sourceExe"
    }
    if (!(Test-Path -LiteralPath (Join-Path $sourceStatic "index.html") -PathType Leaf)) {
        throw "Missing dashboard artifact: $(Join-Path $sourceStatic 'index.html')"
    }
    if (!(Test-Path -LiteralPath (Join-Path $sourceStatic "assets") -PathType Container) -or
        @(Get-ChildItem -LiteralPath (Join-Path $sourceStatic "assets") -File).Count -eq 0) {
        throw "Missing dashboard asset bundle: $(Join-Path $sourceStatic 'assets')"
    }
    foreach ($requiredSource in @($sourceConfig, $sourceStartup, $sourceCommon)) {
        if (!(Test-Path -LiteralPath $requiredSource -PathType Leaf)) {
            throw "Missing deployment input: $requiredSource"
        }
    }

    $runDir = Join-Path $Root "run"
    New-Item -ItemType Directory -Force -Path $runDir | Out-Null
    $stageRoot = Join-Path $runDir ("deploy-" + [guid]::NewGuid().ToString("N"))
    $stageBin = Join-Path $stageRoot "bin"
    New-Item -ItemType Directory -Force -Path $stageBin | Out-Null
    Get-ChildItem -LiteralPath $sourceBin -Force | Copy-Item -Destination $stageBin -Recurse -Force

    $stageStatic = Join-Path $stageBin "static"
    if (Test-Path -LiteralPath $stageStatic) {
        Remove-Item -LiteralPath $stageStatic -Recurse -Force
    }
    Copy-Item -LiteralPath $sourceStatic -Destination $stageStatic -Recurse -Force

    $stageExe = Join-Path $stageBin "gateway-service.exe"
    Copy-Item -LiteralPath $sourceExe -Destination $stageExe -Force
    if ((Get-FileHash -LiteralPath $sourceExe -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $stageExe -Algorithm SHA256).Hash) {
        throw "The staged gateway executable does not match the build artifact."
    }
    if (!(Test-Path -LiteralPath (Join-Path $stageStatic "index.html") -PathType Leaf)) {
        throw "The staged dashboard is incomplete."
    }

    [pscustomobject]@{
        Root = $stageRoot
        Bin = $stageBin
        Executable = $stageExe
    }
}

function Publish-InferDeckDeployment {
    param(
        [Parameter(Mandatory)]
        [string]$Root,

        [Parameter(Mandatory)]
        [psobject]$Stage
    )

    $bin = Join-Path $Root "bin"
    $expectedExecutable = Join-Path $bin "gateway-service.exe"
    Stop-InferDeckLiveOwner -ExpectedExecutable $expectedExecutable

    $rollbackRoot = Join-Path (Join-Path $Root "run") ("rollback-" + (Get-Date -Format "yyyyMMdd-HHmmss") + "-" + [guid]::NewGuid().ToString("N").Substring(0, 8))
    $rollbackBin = Join-Path $rollbackRoot "bin"
    New-Item -ItemType Directory -Force -Path $rollbackRoot | Out-Null

    $hadLiveBin = Test-Path -LiteralPath $bin
    try {
        if ($hadLiveBin) {
            Move-Item -LiteralPath $bin -Destination $rollbackBin
        }
        Move-Item -LiteralPath $Stage.Bin -Destination $bin
    } catch {
        if (!(Test-Path -LiteralPath $bin) -and (Test-Path -LiteralPath $rollbackBin)) {
            Move-Item -LiteralPath $rollbackBin -Destination $bin
        }
        throw
    }

    [pscustomobject]@{
        Executable = $expectedExecutable
        RollbackBin = if ($hadLiveBin) { $rollbackBin } else { $null }
    }
}

function Install-InferDeckStartupFiles {
    param(
        [Parameter(Mandatory)]
        [string]$Root,

        [Parameter(Mandatory)]
        [string]$Repository
    )

    $sourceStartup = Join-Path $Repository "ops\Start-InferDeck.ps1"
    $sourceCommon = Join-Path $Repository "ops\InferDeck-Lifecycle.Common.ps1"
    $destinationStartup = Join-Path $Root "Start-InferDeck.ps1"
    $destinationCommon = Join-Path $Root "InferDeck-Lifecycle.Common.ps1"
    Copy-Item -LiteralPath $sourceStartup -Destination $destinationStartup -Force
    Copy-Item -LiteralPath $sourceCommon -Destination $destinationCommon -Force
    foreach ($pair in @(
        @($sourceStartup, $destinationStartup),
        @($sourceCommon, $destinationCommon)
    )) {
        if ((Get-FileHash -LiteralPath $pair[0] -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $pair[1] -Algorithm SHA256).Hash) {
            throw "Installed startup file does not match its source: $($pair[1])"
        }
    }

    $configDir = Join-Path $Root "config"
    $configPath = Join-Path $configDir "gateway.yml"
    New-Item -ItemType Directory -Force -Path $configDir | Out-Null
    if (!(Test-Path -LiteralPath $configPath -PathType Leaf)) {
        Copy-Item -LiteralPath (Join-Path $Repository "config\gateway.yml") -Destination $configPath
    }
}

function Assert-InferDeckTaskAction {
    param(
        [Parameter(Mandatory)]
        [string]$TaskName,

        [Parameter(Mandatory)]
        [string]$StartupScript
    )

    $task = Get-ScheduledTask -TaskName $TaskName -ErrorAction Stop
    $actions = @($task.Actions)
    if ($actions.Count -ne 1 -or
        [string]$actions[0].Execute -notmatch '(?i)(^|\\)powershell(\.exe)?$' -or
        [string]$actions[0].Arguments -notlike "*$StartupScript*") {
        throw "Scheduled task '$TaskName' was registered with an unexpected action."
    }
    return $task
}
