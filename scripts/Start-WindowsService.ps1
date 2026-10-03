$ErrorActionPreference = 'Stop'
Start-Service -Name 'InferDeck'
for ($attempt = 0; $attempt -lt 20; ++$attempt) {
    try {
        $health = Invoke-RestMethod -Uri 'http://127.0.0.1:11434/api/inferdeck/v1/health' -TimeoutSec 2
        if ($health.ok) {
            Write-Output 'InferDeck production service is healthy.'
            return
        }
    } catch {
        if ((Get-Service -Name 'InferDeck').Status -eq 'Stopped') {
            throw 'InferDeck stopped during startup. Check its service logs.'
        }
    }
    Start-Sleep -Seconds 1
}
throw 'InferDeck health check did not pass within the startup deadline.'
