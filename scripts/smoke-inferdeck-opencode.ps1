param(
    [string]$BaseUrl = "http://127.0.0.1:11434",
    [string]$Model = "qwen3.6-27b",
    [string]$ExpectedExecutablePath = "",
    [switch]$SkipGeneration,
    [switch]$RunDisconnectSmoke
)

$ErrorActionPreference = "Stop"

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) {
        throw $Message
    }
}

function Invoke-Json {
    param(
        [string]$Method,
        [string]$Uri,
        [object]$Body = $null,
        [int]$TimeoutSec = 30
    )
    $parameters = @{
        Method = $Method
        Uri = $Uri
        TimeoutSec = $TimeoutSec
    }
    if ($null -ne $Body) {
        $parameters.Body = $Body | ConvertTo-Json -Depth 16 -Compress
        $parameters.ContentType = "application/json"
    }
    Invoke-RestMethod @parameters
}

$uri = [Uri]$BaseUrl
$listener = Get-NetTCPConnection -LocalPort $uri.Port -State Listen -ErrorAction SilentlyContinue |
    Where-Object {
        $_.LocalAddress -eq $uri.Host -or
        ($uri.Host -eq "127.0.0.1" -and $_.LocalAddress -in @("0.0.0.0", "::"))
    } |
    Select-Object -First 1
Assert-True ($null -ne $listener) "No InferDeck listener found on port $($uri.Port)."

$process = Get-CimInstance Win32_Process -Filter "ProcessId = $($listener.OwningProcess)"
Assert-True ($null -ne $process) "The InferDeck listener owner could not be inspected."
$executableName = [System.IO.Path]::GetFileName($process.ExecutablePath)
Assert-True ($executableName -in @("inferdeck-gateway.exe", "gateway-service.exe")) `
    "Port $($uri.Port) is owned by $executableName, not InferDeck."
if ($ExpectedExecutablePath) {
    $expected = [System.IO.Path]::GetFullPath($ExpectedExecutablePath)
    $actual = [System.IO.Path]::GetFullPath($process.ExecutablePath)
    Assert-True ($actual -ieq $expected) "InferDeck is running from $actual, expected $expected."
}

$health = Invoke-Json -Method Get -Uri "$BaseUrl/api/inferdeck/v1/health"
Assert-True ($health.ok -eq $true) "/api/inferdeck/v1/health did not report ok=true."

$models = Invoke-Json -Method Get -Uri "$BaseUrl/v1/models"
Assert-True ($models.object -eq "list") "/v1/models did not return an OpenAI-compatible list."
Assert-True ($null -ne ($models.data | Where-Object { $_.id -eq $Model })) `
    "Model $Model is not registered."

if (-not $SkipGeneration) {
    $chat = Invoke-Json -Method Post -Uri "$BaseUrl/v1/chat/completions" -TimeoutSec 300 -Body @{
        model = $Model
        messages = @(@{
            role = "user"
            content = "Reply with exactly: inferdeck opencode ok"
        })
        max_tokens = 24
        stream = $false
    }
    Assert-True ($chat.choices.Count -gt 0) "Non-streaming chat returned no choices."
    Assert-True ($chat.choices[0].message.role -eq "assistant") `
        "Non-streaming chat did not return an assistant message."

    $streamBody = @{
        model = $Model
        messages = @(@{
            role = "user"
            content = "Reply with exactly: inferdeck stream ok"
        })
        max_tokens = 24
        stream = $true
    } | ConvertTo-Json -Depth 12 -Compress
    $stream = $streamBody |
        curl.exe -N -sS --max-time 300 -A "opencode/inferdeck-smoke" `
            -H "Content-Type: application/json" --data-binary "@-" `
            "$BaseUrl/v1/chat/completions"
    Assert-True ($LASTEXITCODE -eq 0) "Streaming chat request failed."
    Assert-True ($stream -match "chat.completion.chunk") `
        "Streaming chat did not emit completion chunks."
    Assert-True ($stream -match "data: \[DONE\]") `
        "Streaming chat did not terminate with [DONE]."

    if ($RunDisconnectSmoke) {
        $disconnectBody = @{
            model = $Model
            messages = @(@{
                role = "user"
                content = "Write a long numbered checklist and continue until stopped."
            })
            max_tokens = 2048
            stream = $true
        } | ConvertTo-Json -Depth 12 -Compress
        $disconnectBody |
            curl.exe -N -sS --max-time 2 -A "opencode/inferdeck-smoke" `
                -H "Content-Type: application/json" --data-binary "@-" `
                "$BaseUrl/v1/chat/completions" | Out-Null
        $deadline = [DateTime]::UtcNow.AddSeconds(30)
        do {
            Start-Sleep -Milliseconds 250
            $swap = Invoke-Json -Method Get -Uri "$BaseUrl/api/inferdeck/v1/swap/status"
        } while ([int]$swap.active_requests -gt 0 -and [DateTime]::UtcNow -lt $deadline)
        Assert-True ([int]$swap.active_requests -eq 0) `
            "Disconnected stream retained an active request."
    }
}

$status = Invoke-Json -Method Get -Uri "$BaseUrl/api/inferdeck/v1/status"
Assert-True ($null -ne $status.queue) "/api/inferdeck/v1/status did not return queue state."

$ownerAfter = Get-NetTCPConnection -LocalPort $uri.Port -State Listen -ErrorAction SilentlyContinue |
    Select-Object -First 1 -ExpandProperty OwningProcess
Assert-True ($ownerAfter -eq $listener.OwningProcess) `
    "InferDeck listener ownership changed during the smoke test."

Write-Host "InferDeck in-process OpenCode smoke checks passed."
