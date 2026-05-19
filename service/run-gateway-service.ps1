$exe = "C:\Users\david\Documents\GitHub\InferDeck\build\bin\Release\gateway-service.exe"
$args = "-c", "C:\Users\david\Documents\GitHub\InferDeck\config\gateway.yml"
$logFile = "C:\Users\david\Documents\GitHub\InferDeck\run-logs\gateway-service.log"
$workDir = "C:\Users\david\Documents\GitHub\InferDeck"

$env:PATH = "C:\vcpkg\installed\x64-windows\bin;$env:PATH"

# Kill any existing instances
Get-Process -Name "gateway-service" -ErrorAction SilentlyContinue | Stop-Process -Force
Get-Process -Name "inferdeck-gateway" -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 2

while ($true) {
    Write-Output "Starting gateway-service at $(Get-Date)" | Tee-Object -FilePath $logFile -Append
    try {
        $proc = Start-Process -FilePath $exe -ArgumentList $args -WorkingDirectory $workDir -NoNewWindow -PassThru
        $proc | Wait-Process
        Write-Output "Process exited with code $($proc.ExitCode). Restarting in 5 seconds..." | Tee-Object -FilePath $logFile -Append
    } catch {
        Write-Output "Error: $_. Restarting in 5 seconds..." | Tee-Object -FilePath $logFile -Append
    }
    Start-Sleep -Seconds 5
}
