$exe = "C:\Users\david\Documents\GitHub\InferDeck\build\bin\Release\inferdeck-gateway.exe"
$exeArgs = "-c", "C:\Users\david\Documents\GitHub\InferDeck\config\gateway.yml"
$logFile = "C:\Users\david\Documents\GitHub\InferDeck\run-logs\inferdeck-gateway.log"
$workDir = "C:\Users\david\Documents\GitHub\InferDeck"

New-Item -ItemType Directory -Force (Split-Path $logFile) | Out-Null

Get-Process -Name "inferdeck-gateway" -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 2

while ($true) {
    Write-Output "Starting inferdeck-gateway at $(Get-Date)" | Tee-Object -FilePath $logFile -Append
    try {
        $proc = Start-Process -FilePath $exe -ArgumentList $exeArgs -WorkingDirectory $workDir -NoNewWindow -PassThru
        $proc | Wait-Process
        Write-Output "Process exited with code $($proc.ExitCode). Restarting in 5 seconds..." | Tee-Object -FilePath $logFile -Append
    } catch {
        Write-Output "Error: $_. Restarting in 5 seconds..." | Tee-Object -FilePath $logFile -Append
    }
    Start-Sleep -Seconds 5
}
