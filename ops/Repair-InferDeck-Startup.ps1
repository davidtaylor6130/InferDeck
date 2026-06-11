$ErrorActionPreference = "Stop"

$root = "C:\InferDeck"
$repo = "C:\Users\david\Documents\GitHub\InferDeck"
$bin = Join-Path $root "bin"
$configDir = Join-Path $root "config"
$logs = Join-Path $root "logs"
$startupScript = Join-Path $root "Start-InferDeck.ps1"
$repairLog = Join-Path $logs "startup-repair.log"

New-Item -ItemType Directory -Force -Path $bin, $configDir, $logs | Out-Null
Start-Transcript -Path $repairLog -Force | Out-Null

foreach ($service in @("InferDeck", "InferDeckGateway")) {
    $svc = Get-Service -Name $service -ErrorAction SilentlyContinue
    if ($svc -and $svc.Status -ne "Stopped") {
        sc.exe stop $service | Out-Null
    }
}

Get-Process -Name gateway-service,inferdeck-gateway,ollama,llama-server -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
for ($i = 0; $i -lt 20; $i++) {
    $locked = Get-Process -Name gateway-service,inferdeck-gateway,ollama,llama-server -ErrorAction SilentlyContinue
    $listener = Get-NetTCPConnection -LocalPort 11434 -State Listen -ErrorAction SilentlyContinue
    if (!$locked -and !$listener) { break }
    Start-Sleep -Milliseconds 500
}
if (Get-Process -Name gateway-service,inferdeck-gateway,ollama,llama-server -ErrorAction SilentlyContinue) {
    throw "InferDeck/Ollama process is still running and may hold DLL locks. Stop it from Task Manager or rerun this script after reboot."
}

Copy-Item -Path (Join-Path $repo "build\bin\Release\*") -Destination $bin -Recurse -Force
Copy-Item -Path (Join-Path $repo "ops\Start-InferDeck.ps1") -Destination $startupScript -Force
Copy-Item -Path (Join-Path $repo "config\gateway.yml") -Destination (Join-Path $configDir "gateway.yml") -Force

$cfgPath = Join-Path $configDir "gateway.yml"
$cfg = Get-Content $cfgPath -Raw
$cfg = $cfg -replace 'file: "~/.inferdeck/state\.json"', 'file: "C:/Users/david/.inferdeck/state.json"'
$cfg = $cfg -replace 'stats_db: "~/.inferdeck/stats\.db"', 'stats_db: "C:/Users/david/.inferdeck/stats.db"'
Set-Content -Path $cfgPath -Value $cfg -Encoding UTF8

$legacy = Join-Path $bin "gateway-service.exe"
$backup = Join-Path $bin "gateway-service.legacy-backup.exe"
if ((Test-Path $legacy) -and !(Test-Path $backup)) {
    Copy-Item $legacy $backup -Force
}
Copy-Item (Join-Path $bin "inferdeck-gateway.exe") $legacy -Force

foreach ($service in @("InferDeck", "InferDeckGateway")) {
    $svc = Get-Service -Name $service -ErrorAction SilentlyContinue
    if ($svc) {
        sc.exe stop $service | Out-Null
        sc.exe config $service start= disabled | Out-Null
    }
}

$ollamaTask = Get-ScheduledTask -TaskName "Ollama AI Server" -ErrorAction SilentlyContinue
if ($ollamaTask) {
    Disable-ScheduledTask -TaskName "Ollama AI Server" | Out-Null
}

$taskName = "InferDeck Gateway Logon"
$existingTask = Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
if ($existingTask) {
    Stop-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
    Unregister-ScheduledTask -TaskName $taskName -Confirm:$false
}
Remove-Item (Join-Path $root "run\inferdeck-startup.lock") -Force -ErrorAction SilentlyContinue

$taskUser = [System.Security.Principal.WindowsIdentity]::GetCurrent().Name
$action = New-ScheduledTaskAction -Execute "powershell.exe" -Argument "-NoProfile -ExecutionPolicy Bypass -File `"$startupScript`""
$trigger = New-ScheduledTaskTrigger -AtLogOn -User $taskUser
$principal = New-ScheduledTaskPrincipal -UserId $taskUser -LogonType Interactive
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Days 0) -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1) -MultipleInstances IgnoreNew
$task = New-ScheduledTask -Action $action -Trigger $trigger -Principal $principal -Settings $settings -Description "Starts InferDeck v2 gateway when the user logs on."
Register-ScheduledTask -TaskName $taskName -InputObject $task -Force | Out-Null
Start-ScheduledTask -TaskName $taskName

$health = $null
$lastError = $null
for ($i = 0; $i -lt 15; $i++) {
    try {
        $health = Invoke-RestMethod -Uri "http://127.0.0.1:11434/api/health" -TimeoutSec 2
        break
    } catch {
        $lastError = $_.Exception.Message
        $taskLogProbe = if (Test-Path (Join-Path $logs "startup-task.log")) { (Get-Content (Join-Path $logs "startup-task.log") -Tail 5) -join "`n" } else { "" }
        if ($taskLogProbe -match "About to initialize llama backend" -and $taskLogProbe -notmatch "server_listening") {
            $lastError = "Gateway startup task stalled during llama_backend_init. This usually means the AMD Vulkan runtime is not available in the scheduled-task/pre-logon context."
            break
        }
        Start-Sleep -Seconds 2
    }
}
if (!$health) {
    $taskInfo = Get-ScheduledTaskInfo -TaskName $taskName -ErrorAction SilentlyContinue
    $taskMeta = if (Test-Path (Join-Path $logs "startup-task-meta.json")) { [string](Get-Content (Join-Path $logs "startup-task-meta.json") -Raw) } else { "" }
    $taskLog = if (Test-Path (Join-Path $logs "startup-task.log")) { (Get-Content (Join-Path $logs "startup-task.log") -Tail 80) -join "`n" } else { "" }
    [pscustomobject]@{
        Health = "failed"
        Error = $lastError
        TaskUser = $taskUser
        StartupTask = (Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue).State
        LastTaskResult = $taskInfo.LastTaskResult
        LastRunTime = $taskInfo.LastRunTime
        TaskMeta = $taskMeta
        TaskLogTail = $taskLog
    } | ConvertTo-Json -Depth 6 | Tee-Object -FilePath (Join-Path $logs "startup-repair-result.json")
    throw "InferDeck startup task did not become healthy on http://127.0.0.1:11434/api/health. See C:\InferDeck\logs\startup-repair-result.json"
}
$listener = Get-NetTCPConnection -LocalPort 11434 -State Listen -ErrorAction Stop | Select-Object -First 1
[pscustomobject]@{
    Health = $health.status
    DbPath = $health.db_path
    ListenerPid = $listener.OwningProcess
    TaskUser = $taskUser
    StartupTask = (Get-ScheduledTask -TaskName $taskName).State
    OllamaTask = (Get-ScheduledTask -TaskName "Ollama AI Server" -ErrorAction SilentlyContinue).State
    InferDeckServiceStart = (Get-CimInstance Win32_Service -Filter "Name='InferDeck'" -ErrorAction SilentlyContinue).StartMode
    LegacyGatewayServiceStart = (Get-CimInstance Win32_Service -Filter "Name='InferDeckGateway'" -ErrorAction SilentlyContinue).StartMode
} | ConvertTo-Json | Tee-Object -FilePath (Join-Path $logs "startup-repair-result.json")

Stop-Transcript | Out-Null
