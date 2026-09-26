[CmdletBinding(SupportsShouldProcess)]
param(
    [switch]$Install,
    [switch]$ValidateOnly,
    [string]$ServiceName = 'InferDeckMcp',
    [string]$NssmPath,
    [string]$NodePath,
    [string]$McpRoot,
    [string]$BindHost = '127.0.0.1',
    [string[]]$AllowedHosts,
    [string[]]$AllowedOrigins,
    [int]$Port = 11436,
    [string]$GatewayUrl = 'http://127.0.0.1:11434',
    [securestring]$ApiKey,
    [securestring]$ControlToken,
    [securestring]$McpToken,
    [int]$GenerationTimeoutMs = 300000,
    [switch]$EnableMedia
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Convert-SecureValue {
    param([securestring]$Value)

    $pointer = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($Value)
    try {
        return [Runtime.InteropServices.Marshal]::PtrToStringBSTR($pointer)
    } finally {
        [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($pointer)
    }
}

function Read-RequiredSecret {
    param([string]$Name, [securestring]$Value)

    if ($Value) { return $Value }
    return Read-Host -Prompt "$Name is required" -AsSecureString
}

function Find-Nssm {
    param([string]$RequestedPath)

    $candidates = @($RequestedPath, 'C:\InferDeck\nssm.exe')
    try {
        $imagePath = (Get-ItemProperty -LiteralPath 'HKLM:\SYSTEM\CurrentControlSet\Services\InferDeck' -Name ImagePath).ImagePath
        if ($imagePath -match '"?([^" ]*nssm\.exe)') {
            $candidates += $Matches[1]
        }
    } catch {
    }

    $candidates += @('C:\Program Files\nssm\nssm.exe', 'C:\Program Files (x86)\nssm\nssm.exe')
    foreach ($candidate in ($candidates | Where-Object { $_ })) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }

    throw 'nssm.exe was not found. Supply -NssmPath.'
}

function Find-Node {
    param([string]$RequestedPath)

    $path = if ($RequestedPath) {
        $RequestedPath
    } else {
        $command = Get-Command node.exe -ErrorAction SilentlyContinue
        if (!$command) { throw 'node.exe was not found. Supply -NodePath.' }
        $command.Source
    }

    if (!(Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Node executable was not found: $path"
    }

    $resolved = (Resolve-Path -LiteralPath $path).Path
    $version = (& $resolved --version).Trim()
    if ($LASTEXITCODE -ne 0 -or $version -notmatch '^v(\d+)\.') {
        throw 'Unable to determine the Node.js version.'
    }
    if ([int]$Matches[1] -lt 22) {
        throw "Node.js 22 or newer is required; found $version."
    }
    return $resolved
}

function Get-HostList {
    param([string]$Name, [string[]]$Values)

    if (!$Values) { throw "$Name is required for a LAN binding." }
    foreach ($value in $Values) {
        if (!$value -or $value -match '[\s/]') {
            throw "$Name contains an invalid host value."
        }
    }
    return @($Values | Select-Object -Unique)
}

function Assert-ProtectedRuntimeTree {
    param([string]$Path)

    if (!$Path -or !(Test-Path -LiteralPath $Path -PathType Container)) {
        throw "McpRoot must be an existing protected directory: $Path"
    }

    $resolvedRoot = (Resolve-Path -LiteralPath $Path).Path
    $usersSid = New-Object System.Security.Principal.SecurityIdentifier('S-1-5-32-545')
    $everyoneSid = New-Object System.Security.Principal.SecurityIdentifier('S-1-1-0')
    $authenticatedUsersSid = New-Object System.Security.Principal.SecurityIdentifier('S-1-5-11')
    $creatorOwnerSid = New-Object System.Security.Principal.SecurityIdentifier('S-1-3-0')

    $paths = @(Get-Item -LiteralPath $resolvedRoot) + @(Get-ChildItem -LiteralPath $resolvedRoot -Force -Recurse)
    foreach ($item in $paths) {
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw "McpRoot contains a reparse point: $($item.FullName)"
        }
        foreach ($rule in (Get-Acl -LiteralPath $item.FullName).Access) {
            $sid = $rule.IdentityReference.Translate([System.Security.Principal.SecurityIdentifier])
            $broadSid = $sid.Value -in @($usersSid.Value, $everyoneSid.Value, $authenticatedUsersSid.Value, $creatorOwnerSid.Value)
            $writes = $rule.FileSystemRights.ToString() -match 'Write|Modify|FullControl|Delete'
            if ($rule.AccessControlType -eq 'Allow' -and $broadSid -and $writes) {
                throw "McpRoot has broad write access on $($item.FullName)."
            }
        }
    }
    return $resolvedRoot
}

function Invoke-Nssm {
    param([string]$Path, [string[]]$Arguments)

    & $Path @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "NSSM operation failed: $($Arguments[0]) (exit code $LASTEXITCODE)."
    }
}

