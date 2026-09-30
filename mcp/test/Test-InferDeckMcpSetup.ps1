$ErrorActionPreference = 'Stop'
$script = Join-Path $PSScriptRoot '..\scripts\Install-InferDeckMcp.ps1'
$node = (Get-Command node.exe).Source
$secure = ConvertTo-SecureString ('x' * 32) -AsPlainText -Force
$short = ConvertTo-SecureString ('x' * 8) -AsPlainText -Force

function Assert-Fails {
    param([scriptblock]$Action, [string]$Expected)
    try { & $Action } catch {
        if ($_.Exception.Message -notlike "*$Expected*") { throw "Expected '$Expected', got '$($_.Exception.Message)'" }
        return
    }
    throw "Expected failure containing '$Expected'."
}

& $script -ValidateOnly -Port 64999 -BindHost 192.0.2.10 -AllowedHosts @('machine-a','machine-b','machine-c') -AllowedOrigins @('machine-a','machine-b','machine-c') -ApiKey $secure -ControlToken $secure -McpToken $secure -NodePath $node | Out-Null
Assert-Fails { & $script -ValidateOnly -Port 64998 -BindHost 192.0.2.10 -ApiKey $secure -ControlToken $secure -McpToken $secure -NodePath $node } 'AllowedHosts is required'
Assert-Fails { & $script -ValidateOnly -Port 64997 -BindHost 192.0.2.10 -AllowedHosts 'machine a' -AllowedOrigins 'machine-a' -ApiKey $secure -ControlToken $secure -McpToken $secure -NodePath $node } 'invalid host value'
Assert-Fails { & $script -ValidateOnly -Port 64996 -BindHost 192.0.2.10 -AllowedHosts 'machine-a' -AllowedOrigins 'machine-a' -ApiKey $secure -ControlToken $secure -McpToken $short -NodePath $node } 'MCP_BEARER_TOKEN must be at least 32 characters'
& $script -ValidateOnly -Port 64995 -BindHost 192.0.2.10 -AllowedHosts 'machine-a' -AllowedOrigins 'machine-a' -ApiKey $short -ControlToken $short -McpToken $secure -NodePath $node | Out-Null
Assert-Fails { & $script -ValidateOnly -Port 64994 -GenerationTimeoutMs 1800001 -ApiKey $secure -ControlToken $secure -McpToken $secure -NodePath $node } 'GenerationTimeoutMs'

$listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
$listener.Start()
try { Assert-Fails { & $script -ValidateOnly -Port $listener.LocalEndpoint.Port -ApiKey $secure -ControlToken $secure -McpToken $secure -NodePath $node } 'is occupied' } finally { $listener.Stop() }

$root = Join-Path ([IO.Path]::GetTempPath()) ('inferdeck-mcp-test-' + [Guid]::NewGuid().ToString('N'))
$mock = Join-Path $root 'mock-nssm.cmd'
$marker = Join-Path $root 'removed.txt'
New-Item -ItemType Directory -Path (Join-Path $root 'src') -Force | Out-Null
Copy-Item (Join-Path $PSScriptRoot '..\src\index.mjs') (Join-Path $root 'src\index.mjs')
Set-Content -LiteralPath $mock -Value "@echo off`r`nif `"%1`"==`"install`" exit /b 0`r`nif `"%1`"==`"set`" exit /b 7`r`nif `"%1`"==`"remove`" echo removed> `"$marker`" & exit /b 0`r`nexit /b 0" -NoNewline
$acl = Get-Acl -LiteralPath $root
$systemSid = New-Object System.Security.Principal.SecurityIdentifier('S-1-5-18')
$administratorsSid = New-Object System.Security.Principal.SecurityIdentifier('S-1-5-32-544')
$acl.SetAccessRuleProtection($true, $false)
$acl.Access | ForEach-Object { $acl.RemoveAccessRule($_) | Out-Null }
$acl.AddAccessRule([System.Security.AccessControl.FileSystemAccessRule]::new($systemSid, 'FullControl', 'ContainerInherit,ObjectInherit', 'None', 'Allow'))
$acl.AddAccessRule([System.Security.AccessControl.FileSystemAccessRule]::new($administratorsSid, 'FullControl', 'ContainerInherit,ObjectInherit', 'None', 'Allow'))
Set-Acl -LiteralPath $root -AclObject $acl
try {
    Assert-Fails { & $script -Install -McpRoot $root -NssmPath $mock -NodePath $node -ApiKey $secure -ControlToken $secure -McpToken $secure } 'NSSM operation failed: set'
    if (!(Test-Path -LiteralPath $marker)) { throw 'Expected cleanup of the newly created mocked service.' }
} finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Output 'InferDeck MCP setup validation tests passed.'