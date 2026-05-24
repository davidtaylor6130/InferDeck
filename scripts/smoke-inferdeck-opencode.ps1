param(
    [string]$GatewayBaseUrl = "http://127.0.0.1:11434",
    [string]$DashboardBaseUrl = "http://127.0.0.1:8080",
    [int]$BackendPort = 18080,
    [string]$RuntimePath = "$PSScriptRoot\..\runtime\llama-b9276-vulkan\llama-server.exe",
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

function Get-Listener {
    param([int]$Port)
    Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue |
        Where-Object { $_.LocalPort -eq $Port } |
        Select-Object -First 1
}

$gatewayUri = [Uri]$GatewayBaseUrl
$gatewayPort = $gatewayUri.Port
$gatewayListener = Get-Listener -Port $gatewayPort
$backendListener = Get-Listener -Port $BackendPort

Assert-True ($null -ne $gatewayListener) "No gateway listener found on port $gatewayPort."
Assert-True ($null -ne $backendListener) "No llama.cpp backend listener found on port $BackendPort."

$resolvedRuntime = [System.IO.Path]::GetFullPath($RuntimePath)
$runtimeProcesses = Get-CimInstance Win32_Process |
    Where-Object {
        $_.Name -eq "llama-server.exe" -and
        $_.ExecutablePath -and
        ([System.IO.Path]::GetFullPath($_.ExecutablePath) -ieq $resolvedRuntime)
    }

Assert-True (($runtimeProcesses | Measure-Object).Count -eq 1) "Expected exactly one InferDeck llama-server process, found $(($runtimeProcesses | Measure-Object).Count)."
Assert-True ($backendListener.OwningProcess -eq $runtimeProcesses[0].ProcessId) "Backend port $BackendPort is owned by PID $($backendListener.OwningProcess), not tracked InferDeck llama-server PID $($runtimeProcesses[0].ProcessId)."

$models = Invoke-RestMethod -Method Get -Uri "$GatewayBaseUrl/v1/models" -TimeoutSec 10
Assert-True ($models.object -eq "list") "/v1/models did not return an OpenAI-compatible list."

if (-not $SkipGeneration) {
    $openCodeUserAgent = "opencode/1.14.48"

    function Invoke-OpenAiTinyChat {
        param([string]$Model, [string]$Expected)
        $body = @{
            model = $Model
            messages = @(@{ role = "user"; content = "Reply with exactly: $Expected" })
            max_tokens = 12
            stream = $false
        } | ConvertTo-Json -Depth 8
        $response = Invoke-RestMethod -Method Post -Uri "$GatewayBaseUrl/v1/chat/completions" -Body $body -ContentType "application/json" -TimeoutSec 180
        Assert-True ($response.choices.Count -gt 0) "/v1/chat/completions returned no choices for $Model."
        return $response
    }

    $chatBody = @{
        model = "qwen3.6-35b-a3b"
        messages = @(@{ role = "user"; content = "Reply with exactly: inferdeck opencode ok" })
        max_tokens = 12
        stream = $false
    } | ConvertTo-Json -Depth 8
    $chat = Invoke-RestMethod -Method Post -Uri "$GatewayBaseUrl/v1/chat/completions" -Body $chatBody -ContentType "application/json" -TimeoutSec 180
    Assert-True ($chat.choices.Count -gt 0) "/v1/chat/completions returned no choices."

    $ollamaBody = @{
        model = "qwen3.6-35b-a3b"
        messages = @(@{ role = "user"; content = "Reply with exactly: inferdeck webui ok" })
        stream = $false
        options = @{ num_predict = 12 }
    } | ConvertTo-Json -Depth 8
    $ollama = Invoke-RestMethod -Method Post -Uri "$GatewayBaseUrl/api/chat" -Body $ollamaBody -ContentType "application/json" -TimeoutSec 180
    Assert-True ($ollama.message.role -eq "assistant") "/api/chat did not return an Ollama-compatible assistant message."

    $plainStreamBody = @{
        model = "qwen3.6-35b-a3b"
        messages = @(@{ role = "user"; content = "Reply with exactly: inferdeck stream ok" })
        max_tokens = 24
        stream = $true
    } | ConvertTo-Json -Depth 8 -Compress
    $plainStream = $plainStreamBody | curl.exe -N -sS --max-time 180 -A $openCodeUserAgent -H "Content-Type: application/json" --data-binary "@-" "$GatewayBaseUrl/v1/chat/completions"
    Assert-True ($plainStream -match "data: " -and $plainStream -match "chat.completion.chunk" -and $plainStream -match "data: \[DONE\]") "Plain OpenCode stream:true did not emit OpenAI SSE chunks and [DONE]."

    $statusAfterPlainStream = Invoke-RestMethod -Method Get -Uri "$DashboardBaseUrl/api/status" -TimeoutSec 10
    Assert-True ($null -ne $statusAfterPlainStream.observability.lastOpenCodeRequest) "Plain stream did not update lastOpenCodeRequest."
    Assert-True ($statusAfterPlainStream.observability.lastOpenCodeRequest.client -eq "OpenCode") "Plain stream lastOpenCodeRequest client was $($statusAfterPlainStream.observability.lastOpenCodeRequest.client), expected OpenCode."
    Assert-True ($statusAfterPlainStream.observability.lastOpenCodeRequest.responseMode -eq "backend-stream") "Plain stream responseMode was $($statusAfterPlainStream.observability.lastOpenCodeRequest.responseMode), expected backend-stream."
    Assert-True ([int]$statusAfterPlainStream.observability.lastOpenCodeRequest.sseChunks -gt 0) "Plain stream did not record SSE chunks."

    $toolStreamBody = @{
        model = "qwen3.6-35b-a3b"
        messages = @(@{ role = "user"; content = "Plan a file read using the provided tool." })
        max_tokens = 64
        stream = $true
        tools = @(@{
            type = "function"
            function = @{
                name = "read"
                description = "Read a local file"
                parameters = @{
                    type = "object"
                    properties = @{ path = @{ type = "string" } }
                    required = @("path")
                }
            }
        })
    } | ConvertTo-Json -Depth 12 -Compress
    $toolStream = $toolStreamBody | curl.exe -N -sS --max-time 180 -A $openCodeUserAgent -H "Content-Type: application/json" --data-binary "@-" "$GatewayBaseUrl/v1/chat/completions"
    Assert-True ($toolStream -match "data: " -and $toolStream -match "chat.completion.chunk" -and $toolStream -match "data: \[DONE\]") "Tool stream:true did not emit synthetic OpenAI SSE and [DONE]."

    $statusAfterToolStream = Invoke-RestMethod -Method Get -Uri "$DashboardBaseUrl/api/status" -TimeoutSec 10
    Assert-True ($null -ne $statusAfterToolStream.observability.lastOpenCodeRequest) "Tool stream did not update lastOpenCodeRequest."
    Assert-True ($statusAfterToolStream.observability.lastOpenCodeRequest.client -eq "OpenCode") "Tool stream lastOpenCodeRequest client was $($statusAfterToolStream.observability.lastOpenCodeRequest.client), expected OpenCode."
    Assert-True ($statusAfterToolStream.observability.lastOpenCodeRequest.responseMode -eq "synthetic-sse") "Tool stream responseMode was $($statusAfterToolStream.observability.lastOpenCodeRequest.responseMode), expected synthetic-sse."
    Assert-True ([int]$statusAfterToolStream.observability.lastOpenCodeRequest.sseChunks -gt 0) "Tool stream did not record SSE chunks."

    Invoke-OpenAiTinyChat -Model "gpt-oss:20b" -Expected "gpt ok" | Out-Null
    Invoke-OpenAiTinyChat -Model "qwen3.6-35b-a3b" -Expected "qwen ok" | Out-Null

    if ($RunDisconnectSmoke) {
        $disconnectBody = @{
            model = "qwen3.6-35b-a3b"
            messages = @(@{ role = "user"; content = "Write a long numbered checklist of 200 loading speed optimizations. Keep going until stopped." })
            max_tokens = 2048
            stream = $true
        } | ConvertTo-Json -Depth 8 -Compress
        $disconnectBody | curl.exe -N -sS --max-time 2 -A $openCodeUserAgent -H "Content-Type: application/json" --data-binary "@-" "$GatewayBaseUrl/v1/chat/completions" | Out-Null
        Start-Sleep -Seconds 8
        $status = Invoke-RestMethod -Method Get -Uri "$DashboardBaseUrl/api/status" -TimeoutSec 10
        Assert-True ([int]$status.queue.running -eq 0) "Disconnect smoke left dashboard queue running=$($status.queue.running)."
        Assert-True ($status.observability.lastOpenCodeRequest.client -eq "OpenCode") "Disconnect smoke did not record OpenCode as last client."
        Assert-True ($status.observability.lastOpenCodeRequest.phase -eq "failed") "Disconnect smoke phase was $($status.observability.lastOpenCodeRequest.phase), expected failed."
    }

    Start-Sleep -Seconds 2
    $runtimeProcessesAfterSwitch = Get-CimInstance Win32_Process |
        Where-Object {
            $_.Name -eq "llama-server.exe" -and
            $_.ExecutablePath -and
            ([System.IO.Path]::GetFullPath($_.ExecutablePath) -ieq $resolvedRuntime)
        }
    Assert-True (($runtimeProcessesAfterSwitch | Measure-Object).Count -eq 1) "Expected one InferDeck llama-server process after rapid switching, found $(($runtimeProcessesAfterSwitch | Measure-Object).Count)."
}

Write-Host "InferDeck OpenCode/Open WebUI smoke checks passed."
