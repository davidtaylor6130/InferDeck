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
    icacls $StateDir /inheritance:r /grant:r '*S-1-5-18:(OI)(CI)F' '*S-1-5-32-544:(OI)(CI)F' | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "icacls failed on $StateDir" }
    $owner = (Get-Acl $StateDir).GetOwner([System.Security.Principal.SecurityIdentifier]).Value
    if ($owner -notin @('S-1-5-18', 'S-1-5-32-544')) {
        throw "$StateDir is owned by $owner; refusing to install a SYSTEM task script from it"
    }
    Copy-Item $PSCommandPath $script -Force
    $action = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument "-NoProfile -ExecutionPolicy Bypass -File `"$script`" -Service $Service"
    $trigger = New-ScheduledTaskTrigger -Once -At (Get-Date) -RepetitionInterval (New-TimeSpan -Minutes 1)
    $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Minutes 5) -MultipleInstances IgnoreNew
    Register-ScheduledTask -TaskName 'InferDeck GPU Hang Recovery' -Action $action -Trigger $trigger -Settings $settings -User 'SYSTEM' -RunLevel Highest -Force | Out-Null
    Write-Host "Installed scheduled task 'InferDeck GPU Hang Recovery' running $script every minute."
    return
}

New-Item $StateDir -ItemType Directory -Force | Out-Null
$log = Join-Path $StateDir 'recovery.log'
$last = Join-Path $StateDir 'last-recovery.txt'
$marker = Join-Path $StateDir 'recovery-in-progress.txt'
function Write-Log([string]$Message) { Add-Content $log "$(Get-Date -Format s) $Message" }

if (Test-Path $marker) {
    $pending = Get-CimInstance Win32_Service -Filter "Name='$Service'"
    if ($pending -and $pending.StartMode -ne 'Disabled' -and $pending.State -eq 'Stopped') {
        Start-Service $Service -ErrorAction SilentlyContinue
        Write-Log "interrupted recovery found; started $Service, state=$((Get-Service $Service).Status)"
        Remove-Item $marker -Force -ErrorAction SilentlyContinue
    } elseif ($pending -and $pending.State -eq 'Running') {
        Remove-Item $marker -Force -ErrorAction SilentlyContinue
    }
}

$since = (Get-Date).AddMinutes(-$WindowMinutes)
$processed = Join-Path $StateDir 'processed-dumps.txt'
$seen = @(if (Test-Path $processed) { Get-Content $processed })
$hangs = @(Get-ChildItem (Join-Path $env:SystemRoot 'LiveKernelReports\WATCHDOG') -Filter *.dmp -File -ErrorAction SilentlyContinue |
    Where-Object { $_.LastWriteTime -gt $since -and $seen -notcontains $_.Name })
if ($hangs.Count -lt $Threshold) { return }

Add-Content $processed $hangs.Name

$recent = (Test-Path $last) -and ((Get-Date) - (Get-Item $last).LastWriteTime).TotalMinutes -lt $CooldownMinutes
if ($recent) {
    Write-Log "$($hangs.Count) GPU watchdog reports but a recovery ran within $CooldownMinutes min; taking no action"
    return
}
$svc = Get-Service $Service -ErrorAction SilentlyContinue
if (-not $svc -or $svc.Status -ne 'Running') {
    Write-Log "$($hangs.Count) GPU watchdog reports but $Service is not running; taking no action"
    return
}

Write-Log "$($hangs.Count) GPU watchdog reports in $WindowMinutes min; stopping $Service and restarting the display adapter"
$serviceInfo = Get-CimInstance Win32_Service -Filter "Name='$Service'"
$servicePid = [int]$serviceInfo.ProcessId
$serviceExe = if ($serviceInfo.PathName -match '^\s*"([^"]+)"') { $Matches[1] } else { ($serviceInfo.PathName.Trim() -split '\s+')[0] }
$application = (Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Services\$Service\Parameters" -ErrorAction SilentlyContinue).Application
$children = @(if ($servicePid -gt 0 -and $application) {
    Get-CimInstance Win32_Process -Filter "ParentProcessId=$servicePid" | Where-Object { $_.ExecutablePath -eq $application }
})
Set-Content $marker (Get-Date -Format s)
sc.exe stop $Service | Out-Null
$deadline = (Get-Date).AddSeconds(90)
while ((Get-Service $Service -ErrorAction SilentlyContinue).Status -ne 'Stopped' -and (Get-Date) -lt $deadline) { Start-Sleep 2 }
function Stop-IfPathMatches([int]$ProcessId, [string]$Path) {
    $current = Get-CimInstance Win32_Process -Filter "ProcessId=$ProcessId" -ErrorAction SilentlyContinue
    if ($current -and $current.ExecutablePath -eq $Path) { Stop-Process -Id $ProcessId -Force -ErrorAction SilentlyContinue }
}
foreach ($child in $children) { Stop-IfPathMatches $child.ProcessId $application }
if ($servicePid -gt 0) { Stop-IfPathMatches $servicePid $serviceExe }
foreach ($gpu in Get-PnpDevice -Class Display -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\*' }) {
    pnputil /restart-device "$($gpu.InstanceId)" | Out-Null
    Write-Log "restarted $($gpu.FriendlyName) exit=$LASTEXITCODE"
}
Set-Content $last (Get-Date -Format s)
Start-Sleep 20
Start-Service $Service -ErrorAction SilentlyContinue
$state = (Get-Service $Service -ErrorAction SilentlyContinue).Status
Write-Log "$Service state after start: $state"
if ($state -eq 'Running') { Remove-Item $marker -Force -ErrorAction SilentlyContinue }
