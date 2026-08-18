param(
    [string]$ListenAddress = '192.168.0.168',
    [string]$EditorAddress = '192.168.0.172',
    [int]$Port = 11436,
    [string]$RuntimeRoot = 'C:\InferDeck\runtime\forced-aligner',
    [string]$TokenFile = 'C:\InferDeck\config\forced-aligner-token.txt',
    [string]$RuleName = 'InferDeck Forced Aligner 11436 - Video Editor'
)

$ErrorActionPreference = 'Stop'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this script from an elevated PowerShell window'
}
$address = Get-NetIPAddress -AddressFamily IPv4 | Where-Object { $_.IPAddress -eq $ListenAddress }
if (-not $address) { throw "Host does not own IPv4 address $ListenAddress" }
if (-not (Test-Path -LiteralPath $TokenFile -PathType Leaf)) { throw "Missing token file: $TokenFile" }
$token = (Get-Content -LiteralPath $TokenFile -Raw).Trim()
if ($token.Length -lt 43) { throw 'Aligner Bearer token must contain at least 256 bits of encoded entropy' }
Remove-Variable token

$repoRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$launcherSource = Join-Path $repoRoot 'scripts\windows\Start-ForcedAligner.ps1'
$launcherDestination = Join-Path $RuntimeRoot 'Start-ForcedAligner.ps1'
$python = Join-Path $RuntimeRoot '.venv\Scripts\python.exe'
$watcher = Join-Path $RuntimeRoot 'Watch-ForcedAligner.ps1'
if (-not (Test-Path -LiteralPath $launcherSource -PathType Leaf)) { throw "Missing launcher source: $launcherSource" }
if (-not (Test-Path -LiteralPath $python -PathType Leaf)) { throw "Missing aligner Python: $python" }
if (-not (Test-Path -LiteralPath $watcher -PathType Leaf)) { throw "Missing aligner watchdog: $watcher" }

function Start-AlignerProcesses {
    & $launcherDestination -ListenAddress $ListenAddress -TokenFile $TokenFile
    Start-Process -FilePath 'powershell.exe' -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-WindowStyle', 'Hidden', '-File', $watcher) -WindowStyle Hidden | Out-Null
}

$targets = @()
$watchdogLock = Join-Path $RuntimeRoot 'watchdog.pid'
$alignerLock = Join-Path $RuntimeRoot 'aligner.pid'
foreach ($lock in @($watchdogLock, $alignerLock)) {
    if (Test-Path -LiteralPath $lock) {
        $candidate = Get-Content -LiteralPath $lock | Select-Object -First 1
        if ($candidate) { $targets += [int]$candidate }
    }
}
$listener = netstat.exe -ano | Where-Object { $_ -match ":${Port}\s+.*LISTENING\s+(\d+)\s*$" } | Select-Object -First 1
if ($listener) { $targets += [int]([regex]::Match($listener, '(\d+)\s*$').Groups[1].Value) }
$targets = @($targets | Select-Object -Unique)
$runningTargets = @()
foreach ($targetPid in $targets) {
    $process = Get-Process -Id $targetPid -ErrorAction SilentlyContinue
    if ($process -and $process.ProcessName -notin @('powershell', 'python', 'inferdeck-forced-aligner')) {
        throw "Unexpected process in aligner PID file: $targetPid $($process.ProcessName)"
    }
    if ($process) { $runningTargets += $targetPid }
}
if ($runningTargets) {
    Stop-Process -Id $runningTargets -ErrorAction Stop
    $stopDeadline = [DateTime]::UtcNow.AddSeconds(15)
    do {
        $remaining = @($runningTargets | Where-Object { Get-Process -Id $_ -ErrorAction SilentlyContinue })
        if (-not $remaining) { break }
        Start-Sleep -Milliseconds 250
    } while ([DateTime]::UtcNow -lt $stopDeadline)
    if ($remaining) { throw "Aligner processes did not stop: $($remaining -join ', ')" }
}

try {
    & $python -m pip install --disable-pip-version-check --force-reinstall --no-deps (Join-Path $repoRoot 'apps\forced-aligner')
    if ($LASTEXITCODE -ne 0) { throw 'Aligner package installation failed' }
    Copy-Item -LiteralPath $launcherSource -Destination $launcherDestination -Force
} catch {
    if (Test-Path -LiteralPath $launcherDestination -PathType Leaf) {
        try { Start-AlignerProcesses } catch { }
    }
    throw
}

try {
    $existingRule = Get-NetFirewallRule -DisplayName $RuleName -ErrorAction SilentlyContinue
    if ($existingRule) {
        $portFilter = $existingRule | Get-NetFirewallPortFilter
        $addressFilter = $existingRule | Get-NetFirewallAddressFilter
        $validRule = $existingRule.Direction -eq 'Inbound' -and
            $existingRule.Action -eq 'Allow' -and
            $existingRule.Enabled -eq 'True' -and
            $portFilter.Protocol -eq 'TCP' -and
            $portFilter.LocalPort -eq [string]$Port -and
            $addressFilter.LocalAddress -contains $ListenAddress -and
            $addressFilter.RemoteAddress -contains $EditorAddress
        if (-not $validRule) { throw "Existing firewall rule does not match the required restrictions: $RuleName" }
    } else {
        New-NetFirewallRule -DisplayName $RuleName -Direction Inbound -Action Allow -Enabled True -Profile Any -Protocol TCP -LocalPort $Port -LocalAddress $ListenAddress -RemoteAddress $EditorAddress | Out-Null
    }
} catch {
    Start-AlignerProcesses
    throw
}

Start-AlignerProcesses

$healthUrl = "http://${ListenAddress}:${Port}/health"
$alignmentUrl = "http://${ListenAddress}:${Port}/v1/audio/alignments"
$health = Invoke-RestMethod -Uri $healthUrl -TimeoutSec 10
if (-not $health.modelLoaded) { throw 'Aligner health check did not report a loaded model' }
$optionsStatus = & curl.exe -sS -o NUL -w '%{http_code}' -X OPTIONS -H 'Origin: http://editor.local' -H 'Access-Control-Request-Method: POST' $alignmentUrl
if ($optionsStatus -notin @('200', '204')) { throw "Alignment preflight returned HTTP $optionsStatus" }
$port11434 = netstat.exe -ano | Where-Object { $_ -match ':11434\s+.*LISTENING' } | Select-Object -First 1
if (-not $port11434) { throw 'InferDeck gateway listener on port 11434 is missing' }
[pscustomobject]@{
    Health = $health
    OptionsStatus = [int]$optionsStatus
    FirewallRule = $RuleName
    LocalAddress = $ListenAddress
    RemoteAddress = $EditorAddress
    Gateway11434Unchanged = $true
} | ConvertTo-Json -Depth 4
