$ErrorActionPreference = "Stop"
Set-StrictMode -Version 3.0

$repo = "C:\Users\david\Documents\GitHub\InferDeck"
$common = Join-Path $repo "ops\InferDeck-Lifecycle.Common.ps1"
$installer = Join-Path $repo "ops\Install-InferDeck-LogonStartup.ps1"

if (!(Test-Path -LiteralPath $common -PathType Leaf)) {
    throw "Missing lifecycle helper: $common"
}
. $common

$services = @(Get-InferDeckWindowsServices)
if ($services.Count -gt 0) {
    $details = $services | ForEach-Object {
        "$($_.Name) (account=$($_.StartName), state=$($_.State), path=$($_.PathName))"
    }
    throw "A Windows service deployment exists: $($details -join '; '). Repair will not disable it or migrate it to a logon task. Use the service deployment procedure."
}
if (!(Test-Path -LiteralPath $installer -PathType Leaf)) {
    throw "Missing logon installer: $installer"
}

& $installer