if ($Install -and $ValidateOnly) { throw 'Choose -Install or -ValidateOnly, not both.' }
if (!$Install -and !$ValidateOnly) { $ValidateOnly = $true }
if ($ServiceName -ne 'InferDeckMcp') { throw 'The service name is fixed at InferDeckMcp.' }
if ($Install -and !$McpRoot) { throw '-McpRoot is required for installation.' }
if ($Port -lt 1 -or $Port -gt 65535) { throw 'Port must be between 1 and 65535.' }
if ($GenerationTimeoutMs -lt 1 -or $GenerationTimeoutMs -gt 1800000) { throw 'GenerationTimeoutMs must be between 1 and 1800000.' }
if ($GatewayUrl -notmatch '^https?://127\.0\.0\.1(?::\d+)?/?$') { throw 'GatewayUrl must be loopback HTTP(S).' }

$isLoopback = $BindHost -in @('127.0.0.1', 'localhost', '::1', '[::1]')
$hostList = if ($isLoopback -and !$AllowedHosts) {
    @('localhost', '127.0.0.1', '[::1]')
} else {
    Get-HostList 'AllowedHosts' $AllowedHosts
}
$originList = if ($isLoopback -and !$AllowedOrigins) {
    $hostList
} else {
    Get-HostList 'AllowedOrigins' $AllowedOrigins
}


$apiText = Convert-SecureValue (Read-RequiredSecret 'INFERDECK_API_KEY' $ApiKey)
$controlText = Convert-SecureValue (Read-RequiredSecret 'INFERDECK_CONTROL_TOKEN' $ControlToken)
$mcpText = Convert-SecureValue (Read-RequiredSecret 'MCP_BEARER_TOKEN' $McpToken)
if ($mcpText.Length -lt 32) {
    throw 'MCP_BEARER_TOKEN must be at least 32 characters.'
}
$node = Find-Node $NodePath
$runtimeRoot = if ($McpRoot) {
    Assert-ProtectedRuntimeTree $McpRoot
} else {
    (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
}
$entryPoint = Join-Path $runtimeRoot 'src\index.mjs'
if (!(Test-Path -LiteralPath $entryPoint -PathType Leaf)) {
    throw "MCP entry point was not found: $entryPoint"
}

$occupied = @(Get-NetTCPConnection -LocalPort $Port -ErrorAction SilentlyContinue) + @(Get-NetUDPEndpoint -LocalPort $Port -ErrorAction SilentlyContinue)
if ($occupied.Count -gt 0) {
    throw "MCP port $Port is occupied; no process was stopped."
}
if (!(Test-Path -LiteralPath 'HKLM:\SYSTEM\CurrentControlSet\Services\InferDeck\Parameters')) {
    throw 'Existing InferDeck service registry target was not found.'
}

if ($ValidateOnly) {
    [pscustomobject]@{
        mode = 'validate-only'
        service = $ServiceName
        node = $node
        runtimeRoot = $runtimeRoot
        bindHost = $BindHost
        port = $Port
        allowedHosts = $hostList -join ','
        allowedOrigins = $originList -join ','
        gatewayUrl = $GatewayUrl
    } | Format-List
    exit 0
}

$nssm = Find-Nssm $NssmPath
if (Get-Service -Name $ServiceName -ErrorAction SilentlyContinue) {
    throw "Service $ServiceName already exists; refusing to modify it."
}

$createdService = $false
try {
    Invoke-Nssm $nssm @('install', $ServiceName, $node, '--enable-source-maps', $entryPoint)
    $createdService = $true
    Invoke-Nssm $nssm @('set', $ServiceName, 'AppDirectory', $runtimeRoot)
    Invoke-Nssm $nssm @('set', $ServiceName, 'Start', 'SERVICE_AUTO_START')
    Invoke-Nssm $nssm @('set', $ServiceName, 'AppExit', 'Default', 'Restart')

    $serviceKey = "HKLM:\SYSTEM\CurrentControlSet\Services\$ServiceName\Parameters"
    New-Item -Path $serviceKey -Force | Out-Null
    $acl = Get-Acl -LiteralPath $serviceKey
    $acl.SetAccessRuleProtection($true, $false)
    $acl.Access | ForEach-Object { $acl.RemoveAccessRule($_) | Out-Null }
    $acl.AddAccessRule((New-Object System.Security.AccessControl.RegistryAccessRule('S-1-5-18', 'FullControl', 'Allow')))
    $acl.AddAccessRule((New-Object System.Security.AccessControl.RegistryAccessRule('S-1-5-32-544', 'FullControl', 'Allow')))
    Set-Acl -LiteralPath $serviceKey -AclObject $acl

    $environment = @(
        "INFERDECK_URL=$GatewayUrl",
        "INFERDECK_API_KEY=$apiText",
        "INFERDECK_CONTROL_TOKEN=$controlText",
        "MCP_BEARER_TOKEN=$mcpText",
        "MCP_BIND_HOST=$BindHost",
        "MCP_PORT=$Port",
        "MCP_ALLOWED_HOSTS=$($hostList -join ',')",
        "MCP_ALLOWED_ORIGINS=$($originList -join ',')",
        "MCP_ENABLE_MEDIA=$($EnableMedia.ToString().ToLowerInvariant())",
        "MCP_GENERATION_TIMEOUT_MS=$GenerationTimeoutMs"
    )
    New-ItemProperty -Path $serviceKey -Name AppEnvironmentExtra -PropertyType MultiString -Value $environment -Force | Out-Null
    Write-Output "Installed $ServiceName as Automatic and left it stopped. No firewall rule was created."
} catch {
    if ($createdService) {
        & $nssm remove $ServiceName confirm | Out-Null
    }
    throw
}