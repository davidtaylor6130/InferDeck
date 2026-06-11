$ErrorActionPreference = "Stop"

$root = "C:\InferDeck"
$exe = Join-Path $root "bin\gateway-service.exe"
$config = Join-Path $root "config\gateway.yml"
$logs = Join-Path $root "logs"
$lock = Join-Path $root "run\inferdeck-startup.lock"
$meta = Join-Path $logs "startup-task-meta.json"

New-Item -ItemType Directory -Force -Path $logs, (Split-Path $lock) | Out-Null
try {
    $health = Invoke-RestMethod -Uri "http://127.0.0.1:11434/api/health" -TimeoutSec 2
    if ($health.status -eq "healthy") {
        exit 0
    }
} catch {
}
if (Test-Path $lock) {
    $existing = Get-Content $lock -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($existing -and (Get-Process -Id ([int]$existing) -ErrorAction SilentlyContinue)) {
        exit 0
    }
}

Get-Process -Name ollama,llama-server,inferdeck-gateway -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Set-Content -Path $lock -Value $PID -Encoding ASCII
Set-Location $root
$env:PATH = "$root\bin;$root;$env:PATH"
[pscustomobject]@{
    User = [System.Security.Principal.WindowsIdentity]::GetCurrent().Name
    Pid = $PID
    Root = $root
    Exe = $exe
    Config = $config
    StartedAt = (Get-Date).ToString("o")
    Path = $env:PATH
} | ConvertTo-Json | Set-Content -Path $meta -Encoding UTF8
$stdout = Join-Path $logs "startup-task.out.log"
$stderr = Join-Path $logs "startup-task.err.log"
$child = Start-Process -FilePath $exe -ArgumentList @("-c", $config) -WorkingDirectory $root -WindowStyle Hidden -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
Set-Content -Path $lock -Value $child.Id -Encoding ASCII
for ($i = 0; $i -lt 20; $i++) {
    try {
        $health = Invoke-RestMethod -Uri "http://127.0.0.1:11434/api/health" -TimeoutSec 2
        if ($health.status -eq "healthy") { exit 0 }
    } catch {
        Start-Sleep -Seconds 1
    }
}
exit 1
