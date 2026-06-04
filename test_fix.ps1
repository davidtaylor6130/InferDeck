param(
    [switch]$NoCopy
)

$BuildDir = "C:\Users\david\Documents\GitHub\InferDeck\build\bin\Release"
$DeployDir = "C:\InferDeck"
$ConfigDir = "C:\Users\david\Documents\GitHub\InferDeck\config"
$ConfigDeployDir = "$DeployDir\config"
$LogDir = "C:\Users\david\Documents\GitHub\InferDeck\run-logs"
$LogFile = "$LogDir\gateway-test.log"
$Timestamp = Get-Date -Format "yyyy-MM-dd HH:mm:ss"

Write-Host "=== InferDeck Gateway Test ===" -ForegroundColor Cyan
Write-Host "Time: $Timestamp" -ForegroundColor Gray

# Copy build artifacts to deploy dir
if (-not $NoCopy) {
    Write-Host "`nCopying build to $DeployDir..." -ForegroundColor Yellow
    $files = @("gateway-service.exe", "llama.dll", "ggml.dll", "ggml-base.dll", "ggml-vulkan.dll", "ggml-cpu.dll", "llama-common.dll", "mtmd.dll", "fmt.dll", "spdlog.dll", "vulkan-1.dll")
    foreach ($f in $files) {
        Copy-Item -LiteralPath "$BuildDir\$f" -Destination "$DeployDir\" -Force -ErrorAction SilentlyContinue
    }
    if (Test-Path "$ConfigDeployDir\gateway.yml") {
        Copy-Item -LiteralPath "$ConfigDir\gateway.yml" -Destination "$ConfigDeployDir\gateway.yml" -Force
    }
}

# Kill any existing gateway-service
$existing = Get-Process -Name "gateway-service" -ErrorAction SilentlyContinue
if ($existing) {
    Write-Host "`nStopping existing gateway-service..." -ForegroundColor Yellow
    $existing | Stop-Process -Force
    Start-Sleep -Seconds 2
}

# Start gateway-service in a new window
Write-Host "`nStarting gateway-service..." -ForegroundColor Yellow
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = "$DeployDir\gateway-service.exe"
$psi.WorkingDirectory = $DeployDir
$psi.Arguments = "--config `"$ConfigDeployDir\gateway.yml`""
$psi.UseShellExecute = $true
$psi.CreateNoWindow = $false
$psi.WindowStyle = [System.Diagnostics.ProcessWindowStyle]::Normal
$proc = [System.Diagnostics.Process]::Start($psi)

Write-Host "   PID: $($proc.Id)" -ForegroundColor Gray

# Wait for the server to start
Write-Host "`nWaiting for server to be ready..." -ForegroundColor Yellow
$ready = $false
for ($i = 0; $i -lt 60; $i++) {
    Start-Sleep -Seconds 1
    try {
        $resp = Invoke-WebRequest -Uri "http://localhost:11434/api/chat" -Method POST -ContentType "application/json" -Body '{"model":"test","messages":[{"role":"user","content":"hi"}],"stream":false,"max_tokens":5}' -TimeoutSec 3 -ErrorAction SilentlyContinue
        if ($resp.StatusCode -eq 200) {
            $ready = $true
            break
        }
    } catch {}
}
if (-not $ready) {
    Write-Host "   Timed out waiting for server" -ForegroundColor Red
    exit 1
}
Write-Host "   Server is ready!" -ForegroundColor Green

# Send test message
Write-Host "`nSending test message..." -ForegroundColor Yellow
$body = @{
    model = "qwen3"
    messages = @(
        @{ role = "user"; content = "Say hello in one short sentence." }
    )
    stream = $false
    max_tokens = 100
    temperature = 0.01
} | ConvertTo-Json

try {
    $response = Invoke-WebRequest -Uri "http://localhost:11434/api/chat" -Method POST -ContentType "application/json" -Body $body -TimeoutSec 60
    $result = $response.Content | ConvertFrom-Json
    Write-Host "`n=== Response ===" -ForegroundColor Cyan
    Write-Host $result.message.content -ForegroundColor White
    
    # Check for think tags
    if ($result.message.content -match '<think>') {
        Write-Host "`n[PASS] Response contains <think> tag" -ForegroundColor Green
    } else {
        Write-Host "`n[FAIL] Response does NOT contain <think> tag" -ForegroundColor Red
    }
    
    # Check for raw reasoning leak (reasoning text without think tags)
    if ($result.message.content -match '(?s)^(?!<think>).+?</think>') {
        Write-Host "[WARN] Possible raw reasoning leak detected" -ForegroundColor Yellow
    }
    
    # Log to file
    $result.message.content | Out-File -FilePath $LogFile -Encoding utf8
    Write-Host "`nResponse saved to $LogFile" -ForegroundColor Gray
    
} catch {
    Write-Host "`nError: $_" -ForegroundColor Red
}

# Cleanup prompt
Write-Host "`nPress Ctrl+C to stop the gateway, or leave it running." -ForegroundColor Gray
Write-Host "To stop: Get-Process gateway-service | Stop-Process -Force" -ForegroundColor Gray
