param(
    [string]$ExpectedBaseUrl = "http://ai.homelab.com:11434/v1",
    [string]$FallbackBaseUrl = "http://127.0.0.1:11434/v1",
    [string]$Model = "qwen3.6-35b-a3b",
    [switch]$SkipGeneration
)

$ErrorActionPreference = "Continue"

function Write-Check {
    param([string]$Name, [object]$Value)
    Write-Host ("{0}: {1}" -f $Name, $Value)
}

function Read-JsonFile {
    param([string]$Path)
    if ([string]::IsNullOrWhiteSpace($Path)) { return $null }
    if (-not (Test-Path -LiteralPath $Path)) { return $null }
    try {
        return Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    } catch {
        Write-Check "config parse error" "$Path :: $($_.Exception.Message)"
        return $null
    }
}

function Get-JsonProperty {
    param([object]$Object, [string[]]$Names)
    if ($null -eq $Object) { return $null }
    foreach ($name in $Names) {
        if ($Object.PSObject.Properties.Name -contains $name) {
            return $Object.$name
        }
    }
    return $null
}

function Find-ValueRecursive {
    param([object]$Object, [string[]]$Names)
    if ($null -eq $Object) { return $null }
    $direct = Get-JsonProperty -Object $Object -Names $Names
    if ($null -ne $direct -and "$direct" -ne "") { return $direct }
    foreach ($prop in $Object.PSObject.Properties) {
        $value = $prop.Value
        if ($null -eq $value -or $value -is [string]) { continue }
        if ($value -is [System.Collections.IEnumerable]) {
            foreach ($item in $value) {
                $found = Find-ValueRecursive -Object $item -Names $Names
                if ($null -ne $found -and "$found" -ne "") { return $found }
            }
        } else {
            $found = Find-ValueRecursive -Object $value -Names $Names
            if ($null -ne $found -and "$found" -ne "") { return $found }
        }
    }
    return $null
}

function Test-TcpPort {
    param([string]$HostName, [int]$Port)
    try {
        $client = [System.Net.Sockets.TcpClient]::new()
        $task = $client.ConnectAsync($HostName, $Port)
        if (-not $task.Wait(5000)) {
            $client.Dispose()
            return "timeout"
        }
        $client.Dispose()
        return "ok"
    } catch {
        return "failed: $($_.Exception.Message)"
    }
}

function Get-AssistantPreview {
    param([object]$Response)
    if ($null -eq $Response -or $null -eq $Response.choices -or $Response.choices.Count -lt 1) {
        return "no choices"
    }
    $message = $Response.choices[0].message
    $content = if ($message -and ($message.PSObject.Properties.Name -contains "content")) { $message.content } else { "" }
    $reasoning = if ($message -and ($message.PSObject.Properties.Name -contains "reasoning_content")) { $message.reasoning_content } else { "" }
    $finish = $Response.choices[0].finish_reason
    if (-not [string]::IsNullOrWhiteSpace($content)) { return "content=$content; finish=$finish" }
    if (-not [string]::IsNullOrWhiteSpace($reasoning)) { return "reasoning=$reasoning; finish=$finish" }
    return "empty assistant text; finish=$finish"
}

function Get-OllamaPreview {
    param([object]$Response)
    if ($null -eq $Response) { return "empty response" }
    $content = if ($Response.message -and ($Response.message.PSObject.Properties.Name -contains "content")) { $Response.message.content } else { "" }
    $thinking = if ($Response.message -and ($Response.message.PSObject.Properties.Name -contains "thinking")) { $Response.message.thinking } elseif ($Response.PSObject.Properties.Name -contains "thinking") { $Response.thinking } else { "" }
    if (-not [string]::IsNullOrWhiteSpace($content)) { return "content=$content" }
    if (-not [string]::IsNullOrWhiteSpace($thinking)) { return "thinking=$thinking" }
    return "empty assistant text"
}

$homeDir = [Environment]::GetFolderPath("UserProfile")
if ([string]::IsNullOrWhiteSpace($homeDir)) { $homeDir = $HOME }
$candidateConfigs = @(
    (Join-Path $homeDir ".config/opencode/opencode.json"),
    (Join-Path $homeDir ".config/opencode/config.json"),
    (Join-Path $homeDir "Library/Application Support/opencode/opencode.json"),
    (Join-Path $homeDir "AppData/Roaming/opencode/opencode.json"),
    (Join-Path (Get-Location) "opencode.json")
) | Select-Object -Unique

