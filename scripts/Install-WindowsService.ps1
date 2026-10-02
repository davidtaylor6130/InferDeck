[CmdletBinding(SupportsShouldProcess)]
param([string]$InstallRoot = 'C:\InferDeck')

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath $InstallRoot).Path
$nssm = Join-Path $root 'nssm.exe'
$application = Join-Path $root 'inferdeck-gateway.exe'
$recoveryScript = Join-Path $PSScriptRoot 'Set-WindowsServiceRecovery.ps1'
foreach ($file in @($nssm, $application, (Join-Path $root 'config\gateway.yml'), $recoveryScript)) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
        throw "Required deployment file is missing: '$file'."
    }
}
if (-not $PSCmdlet.ShouldProcess('InferDeck', "Install or repair the Windows service in '$root'")) {
    return
}

function Invoke-Nssm([string[]]$NativeArguments) {
    $output = & $nssm @NativeArguments 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "NSSM failed ($LASTEXITCODE): $($output -join [Environment]::NewLine)"
    }
}

$service = Get-CimInstance Win32_Service -Filter "Name='InferDeck'"
if ($service -and -not [string]::Equals($service.PathName.Trim('"'), $nssm,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The existing InferDeck service uses a different service manager.'
}
if (-not $service) {
    Invoke-Nssm @('install', 'InferDeck', $application, '-c config\gateway.yml')
}
New-Item -ItemType Directory -Path (Join-Path $root 'logs') -Force | Out-Null
Invoke-Nssm @('set', 'InferDeck', 'Application', $application)
Invoke-Nssm @('set', 'InferDeck', 'AppDirectory', $root)
Invoke-Nssm @('set', 'InferDeck', 'AppParameters', '-c config\gateway.yml')
Invoke-Nssm @('set', 'InferDeck', 'Start', 'SERVICE_AUTO_START')
Invoke-Nssm @('set', 'InferDeck', 'AppStdout', (Join-Path $root 'logs\service-out.log'))
Invoke-Nssm @('set', 'InferDeck', 'AppStderr', (Join-Path $root 'logs\service-err.log'))
& $recoveryScript -NssmPath $nssm
Write-Output 'InferDeck service installation and crash recovery are configured.'
