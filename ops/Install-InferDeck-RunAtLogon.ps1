$ErrorActionPreference = "Stop"
Set-StrictMode -Version 3.0

$root = "C:\InferDeck"
$repo = "C:\Users\david\Documents\GitHub\InferDeck"
$logs = Join-Path $root "logs"
$startupScript = Join-Path $root "Start-InferDeck.ps1"
$expectedExecutable = Join-Path $root "bin\gateway-service.exe"
$common = Join-Path $repo "ops\InferDeck-Lifecycle.Common.ps1"

if (!(Test-Path -LiteralPath $common -PathType Leaf)) {
    throw "Missing lifecycle helper: $common"
}
. $common

Assert-NoInferDeckWindowsService
New-Item -ItemType Directory -Force -Path $root, $logs | Out-Null
$configExisted = Test-Path -LiteralPath (Join-Path $root "config\gateway.yml") -PathType Leaf
$stage = New-InferDeckDeploymentStage -Root $root -Repository $repo

foreach ($taskName in @("InferDeck Gateway Logon", "InferDeck Gateway Watchdog")) {
    $task = Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
    if ($task) {
        Stop-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
        Unregister-ScheduledTask -TaskName $taskName -Confirm:$false
    }
}

$deployment = Publish-InferDeckDeployment -Root $root -Stage $stage
Install-InferDeckStartupFiles -Root $root -Repository $repo
Remove-InferDeckStaleLock -LockPath (Join-Path $root "run\inferdeck-startup.lock") -ExpectedExecutable $expectedExecutable | Out-Null

$runCommand = "powershell.exe -NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$startupScript`""
New-Item -Path "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run" -Force | Out-Null
Set-ItemProperty -Path "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run" -Name "InferDeck Gateway" -Value $runCommand
$registeredCommand = (Get-ItemProperty -Path "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run" -Name "InferDeck Gateway")."InferDeck Gateway"
if ($registeredCommand -ne $runCommand) {
    throw "The InferDeck HKCU Run command could not be verified."
}

Start-Process -FilePath "powershell.exe" -ArgumentList "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$startupScript`"" -WindowStyle Hidden
$verified = Wait-InferDeckHealth -ExpectedExecutable $expectedExecutable -Attempts 120
[pscustomobject]@{
    Healthy = $verified.Health.ok
    ListenerProcessId = $verified.Owner.ProcessId
    ListenerExecutable = $verified.Owner.ExecutablePath
    Startup = "HKCU Run"
    Command = $runCommand
    ConfigPreserved = $configExisted
    RollbackBin = $deployment.RollbackBin
} | ConvertTo-Json | Tee-Object -FilePath (Join-Path $logs "run-at-logon-result.json")
