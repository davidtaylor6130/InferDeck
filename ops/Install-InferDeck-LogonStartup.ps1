$ErrorActionPreference = "Stop"

$root = "C:\InferDeck"
$repo = "C:\Users\david\Documents\GitHub\InferDeck"
$bin = Join-Path $root "bin"
$configDir = Join-Path $root "config"
$logs = Join-Path $root "logs"
$startupScript = Join-Path $root "Start-InferDeck.ps1"
$taskName = "InferDeck Gateway Logon"
$taskUser = [System.Security.Principal.WindowsIdentity]::GetCurrent().Name

New-Item -ItemType Directory -Force -Path $bin, $configDir, $logs | Out-Null
Copy-Item -Path (Join-Path $repo "build\bin\Release\*") -Destination $bin -Recurse -Force
Copy-Item -Path (Join-Path $repo "ops\Start-InferDeck.ps1") -Destination $startupScript -Force
Copy-Item -Path (Join-Path $repo "config\gateway.yml") -Destination (Join-Path $configDir "gateway.yml") -Force

$cfgPath = Join-Path $configDir "gateway.yml"
$cfg = Get-Content $cfgPath -Raw
$cfg = $cfg -replace 'file: "~/.inferdeck/state\.json"', 'file: "C:/Users/david/.inferdeck/state.json"'
$cfg = $cfg -replace 'stats_db: "~/.inferdeck/stats\.db"', 'stats_db: "C:/Users/david/.inferdeck/stats.db"'
Set-Content -Path $cfgPath -Value $cfg -Encoding UTF8

$legacy = Join-Path $bin "gateway-service.exe"
Copy-Item (Join-Path $bin "inferdeck-gateway.exe") $legacy -Force

$existing = Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
if ($existing) {
    Stop-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
    Unregister-ScheduledTask -TaskName $taskName -Confirm:$false
}

$ollamaTask = Get-ScheduledTask -TaskName "Ollama AI Server" -ErrorAction SilentlyContinue
if ($ollamaTask) {
    Disable-ScheduledTask -TaskName "Ollama AI Server" | Out-Null
}

$action = New-ScheduledTaskAction -Execute "powershell.exe" -Argument "-NoProfile -ExecutionPolicy Bypass -File `"$startupScript`""
$trigger = New-ScheduledTaskTrigger -AtLogOn -User $taskUser
$principal = New-ScheduledTaskPrincipal -UserId $taskUser -LogonType Interactive
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Days 0) -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1) -MultipleInstances IgnoreNew
$task = New-ScheduledTask -Action $action -Trigger $trigger -Principal $principal -Settings $settings -Description "Starts InferDeck v2 gateway when the user logs on."
Register-ScheduledTask -TaskName $taskName -InputObject $task -Force | Out-Null
Start-ScheduledTask -TaskName $taskName
Start-Sleep -Seconds 8

$health = Invoke-RestMethod -Uri "http://127.0.0.1:11434/api/health" -TimeoutSec 10
[pscustomobject]@{
    Health = $health.status
    DbPath = $health.db_path
    TaskName = $taskName
    TaskUser = $taskUser
    Trigger = "AtLogOn"
    OllamaTask = (Get-ScheduledTask -TaskName "Ollama AI Server" -ErrorAction SilentlyContinue).State
} | ConvertTo-Json | Tee-Object -FilePath (Join-Path $logs "logon-startup-result.json")
