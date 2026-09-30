[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$CandidateManifest,
    [string]$Acceptance = 'goals/runtime-pp/acceptance.json'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$manifestPath = if ([IO.Path]::IsPathRooted($CandidateManifest)) { $CandidateManifest } else { Join-Path $root $CandidateManifest }
$acceptancePath = if ([IO.Path]::IsPathRooted($Acceptance)) { $Acceptance } else { Join-Path $root $Acceptance }
try {
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    $acceptanceState = Get-Content -LiteralPath $acceptancePath -Raw | ConvertFrom-Json
    $artifactBase = $root
    if ($manifest.PSObject.Properties.Name -contains 'artifactRoot') {
        if ([string]::IsNullOrWhiteSpace($manifest.artifactRoot)) { throw 'artifactRoot must not be empty' }
        $artifactBase = [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $manifestPath) $manifest.artifactRoot))
    }
    $checks = @()
    foreach ($property in $manifest.hashes.PSObject.Properties) {
        $path = Join-Path $artifactBase $property.Name
        $matches = (Test-Path -LiteralPath $path -PathType Leaf) -and ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -eq $property.Value)
        $checks += [pscustomobject]@{id=('artifact:' + $property.Name);status=$(if($matches){'PASS'}else{'FAIL'})}
    }
    if ($checks.Count -eq 0) { throw 'Candidate manifest has no artifact hashes' }
    foreach ($gate in $acceptanceState.gates) {
        if ($gate.status -notin @('PENDING','IN_PROGRESS','PASS','FAIL','BLOCKED')) { throw ('Invalid acceptance status: ' + $gate.id) }
        $checks += [pscustomobject]@{id=$gate.id;status=$gate.status}
    }
    if (@($acceptanceState.gates).Count -eq 0) { throw 'Acceptance list is empty' }
    $unresolved = @($checks | Where-Object status -ne 'PASS')
    [pscustomobject]@{version=$manifest.version;revision=$manifest.revision;utc=[DateTime]::UtcNow.ToString('o');status=$(if($unresolved.Count){'NON_PASS'}else{'PASS'});unresolved=$unresolved;checks=$checks} | ConvertTo-Json -Depth 6
    if ($unresolved.Count) { exit 2 }
    exit 0
} catch {
    [pscustomobject]@{status='FAIL';error=$_.Exception.Message} | ConvertTo-Json
    exit 1
}
