param([string]$SourceRoot,[string]$BackupRoot,[string]$ResultPath)
$ErrorActionPreference='Stop'
function CopyTree($from,$to){New-Item -ItemType Directory -Path $to -Force|Out-Null;foreach($f in Get-ChildItem -LiteralPath $from -Recurse -File){$r=$f.FullName.Substring($from.Length).TrimStart('\');$d=Join-Path $to $r;New-Item -ItemType Directory -Path (Split-Path $d -Parent) -Force|Out-Null;Copy-Item -LiteralPath $f.FullName -Destination $d -Force}}
function WaitState($state){$end=[DateTime]::UtcNow.AddSeconds(120);do{if((Get-Service InferDeck).Status.ToString()-eq $state){return};Start-Sleep -Milliseconds 500}while([DateTime]::UtcNow-lt$end);throw "InferDeck did not reach $state"}
$admin=New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if(-not$admin.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){throw 'Elevation required'}
$source=[IO.Path]::GetFullPath($SourceRoot);$backup=[IO.Path]::GetFullPath($BackupRoot);$result=[IO.Path]::GetFullPath($ResultPath);$live='C:\InferDeck'
if($source-ne'C:\tmp\inferdeck-081-release-v0.8.1\payload'){throw "Bad source $source"}
if(-not$backup.StartsWith('C:\InferDeck\deploy-backups\v0.8.1-exact-',[StringComparison]::OrdinalIgnoreCase)){throw "Bad backup $backup"}
if(Test-Path -LiteralPath $backup){throw "Backup exists $backup"}
if($result-ne'C:\tmp\inferdeck-v081-deploy-result.json'){throw "Bad result $result"}
$manifest=Get-Content -LiteralPath (Join-Path $source 'release-manifest.json') -Raw|ConvertFrom-Json
if($manifest.version-ne'0.8.1'-or$manifest.commit-ne'ecea086c2c28cce3822e63b637725a0b378d71e2'){throw 'Bad manifest identity'}
$expectedExe='5C5A6C800318E850829FF3BF3DA5D428757246FE5EDCB277F370271DB9FC3C9E'
if((Get-FileHash -LiteralPath (Join-Path $source 'inferdeck-gateway.exe') -Algorithm SHA256).Hash-ne$expectedExe){throw 'Bad source executable'}
foreach($a in $manifest.artifacts){$p=Join-Path $source ($a.path-replace'/','\');if(-not(Test-Path -LiteralPath $p -PathType Leaf)){throw "Missing $($a.path)"};if((Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash.ToLowerInvariant()-ne$a.sha256.ToLowerInvariant()){throw "Bad source hash $($a.path)"}}
$reg=Get-ItemProperty -LiteralPath 'HKLM:\SYSTEM\CurrentControlSet\Services\InferDeck\Parameters'
if([IO.Path]::GetFullPath([string]$reg.Application)-ne'C:\InferDeck\inferdeck-gateway.exe'-or[string]$reg.AppParameters-ne'-c config\gateway.yml'-or[IO.Path]::GetFullPath([string]$reg.AppDirectory)-ne'C:\InferDeck'){throw 'Unexpected service definition'}
$stats=Join-Path $live 'data\stats.db';$config=Join-Path $live 'config\gateway.yml';$active=Join-Path $live 'config\gateway.active.yml';$pricing=Join-Path $live 'data\pricing.json';$templates=Join-Path $live 'config\templates';$static=Join-Path $live 'static';$stage=Join-Path $live 'static.v081.staging'
$ready=$false;$swapped=$false
try{
 if(Test-Path -LiteralPath $result){Remove-Item -LiteralPath $result -Force};if(Test-Path -LiteralPath $stage){throw "Staging exists $stage"}
 Stop-Service InferDeck;WaitState 'Stopped'
 $dbHash=(Get-FileHash -LiteralPath $stats -Algorithm SHA256).Hash;$dbBytes=(Get-Item -LiteralPath $stats).Length
 New-Item -ItemType Directory -Path $backup|Out-Null;New-Item -ItemType Directory -Path (Join-Path $backup 'root'),(Join-Path $backup 'config'),(Join-Path $backup 'data')|Out-Null;$ready=$true
 Copy-Item -LiteralPath $stats -Destination (Join-Path $backup 'data\stats.db');if((Get-FileHash -LiteralPath (Join-Path $backup 'data\stats.db') -Algorithm SHA256).Hash-ne$dbHash){throw 'StatsDb backup mismatch'}
 Copy-Item -LiteralPath $config -Destination (Join-Path $backup 'config\gateway.yml');Copy-Item -LiteralPath $active -Destination (Join-Path $backup 'config\gateway.active.yml')
 if(Test-Path -LiteralPath $pricing){Copy-Item -LiteralPath $pricing -Destination (Join-Path $backup 'data\pricing.json')};if(Test-Path -LiteralPath $templates){CopyTree $templates (Join-Path $backup 'config\templates')}
 foreach($a in $manifest.artifacts){if($a.path-match'[/\\]'){continue};$p=Join-Path $live $a.path;if(Test-Path -LiteralPath $p -PathType Leaf){Copy-Item -LiteralPath $p -Destination (Join-Path $backup "root\$($a.path)")}}
 CopyTree (Join-Path $source 'static') $stage;if(Test-Path -LiteralPath $static){Move-Item -LiteralPath $static -Destination (Join-Path $backup 'static')};Move-Item -LiteralPath $stage -Destination $static;$swapped=$true
 foreach($a in $manifest.artifacts){if($a.path-eq'config/gateway.yml'-or$a.path.StartsWith('static/')){continue};$f=Join-Path $source ($a.path-replace'/','\');$d=Join-Path $live ($a.path-replace'/','\');New-Item -ItemType Directory -Path (Split-Path $d -Parent) -Force|Out-Null;Copy-Item -LiteralPath $f -Destination $d -Force}
 foreach($a in $manifest.artifacts){if($a.path-eq'config/gateway.yml'){continue};$p=Join-Path $live ($a.path-replace'/','\');if((Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash.ToLowerInvariant()-ne$a.sha256.ToLowerInvariant()){throw "Bad deployed hash $($a.path)"}}
 if((Get-FileHash -LiteralPath $config -Algorithm SHA256).Hash-ne(Get-FileHash -LiteralPath (Join-Path $backup 'config\gateway.yml') -Algorithm SHA256).Hash){throw 'Gateway config changed'}
 if((Get-FileHash -LiteralPath $active -Algorithm SHA256).Hash-ne(Get-FileHash -LiteralPath (Join-Path $backup 'config\gateway.active.yml') -Algorithm SHA256).Hash){throw 'Active config changed'}
 $version=& (Join-Path $live 'inferdeck-gateway.exe') --version;if($LASTEXITCODE-ne0-or$version-ne'inferdeck-gateway 0.8.1'){throw "Bad version $version"}
 Start-Service InferDeck;WaitState 'Running'
 [ordered]@{ok=$true;backup=$backup;statsDbSha256=$dbHash;statsDbBytes=$dbBytes;executableSha256=(Get-FileHash -LiteralPath (Join-Path $live 'inferdeck-gateway.exe') -Algorithm SHA256).Hash;executableVersion=$version;sourceCommit=$manifest.commit;service=(Get-Service InferDeck).Status.ToString()}|ConvertTo-Json|Set-Content -LiteralPath $result -Encoding utf8
}catch{
 $failure=$_;try{if((Get-Service InferDeck).Status-ne'Stopped'){Stop-Service InferDeck;WaitState 'Stopped'};if($ready){foreach($f in Get-ChildItem -LiteralPath (Join-Path $backup 'root') -File -ErrorAction SilentlyContinue){Copy-Item -LiteralPath $f.FullName -Destination (Join-Path $live $f.Name) -Force};if(Test-Path -LiteralPath (Join-Path $backup 'config\templates')){CopyTree (Join-Path $backup 'config\templates') $templates};if(Test-Path -LiteralPath (Join-Path $backup 'data\pricing.json')){Copy-Item -LiteralPath (Join-Path $backup 'data\pricing.json') -Destination $pricing -Force};if($swapped-and(Test-Path -LiteralPath (Join-Path $backup 'static'))){if(Test-Path -LiteralPath $static){Move-Item -LiteralPath $static -Destination (Join-Path $backup 'failed-static')};Move-Item -LiteralPath (Join-Path $backup 'static') -Destination $static}};Start-Service InferDeck;WaitState 'Running'}catch{};[ordered]@{ok=$false;error=$failure.Exception.Message;backup=$backup}|ConvertTo-Json|Set-Content -LiteralPath $result -Encoding utf8;throw $failure
}