$configPath = $candidateConfigs | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
$config = Read-JsonFile -Path $configPath
$selectedModel = Get-JsonProperty -Object $config -Names @("model")
$provider = Find-ValueRecursive -Object $config -Names @("providerID", "providerId")
if (($null -eq $provider -or "$provider" -eq "") -and $selectedModel -and "$selectedModel".Contains("/")) {
    $provider = "$selectedModel".Split("/")[0]
}
if (($null -eq $provider -or "$provider" -eq "") -and $config -and ($config.PSObject.Properties.Name -contains "provider")) {
    $providerNames = @($config.provider.PSObject.Properties | ForEach-Object { $_.Name })
    if ($providerNames.Count -eq 1) { $provider = $providerNames[0] }
}
$baseUrl = $null
if ($provider -and $config -and ($config.PSObject.Properties.Name -contains "provider") -and
    ($config.provider.PSObject.Properties.Name -contains "$provider")) {
    $providerConfig = $config.provider."$provider"
    if ($providerConfig -and ($providerConfig.PSObject.Properties.Name -contains "options")) {
        $baseUrl = Get-JsonProperty -Object $providerConfig.options -Names @("baseURL", "baseUrl", "base_url")
    }
}
if ($null -eq $baseUrl -or "$baseUrl" -eq "") {
    $baseUrl = Find-ValueRecursive -Object $config -Names @("baseURL", "baseUrl", "base_url")
}

$configPathText = if ($configPath) { $configPath } else { "not found" }
$providerText = if ($provider) { $provider } else { "not found" }
$selectedModelText = if ($selectedModel) { $selectedModel } else { "not found" }
$baseUrlText = if ($baseUrl) { $baseUrl } else { "not found" }
Write-Check "config path" $configPathText
if (Get-Command opencode -ErrorAction SilentlyContinue) {
    try {
        Write-Check "opencode version" ((opencode --version 2>$null) -join " ")
    } catch {
        Write-Check "opencode version" "failed: $($_.Exception.Message)"
    }
} else {
    Write-Check "opencode version" "not found"
}
Write-Check "selected provider" $providerText
Write-Check "selected model" $selectedModelText
Write-Check "baseURL" $baseUrlText
$normalizedBaseUrl = if ($baseUrl) { "$baseUrl".TrimEnd("/") } else { "not found" }
Write-Check "normalized baseURL" $normalizedBaseUrl

if (-not $configPath) {
    Write-Host "FAIL: OpenCode config file was not found on this machine; cannot prove OpenCode targets InferDeck." -ForegroundColor Red
    $script:failed = $true
}

if (Get-Command opencode -ErrorAction SilentlyContinue) {
    try {
        $pathsOutput = opencode debug paths 2>$null
        $dataPath = ($pathsOutput | Where-Object { $_ -match '^data\s+' } | Select-Object -First 1) -replace '^data\s+', ''
        $logPath = ($pathsOutput | Where-Object { $_ -match '^log\s+' } | Select-Object -First 1) -replace '^log\s+', ''
        $tmpPath = ($pathsOutput | Where-Object { $_ -match '^tmp\s+' } | Select-Object -First 1) -replace '^tmp\s+', ''
        Write-Check "opencode data path" ($(if ($dataPath) { $dataPath } else { "unknown" }))
        Write-Check "opencode log path" ($(if ($logPath) { $logPath } else { "unknown" }))
        Write-Check "opencode tmp path" ($(if ($tmpPath) { $tmpPath } else { "unknown" }))
        if ($logPath -and (Test-Path -LiteralPath $logPath)) {
            $latestLog = Get-ChildItem -LiteralPath $logPath -Filter "*.log" -File -ErrorAction SilentlyContinue |
                Sort-Object LastWriteTime -Descending |
                Select-Object -First 1
            Write-Check "opencode latest log" ($(if ($latestLog) { $latestLog.FullName } else { "none" }))
            if ($latestLog) {
                $lines = Select-String -LiteralPath $latestLog.FullName -Pattern "ERROR|WARN|SQLITE|busy|failed|tool|read" -CaseSensitive:$false -ErrorAction SilentlyContinue |
                    Select-Object -Last 12 |
                    ForEach-Object { $_.Line }
                if ($lines) {
                    Write-Host "opencode latest relevant log lines:"
                    $lines | ForEach-Object { Write-Host $_ }
                } else {
                    Write-Check "opencode latest relevant log lines" "none"
                }
            }
        }
    } catch {
        Write-Check "opencode paths/logs" "failed: $($_.Exception.Message)"
    }
}

