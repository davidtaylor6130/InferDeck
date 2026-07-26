$ErrorActionPreference = "Stop"
Set-StrictMode -Version 3.0

$root = "C:\InferDeck"
$repo = "C:\Users\david\Documents\GitHub\InferDeck"
$logs = Join-Path $root "logs"
$startupScript = Join-Path $root "Start-InferDeck.ps1"
$expectedExecutable = Join-Path $root "bin\gateway-service.exe"
$logonTaskName = "InferDeck Gateway Logon"
$watchdogTaskName = "InferDeck Gateway Watchdog"
$taskUser = [System.Security.Principal.WindowsIdentity]::GetCurrent().Name
$common = Join-Path $repo "ops\InferDeck-Lifecycle.Common.ps1"

if (!(Test-Path -LiteralPath $common -PathType Leaf)) {
    throw "Missing lifecycle helper: $common"
}
. $common

Assert-NoInferDeckWindowsService
New-Item -ItemType Directory -Force -Path $root, $logs | Out-Null
$configExisted = Test-Path -LiteralPath (Join-Path $root "config\gateway.yml") -PathType Leaf
$stage = New-InferDeckDeploymentStage -Root $root -Repository $repo

foreach ($taskName in @($logonTaskName, $watchdogTaskName)) {
    $task = Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
    if ($task) {
        Stop-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
        Unregister-ScheduledTask -TaskName $taskName -Confirm:$false
    }
}

$deployment = Publish-InferDeckDeployment -Root $root -Stage $stage
Install-InferDeckStartupFiles -Root $root -Repository $repo
Remove-InferDeckStaleLock -LockPath (Join-Path $root "run\inferdeck-startup.lock") -ExpectedExecutable $expectedExecutable | Out-Null

$runKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run"
if (Test-Path -LiteralPath $runKey) {
    Remove-ItemProperty -Path $runKey -Name "InferDeck Gateway" -ErrorAction SilentlyContinue
}

$action = New-ScheduledTaskAction -Execute "powershell.exe" -Argument "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$startupScript`""
$principal = New-ScheduledTaskPrincipal -UserId $taskUser -LogonType Interactive
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Minutes 5) -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1) -MultipleInstances IgnoreNew
$logonTrigger = New-ScheduledTaskTrigger -AtLogOn -User $taskUser
$logonTask = New-ScheduledTask -Action $action -Trigger $logonTrigger -Principal $principal -Settings $settings -Description "Starts InferDeck at interactive logon."
Register-ScheduledTask -TaskName $logonTaskName -InputObject $logonTask -Force | Out-Null

$watchdogTrigger = New-ScheduledTaskTrigger -Once -At ((Get-Date).AddMinutes(1)) -RepetitionInterval (New-TimeSpan -Minutes 15)
$watchdogTask = New-ScheduledTask -Action $action -Trigger $watchdogTrigger -Principal $principal -Settings $settings -Description "Checks InferDeck every 15 minutes and starts it only when it is not already healthy."
Register-ScheduledTask -TaskName $watchdogTaskName -InputObject $watchdogTask -Force | Out-Null

Assert-InferDeckTaskAction -TaskName $logonTaskName -StartupScript $startupScript | Out-Null
$registeredWatchdog = Assert-InferDeckTaskAction -TaskName $watchdogTaskName -StartupScript $startupScript
$watchdogTriggers = @($registeredWatchdog.Triggers)
if ($watchdogTriggers.Count -ne 1 -or !$watchdogTriggers[0].Repetition.Interval -or
    [System.Xml.XmlConvert]::ToTimeSpan([string]$watchdogTriggers[0].Repetition.Interval) -ne (New-TimeSpan -Minutes 15)) {
    throw "The InferDeck watchdog trigger could not be verified."
}

Start-ScheduledTask -TaskName $logonTaskName
$verified = Wait-InferDeckHealth -ExpectedExecutable $expectedExecutable -Attempts 120
[pscustomobject]@{
    Healthy = $verified.Health.ok
    ListenerProcessId = $verified.Owner.ProcessId
    ListenerExecutable = $verified.Owner.ExecutablePath
    LogonTask = (Get-ScheduledTask -TaskName $logonTaskName).State
    WatchdogTask = (Get-ScheduledTask -TaskName $watchdogTaskName).State
    WatchdogInterval = $watchdogTriggers[0].Repetition.Interval
    TaskUser = $taskUser
    ConfigPreserved = $configExisted
    RollbackBin = $deployment.RollbackBin
} | ConvertTo-Json | Tee-Object -FilePath (Join-Path $logs "logon-startup-result.json")
