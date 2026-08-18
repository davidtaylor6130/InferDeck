$ErrorActionPreference = 'Continue'
$launcher = 'C:\InferDeck\runtime\forced-aligner\Start-ForcedAligner.ps1'
$lock = 'C:\InferDeck\runtime\forced-aligner\watchdog.pid'
if (Test-Path -LiteralPath $lock) {
    $existing = Get-Content -LiteralPath $lock -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($existing -and ([int]$existing -ne $PID) -and (Get-Process -Id ([int]$existing) -ErrorAction SilentlyContinue)) { exit 0 }
}
Set-Content -LiteralPath $lock -Value $PID -Encoding ASCII
while ($true) {
    try {
        & $launcher
    } catch {
        $message = "$(Get-Date -Format o) watchdog_error=$($_.Exception.GetType().Name)`r`n"
        Add-Content -LiteralPath 'C:\InferDeck\logs\forced-aligner-watchdog.log' -Value $message -Encoding UTF8
    }
    Start-Sleep -Seconds 60
}
