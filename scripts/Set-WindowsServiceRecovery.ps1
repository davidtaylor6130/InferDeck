[CmdletBinding(SupportsShouldProcess)]
param(
    [ValidatePattern('^[A-Za-z0-9_. -]+$')]
    [string]$ServiceName = 'InferDeck',
    [string]$NssmPath = 'C:\InferDeck\nssm.exe',
    [string]$BackupDirectory = ''
)

$ErrorActionPreference = 'Stop'
$nssm = (Resolve-Path -LiteralPath $NssmPath).Path
$service = Get-CimInstance Win32_Service -Filter "Name='$ServiceName'"
if (-not $service) {
    throw "Windows service '$ServiceName' does not exist."
}
if (-not [string]::Equals($service.PathName.Trim('"'), $nssm,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw "Service '$ServiceName' is not managed by the specified NSSM executable."
}
if (-not $PSCmdlet.ShouldProcess($ServiceName, 'Enable NSSM restart and Windows service recovery')) {
    return
}

function Invoke-Native([string]$Executable, [string[]]$NativeArguments) {
    $output = & $Executable @NativeArguments 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "$Executable failed ($LASTEXITCODE): $($output -join [Environment]::NewLine)"
    }
}

if (-not $BackupDirectory) {
    $BackupDirectory = Join-Path (Split-Path -Parent $nssm) (
        'backups\service-recovery-' + $ServiceName + '-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
}
$backup = New-Item -ItemType Directory -Path $BackupDirectory -Force
$registryBackup = Join-Path $backup.FullName 'service-before.reg'
if (Test-Path -LiteralPath $registryBackup) {
    throw "A recovery backup already exists at '$registryBackup'. Choose a new directory."
}
Invoke-Native 'reg.exe' @('export', "HKLM\SYSTEM\CurrentControlSet\Services\$ServiceName", $registryBackup)

Invoke-Native $nssm @('set', $ServiceName, 'AppExit', 'Default', 'Restart')
Invoke-Native $nssm @('set', $ServiceName, 'AppRestartDelay', '5000')
Invoke-Native 'sc.exe' @('failure', $ServiceName, 'reset=', '86400',
    'actions=', 'restart/10000/restart/30000/restart/60000')
Invoke-Native 'sc.exe' @('failureflag', $ServiceName, '1')

$parameters = "HKLM:\SYSTEM\CurrentControlSet\Services\$ServiceName\Parameters"
$exitAction = (Get-Item -LiteralPath "$parameters\AppExit").GetValue('')
$delay = (Get-ItemProperty -LiteralPath $parameters).AppRestartDelay
if ($exitAction -ne 'Restart' -or $delay -ne 5000) {
    throw 'NSSM recovery settings did not persist.'
}
[pscustomobject]@{
    ServiceName = $ServiceName
    ExitAction = $exitAction
    RestartDelayMs = $delay
    RegistryBackup = $registryBackup
}
