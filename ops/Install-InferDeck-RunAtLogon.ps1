$ErrorActionPreference = "Stop"

$root = "C:\InferDeck"
$repo = "C:\Users\david\Documents\GitHub\InferDeck"
$bin = Join-Path $root "bin"
$configDir = Join-Path $root "config"
$logs = Join-Path $root "logs"
$startupScript = Join-Path $root "Start-InferDeck.ps1"

New-Item -ItemType Directory -Force -Path $bin, $configDir, $logs | Out-Null
Copy-Item -Path (Join-Path $repo "build\bin\Release\*") -Destination $bin -Recurse -Force
Copy-Item -Path (Join-Path $repo "ops\Start-InferDeck.ps1") -Destination $startupScript -Force
Copy-Item -Path (Join-Path $repo "config\gateway.yml") -Destination (Join-Path $configDir "gateway.yml") -Force

$cfgPath = Join-Path $configDir "gateway.yml"
$cfg = Get-Content $cfgPath -Raw
$cfg = $cfg -replace 'file: "~/.inferdeck/state\.json"', 'file: "C:/Users/david/.inferdeck/state.json"'
$cfg = $cfg -replace 'stats_db: "~/.inferdeck/stats\.db"', 'stats_db: "C:/Users/david/.inferdeck/stats.db"'
Set-Content -Path $cfgPath -Value $cfg -Encoding UTF8

Copy-Item (Join-Path $bin "inferdeck-gateway.exe") (Join-Path $bin "gateway-service.exe") -Force

Get-ScheduledTask -TaskName "InferDeck Gateway Logon" -ErrorAction SilentlyContinue | ForEach-Object {
    Stop-ScheduledTask -TaskName $_.TaskName -ErrorAction SilentlyContinue
    Unregister-ScheduledTask -TaskName $_.TaskName -Confirm:$false -ErrorAction SilentlyContinue
}

$runCommand = "powershell.exe -NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$startupScript`""
New-Item -Path "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run" -Force | Out-Null
Set-ItemProperty -Path "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run" -Name "InferDeck Gateway" -Value $runCommand

Get-Process -Name gateway-service,inferdeck-gateway -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Process -FilePath "powershell.exe" -ArgumentList "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$startupScript`"" -WindowStyle Hidden
Start-Sleep -Seconds 8

$health = Invoke-RestMethod -Uri "http://127.0.0.1:11434/api/health" -TimeoutSec 10
[pscustomobject]@{
    Health = $health.status
    DbPath = $health.db_path
    Startup = "HKCU Run"
    Command = $runCommand
} | ConvertTo-Json | Tee-Object -FilePath (Join-Path $logs "run-at-logon-result.json")
