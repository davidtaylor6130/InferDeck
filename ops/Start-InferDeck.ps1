$ErrorActionPreference = "Stop"
Set-StrictMode -Version 3.0

$root = "C:\InferDeck"
$exe = Join-Path $root "bin\gateway-service.exe"
$config = Join-Path $root "config\gateway.yml"
$logs = Join-Path $root "logs"
$lock = Join-Path $root "run\inferdeck-startup.lock"
$meta = Join-Path $logs "startup-task-meta.json"
$common = Join-Path $root "InferDeck-Lifecycle.Common.ps1"

if (!(Test-Path -LiteralPath $common -PathType Leaf)) {
    throw "Missing lifecycle helper: $common"
}
. $common

if (!(Test-Path -LiteralPath $exe -PathType Leaf)) {
    throw "Missing InferDeck executable: $exe"
}
if (!(Test-Path -LiteralPath $config -PathType Leaf)) {
    throw "Missing InferDeck configuration: $config"
}

New-Item -ItemType Directory -Force -Path $logs, (Split-Path $lock) | Out-Null

try {
    Wait-InferDeckHealth -ExpectedExecutable $exe -Attempts 1 | Out-Null
    exit 0
} catch {
    $listener = Get-InferDeckListenerIdentity
    if ($listener) {
        Assert-InferDeckListenerOwner -Identity $listener -ExpectedExecutable $exe
    }
}

$existing = Remove-InferDeckStaleLock -LockPath $lock -ExpectedExecutable $exe
if ($existing) {
    Wait-InferDeckHealth -ExpectedExecutable $exe -Attempts 120 | Out-Null
    exit 0
}

$listener = Get-InferDeckListenerIdentity
if ($listener) {
    Assert-InferDeckListenerOwner -Identity $listener -ExpectedExecutable $exe
    throw "The expected InferDeck executable already owns port 11434 but did not pass /v1/health."
}

Set-Location $root
$env:PATH = "$root\bin;$root;$env:PATH"
$stdout = Join-Path $logs "startup-task.out.log"
$stderr = Join-Path $logs "startup-task.err.log"
$child = Start-Process -FilePath $exe -ArgumentList @("-c", $config) -WorkingDirectory $root -WindowStyle Hidden -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
$childIdentity = $null
for ($attempt = 0; $attempt -lt 10 -and !$childIdentity; $attempt++) {
    $childIdentity = Get-InferDeckProcessIdentity -ProcessId $child.Id
    if (!$childIdentity -and !$child.HasExited) {
        Start-Sleep -Milliseconds 100
    }
}
if (!$childIdentity -or !$childIdentity.ExecutablePath -or !$childIdentity.CreationTimeUtc) {
    if (!$child.HasExited) {
        $child.Kill()
        $child.WaitForExit(5000) | Out-Null
    }
    throw "InferDeck started, but its process identity could not be verified."
}

Write-InferDeckLockIdentity -LockPath $lock -Identity $childIdentity
[pscustomobject]@{
    User = [System.Security.Principal.WindowsIdentity]::GetCurrent().Name
    LauncherProcessId = $PID
    Child = $childIdentity
    Root = $root
    Executable = $exe
    Config = $config
    StartedAtUtc = (Get-Date).ToUniversalTime().ToString("o")
} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $meta -Encoding UTF8

try {
    Wait-InferDeckHealth -ExpectedExecutable $exe -Attempts 120 | Out-Null
    exit 0
} catch {
    if ((Test-InferDeckProcessIdentity -Identity $childIdentity -ExpectedExecutable $exe) -and !$child.HasExited) {
        $child.Kill()
        $child.WaitForExit(5000) | Out-Null
    }
    $locked = Read-InferDeckLockIdentity -LockPath $lock
    if ($locked -and
        [int]$locked.ProcessId -eq [int]$childIdentity.ProcessId -and
        [string]$locked.CreationTimeUtc -eq [string]$childIdentity.CreationTimeUtc) {
        Remove-Item -LiteralPath $lock -Force -ErrorAction SilentlyContinue
    }
    throw
}
