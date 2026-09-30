[CmdletBinding()]
param([switch]$Activate)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
[Net.ServicePointManager]::Expect100Continue=$false
if(!$Activate){Write-Output 'Validated preparation only; use -Activate to install.';exit 0}
$root='C:\InferDeck\mcp'
$source=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if(Test-Path -LiteralPath $root){throw 'MCP runtime exists; refusing replacement'}
if(Get-Service InferDeckMcp -ErrorAction SilentlyContinue){throw 'MCP service exists'}
if(@(Get-NetTCPConnection -LocalPort 11436 -ErrorAction SilentlyContinue).Count){throw 'MCP port occupied'}
$node=(Get-Command node.exe).Source
$serviceImage=(Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\InferDeck').ImagePath
$nssm=[regex]::Match($serviceImage,'(?i)^"?(.+?nssm\.exe)"?').Groups[1].Value
if(!(Test-Path -LiteralPath $nssm)){throw 'NSSM not found'}
function Protect-Directory([string]$Path) {
    $acl=New-Object System.Security.AccessControl.DirectorySecurity
    $acl.SetAccessRuleProtection($true,$false)
    foreach($sid in @('S-1-5-18','S-1-5-32-544')) {
        $identity=New-Object System.Security.Principal.SecurityIdentifier($sid)
        $rule=New-Object System.Security.AccessControl.FileSystemAccessRule($identity,'FullControl','ContainerInherit,ObjectInherit','None','Allow')
        $acl.AddAccessRule($rule)
    }
    Set-Acl -LiteralPath $Path -AclObject $acl
}
function Nssm([string[]]$Arguments) {
    & $nssm @Arguments | Out-Null
    if($LASTEXITCODE -ne 0){throw ('NSSM failed: '+$Arguments[0])}
}
$config=if(Test-Path 'C:\InferDeck\config\gateway.active.yml'){'C:\InferDeck\config\gateway.active.yml'}else{'C:\InferDeck\config\gateway.yml'}
$reader=Join-Path $PSScriptRoot 'read-local-control.cjs'
$control=& $node $reader $config
if($LASTEXITCODE -ne 0 -or !$control){throw 'Live control credential unavailable'}
$headers=@{Authorization="Bearer $control";Origin='http://127.0.0.1:11434'}
$runtimeCreated=$false
$created=$false
$firewall=$false
$keyId=$null
try {
    New-Item -ItemType Directory -Path $root | Out-Null
    $runtimeCreated=$true
    Protect-Directory $root
    foreach($name in @('src','package.json','package-lock.json')) {
        if(Test-Path (Join-Path $source $name)){Copy-Item -LiteralPath (Join-Path $source $name) -Destination $root -Recurse}
    }
    & npm.cmd ci --omit=dev --ignore-scripts --no-audit --no-fund --prefix $root
    if($LASTEXITCODE -ne 0){throw 'MCP dependency installation failed'}
    if(@(Get-ChildItem $root -Recurse -Force | Where-Object {$_.Attributes -band [IO.FileAttributes]::ReparsePoint}).Count){throw 'Unexpected runtime reparse point'}
    $writeRights=[Security.AccessControl.FileSystemRights]::Write -bor [Security.AccessControl.FileSystemRights]::Modify -bor [Security.AccessControl.FileSystemRights]::Delete
    foreach($item in @((Get-Item -LiteralPath $root)) + @(Get-ChildItem -LiteralPath $root -Recurse -Force)) {
        foreach($rule in (Get-Acl -LiteralPath $item.FullName).Access) {
            $sid=$rule.IdentityReference.Translate([Security.Principal.SecurityIdentifier]).Value
            if($sid -in @('S-1-1-0','S-1-5-11','S-1-5-32-545') -and $rule.AccessControlType -eq 'Allow' -and ($rule.FileSystemRights -band $writeRights)) {
                throw 'MCP runtime inherited broad write permissions'
            }
        }
    }

    $key=Invoke-RestMethod 'http://127.0.0.1:11434/api/inferdeck/v1/api-keys' -Method Post -Headers $headers -ContentType 'application/json' -Body '{"name":"InferDeck MCP","priority":0}' -TimeoutSec 15
    $keyId=$key.id
    if(!$key.key){throw 'Key creation returned no credential'}
    $bytes=New-Object byte[] 32
    $rng=[Security.Cryptography.RandomNumberGenerator]::Create()
    try{$rng.GetBytes($bytes)}finally{$rng.Dispose()}
    $token=[Convert]::ToBase64String($bytes).TrimEnd('=').Replace('+','-').Replace('/','_')
    $hosts="localhost,127.0.0.1,192.168.0.168,$([Environment]::MachineName.ToLowerInvariant())"
    $environment=@(
      'INFERDECK_URL=http://127.0.0.1:11434',
      "INFERDECK_API_KEY=$($key.key)",
      "INFERDECK_CONTROL_TOKEN=$control",
      "MCP_BEARER_TOKEN=$token",
      'MCP_BIND_HOST=0.0.0.0',
      'MCP_PORT=11436',
      "MCP_ALLOWED_HOSTS=$hosts",
      "MCP_ALLOWED_ORIGINS=$hosts",
      'MCP_ENABLE_MEDIA=true',
      'MCP_GENERATION_TIMEOUT_MS=1800000',
      'INFERDECK_PUBLIC_URL=http://192.168.0.168:11434',
      'INFERDECK_MCP_PUBLIC_URL=http://192.168.0.168:11436/mcp'
    )
    $environment | Set-Content -LiteralPath "$root\connection.env" -Encoding ascii
    Nssm @('install','InferDeckMcp',$node,"$root\src\index.mjs")
    $created=$true
    Nssm @('set','InferDeckMcp','AppDirectory',$root)
    Nssm @('set','InferDeckMcp','Start','SERVICE_AUTO_START')
    Nssm @('set','InferDeckMcp','AppExit','Default','Restart')
    Nssm @('set','InferDeckMcp','AppStdout',"$root\stdout.log")
    Nssm @('set','InferDeckMcp','AppStderr',"$root\stderr.log")
    $registry='HKLM:\SYSTEM\CurrentControlSet\Services\InferDeckMcp\Parameters'
    $acl=New-Object System.Security.AccessControl.RegistrySecurity
    $acl.SetAccessRuleProtection($true,$false)
    foreach($sid in @('S-1-5-18','S-1-5-32-544')) {
      $identity=New-Object System.Security.Principal.SecurityIdentifier($sid)
      $acl.AddAccessRule((New-Object System.Security.AccessControl.RegistryAccessRule($identity,'FullControl','Allow')))
    }
    if(!(Test-Path -LiteralPath $registry)){New-Item -Path $registry | Out-Null}
    Set-Acl -LiteralPath $registry -AclObject $acl
    New-ItemProperty -LiteralPath $registry -Name AppEnvironmentExtra -PropertyType MultiString -Value $environment -Force | Out-Null
    New-NetFirewallRule -DisplayName 'InferDeck MCP local subnet' -Direction Inbound -Action Allow -Protocol TCP -LocalPort 11436 -LocalAddress 192.168.0.168 -RemoteAddress 192.168.0.0/24 -Profile Any | Out-Null
    $firewall=$true
    $saved=Get-ItemProperty -LiteralPath $registry
    if($saved.Application -ne $node -or !$saved.AppParameters){throw 'MCP launch configuration is incomplete'}
    Start-Service InferDeckMcp
    (Get-Service InferDeckMcp).WaitForStatus('Running',[TimeSpan]::FromSeconds(30))
    Write-Output 'MCP service started at port 11436; credentials saved in protected connection.env.'
}catch {
    if($firewall){Remove-NetFirewallRule -DisplayName 'InferDeck MCP local subnet' -ErrorAction SilentlyContinue}
    if($created){Stop-Service InferDeckMcp -ErrorAction SilentlyContinue; & $nssm remove InferDeckMcp confirm | Out-Null}
    if($keyId){try{Invoke-RestMethod "http://127.0.0.1:11434/api/inferdeck/v1/api-keys/$keyId" -Method Delete -Headers $headers -TimeoutSec 10 | Out-Null}catch{}}
    if($runtimeCreated -and (Test-Path -LiteralPath $root)){
      $resolved=(Resolve-Path -LiteralPath $root).Path
      if($resolved -ne 'C:\InferDeck\mcp'){throw 'Unsafe cleanup path'}
      Remove-Item -LiteralPath $resolved -Recurse -Force
    }
    throw
}