if ([string]::IsNullOrWhiteSpace($selectedModel)) {
    Write-Check "selected model warning" "no top-level model set; OpenCode may use CLI/session/default model"
}

if (-not [string]::IsNullOrWhiteSpace($selectedModel) -and "$selectedModel".Contains("/") -and
    -not [string]::IsNullOrWhiteSpace($provider) -and "$selectedModel".Split("/")[0] -ne "$provider") {
    Write-Host "FAIL: OpenCode selected model provider prefix ($("$selectedModel".Split("/")[0])) does not match selected provider ($provider)." -ForegroundColor Red
    $script:failed = $true
}

$allowed = @($ExpectedBaseUrl.TrimEnd("/"), $FallbackBaseUrl.TrimEnd("/"))
if ($null -eq $baseUrl -or $allowed -notcontains "$baseUrl".TrimEnd("/")) {
    Write-Host "FAIL: OpenCode provider/baseURL is not targeting InferDeck. Expected $ExpectedBaseUrl or $FallbackBaseUrl." -ForegroundColor Red
    $script:failed = $true
}

try {
    $addresses = [System.Net.Dns]::GetHostAddresses("ai.homelab.com") | ForEach-Object { $_.IPAddressToString }
    Write-Check "ai.homelab.com DNS/IP" ($addresses -join ", ")
} catch {
    Write-Check "ai.homelab.com DNS/IP" "failed: $($_.Exception.Message)"
    $script:failed = $true
}

$tcpResult = Test-TcpPort -HostName "ai.homelab.com" -Port 11434
Write-Check "TCP ai.homelab.com:11434" $tcpResult
if ($tcpResult -ne "ok") { $script:failed = $true }

$apiBase = if ($baseUrl) { "$baseUrl".TrimEnd("/") } else { $ExpectedBaseUrl.TrimEnd("/") }
$rootBase = $apiBase -replace "/v1$",""

try {
    $models = Invoke-RestMethod -Method Get -Uri "$apiBase/models" -TimeoutSec 15
    Write-Check "/v1/models result" (($models.data | Select-Object -First 5 | ForEach-Object { $_.id }) -join ", ")
} catch {
    Write-Check "/v1/models result" "FAILED: $($_.Exception.Message)"
    $script:failed = $true
}

if (-not $SkipGeneration) {
    $chatBody = @{
        model = $Model
        stream = $false
        max_tokens = 16
        messages = @(@{ role = "user"; content = "Reply with exactly: inferdeck opencode diag ok" })
    } | ConvertTo-Json -Depth 8
    try {
        $chat = Invoke-RestMethod -Method Post -Uri "$apiBase/chat/completions" -Body $chatBody -ContentType "application/json" -TimeoutSec 180
        Write-Check "/v1/chat/completions tiny result" (Get-AssistantPreview -Response $chat)
    } catch {
        Write-Check "/v1/chat/completions tiny result" "FAILED: $($_.Exception.Message)"
        $script:failed = $true
    }

    $ollamaBody = @{
        model = $Model
        stream = $false
        messages = @(@{ role = "user"; content = "Reply with exactly: inferdeck webui diag ok" })
        options = @{ num_predict = 16 }
    } | ConvertTo-Json -Depth 8
    try {
        $ollama = Invoke-RestMethod -Method Post -Uri "$rootBase/api/chat" -Body $ollamaBody -ContentType "application/json" -TimeoutSec 180
        Write-Check "/api/chat Open WebUI control result" (Get-OllamaPreview -Response $ollama)
    } catch {
        Write-Check "/api/chat Open WebUI control result" "FAILED: $($_.Exception.Message)"
        $script:failed = $true
    }
}

if ($script:failed) { exit 1 }
Write-Host "OpenCode InferDeck diagnostics passed."
