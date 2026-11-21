#Requires -RunAsAdministrator
param(
    [switch]$Install,
    [string]$Service = 'InferDeck',
    [int]$WindowMinutes = 5,
    [int]$Threshold = 2,
    [int]$CooldownMinutes = 120,
    [string]$StateDir = (Join-Path $env:ProgramData 'InferDeck\gpu-hang-recovery')
)
$ErrorActionPreference = 'Stop'

if ($Install) {
    $script = Join-Path $StateDir 'Recover-GpuHang.ps1'
    New-Item $StateDir -ItemType Directory -Force | Out-Null
    Copy-Item $PSCommandPath $script -Force
    $action = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument "-NoProfile -ExecutionPolicy Bypass -File `"$script`" -Service $Service"
    $trigger = New-ScheduledTaskTrigger -Once -At (Get-Date) -RepetitionInterval (New-TimeSpan -Minutes 1)
    $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Minutes 5) -MultipleInstances IgnoreNew
    Register-ScheduledTask -TaskName 'InferDeck GPU Hang Recovery' -Action $action -Trigger $trigger -Settings $settings -User 'SYSTEM' -RunLevel Highest -Force | Out-Null
    Write-Host "Installed scheduled task 'InferDeck GPU Hang Recovery' running $script every minute."
    return
}

$since = (Get-Date).AddMinutes(-$WindowMinutes)
$hangs = @(Get-ChildItem (Join-Path $env:SystemRoot 'LiveKernelReports\WATCHDOG') -Filter *.dmp -File -ErrorAction SilentlyContinue |
    Where-Object LastWriteTime -gt $since)
if ($hangs.Count -lt $Threshold) { return }

New-Item $StateDir -ItemType Directory -Force | Out-Null
$log = Join-Path $StateDir 'recovery.log'
$last = Join-Path $StateDir 'last-recovery.txt'
$recent = (Test-Path $last) -and ((Get-Date) - (Get-Item $last).LastWriteTime).TotalMinutes -lt $CooldownMinutes
function Write-Log([string]$Message) { Add-Content $log "$(Get-Date -Format s) $Message" }

Write-Log "$($hangs.Count) GPU watchdog reports in $WindowMinutes min; stopping $Service and restarting the display adapter"
Stop-Service $Service -Force -ErrorAction SilentlyContinue
Get-Process inferdeck-gateway -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
foreach ($gpu in Get-PnpDevice -Class Display -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\*' }) {
    pnputil /restart-device "$($gpu.InstanceId)" | Out-Null
    Write-Log "restarted $($gpu.FriendlyName) exit=$LASTEXITCODE"
}
Get-ChildItem (Join-Path $env:SystemRoot 'LiveKernelReports\WATCHDOG') -Filter *.dmp -File -ErrorAction SilentlyContinue |
    Where-Object LastWriteTime -gt $since | Remove-Item -Force -ErrorAction SilentlyContinue
Set-Content $last (Get-Date -Format s)
if ($recent) {
    Write-Log "second recovery within $CooldownMinutes min; leaving $Service stopped"
} else {
    Start-Sleep 20
    Start-Service $Service -ErrorAction SilentlyContinue
    Write-Log "$Service started"
}
