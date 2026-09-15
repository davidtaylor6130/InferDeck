#!/usr/bin/env pwsh
# Deploy alpha-18 (commit 8fb106a) to C:\InferDeck and verify

$ErrorActionPreference = 'Stop'
Set-Location C:\Users\david\Documents\GitHub\InferDeck

Write-Host "=== Pulling verified branch ==="
git checkout agent/v1-pp-stability
git pull
git log --oneline -n 3

Write-Host "=== Stopping service ==="
$nssm = "C:\InferDeck\nssm.exe", "C:\Program Files\nssm\nssm.exe", "C:\Program Files (x86)\nssm\nssm.exe", "C:\nssm\nssm.exe" | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $nssm) { throw "nssm.exe not found" }
& $nssm stop InferDeck

Write-Host "=== Backing up current runtime ==="
$ts = Get-Date -Format "yyyyMMdd-HHmmss"
Copy-Item "C:\InferDeck\inferdeck-gateway.exe" "C:\InferDeck\backup-inferdeck-gateway-$ts.exe" -Force
New-Item -ItemType Directory -Force -Path "C:\InferDeck\static.bak\$ts" | Out-Null
Copy-Item "C:\InferDeck\static\*" "C:\InferDeck\static.bak\$ts\" -Recurse -Force

Write-Host "=== Deploying artifacts ==="
Copy-Item "C:\Users\david\Documents\GitHub\InferDeck\build\bin\Release\inferdeck-gateway.exe" "C:\InferDeck\inferdeck-gateway.exe" -Force
Copy-Item "C:\Users\david\Documents\GitHub\InferDeck\apps\inferdeck-gateway\static\*" "C:\InferDeck\static\" -Recurse -Force

Write-Host "=== Starting service ==="
& $nssm start InferDeck
Start-Sleep -Seconds 3

Write-Host "=== Recent log ==="
Get-Content "C:\InferDeck\inferdeck.log" -Tail 20

Write-Host "=== Health checks ==="
Invoke-RestMethod http://127.0.0.1:11434/api/inferdeck/v1/health
Invoke-RestMethod http://127.0.0.1:11434/v1/models
Invoke-RestMethod http://127.0.0.1:11434/api/inferdeck/v1/swap/status

Write-Host "=== Smoke test (utility alias) ==="
$body = @{
  model = "utility"
  messages = @(@{ role = "user"; content = "ping" })
  max_tokens = 16
} | ConvertTo-Json -Depth 5
Invoke-RestMethod -Uri http://127.0.0.1:11434/v1/chat/completions -Method Post -Body $body -ContentType "application/json"

Write-Host "=== Deploy complete ==="