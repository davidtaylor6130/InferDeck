# Run this script AS ADMINISTRATOR to start the InferDeck Gateway service
& "C:\Windows\System32\sc.exe" start InferDeckGateway
if ($LASTEXITCODE -eq 0) {
    Write-Host "Service started successfully." -ForegroundColor Green
    & "C:\Windows\System32\sc.exe" query InferDeckGateway
} else {
    Write-Host "Failed to start service (may need admin rights, or port is in use)." -ForegroundColor Red
}
Read-Host "Press Enter to exit"
