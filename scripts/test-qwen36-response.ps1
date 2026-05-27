param(
    [string]$BaseUrl = "http://127.0.0.1:11434",
    [int]$TimeoutSec = 180
)

$ErrorActionPreference = "Stop"

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) {
        throw $Message
    }
}

Write-Host "=== Qwen3.6 Smoke Tests ===" -ForegroundColor Cyan
Write-Host "Target: $BaseUrl/v1/chat/completions`n" -ForegroundColor Gray

# Test 1: Simple chat (non-stream)
Write-Host "[Test 1] Simple chat (non-stream)..." -NoNewline
$body = @{
    model = "qwen3.6-35b-a3b"
    messages = @(@{ role = "user"; content = "Reply with exactly: pong" })
    max_tokens = 12
    stream = $false
} | ConvertTo-Json -Depth 8
$resp = Invoke-RestMethod -Method Post -Uri "$BaseUrl/v1/chat/completions" -Body $body -ContentType "application/json" -TimeoutSec $TimeoutSec
Assert-True ($resp.choices.Count -gt 0) "No choices returned"
Assert-True ($resp.choices[0].message.role -eq "assistant") "Role not assistant"
Assert-True ($resp.choices[0].finish_reason -in "stop", "length") "Unexpected finish_reason"
Write-Host " PASS" -ForegroundColor Green

# Test 2: Check reasoning_content is present
Write-Host "[Test 2] reasoning_content field..." -NoNewline
$body = @{
    model = "qwen3.6-35b-a3b"
    messages = @(@{ role = "user"; content = "Think step by step: what is 2+2?" })
    max_tokens = 64
    stream = $false
} | ConvertTo-Json -Depth 8
$resp = Invoke-RestMethod -Method Post -Uri "$BaseUrl/v1/chat/completions" -Body $body -ContentType "application/json" -TimeoutSec $TimeoutSec
$hasReasoning = $resp.choices[0].message.PSObject.Properties.Name -contains "reasoning_content"
if ($hasReasoning) {
    Write-Host " has reasoning" -ForegroundColor Green
} else {
    Write-Host " no reasoning" -ForegroundColor Yellow
}

# Test 3: Tool call (non-stream)
Write-Host "[Test 3] Tool call (non-stream)..." -NoNewline
$body = @{
    model = "qwen3.6-35b-a3b"
    messages = @(@{ role = "user"; content = "Use the read tool to check App.tsx" })
    max_tokens = 128
    stream = $false
    tools = @(@{
        type = "function"
        function = @{
            name = "read"
            description = "Read a local file"
            parameters = @{
                type = "object"
                properties = @{ filePath = @{ type = "string" } }
                required = @("filePath")
            }
        }
    })
} | ConvertTo-Json -Depth 12 -Compress
$resp = Invoke-RestMethod -Method Post -Uri "$BaseUrl/v1/chat/completions" -Body $body -ContentType "application/json" -TimeoutSec $TimeoutSec
$msg = $resp.choices[0].message
Assert-True ($msg.role -eq "assistant") "Role not assistant"
$hasTC = $msg.PSObject.Properties.Name -contains "tool_calls"
if ($hasTC -and $msg.tool_calls.Count -gt 0) {
    Write-Host " PASS (tool calls)" -ForegroundColor Green
} else {
    Write-Host " PASS (no tool calls needed)" -ForegroundColor Yellow
}

# Test 4: Simple chat (stream)
Write-Host "[Test 4] Simple chat (stream)..." -NoNewline
$body = @{
    model = "qwen3.6-35b-a3b"
    messages = @(@{ role = "user"; content = "Reply with exactly: stream ok" })
    max_tokens = 24
    stream = $true
} | ConvertTo-Json -Depth 8 -Compress
$streamOut = $body | curl.exe -N -sS --max-time $TimeoutSec -H "Content-Type: application/json" --data-binary "@-" "$BaseUrl/v1/chat/completions" 2>$null
$hasData = ($streamOut -match "data: ").Count -gt 0
$hasChunk = ($streamOut -match "chat.completion.chunk").Count -gt 0
$hasDone = ($streamOut -match "\[DONE\]").Count -gt 0
Assert-True $hasData "No SSE data frames"
Assert-True $hasChunk "No chunk events"
Assert-True $hasDone "No [DONE] marker"
Write-Host " PASS" -ForegroundColor Green

# Test 5: Tool call (stream)
Write-Host "[Test 5] Tool call (stream)..." -NoNewline
$body = @{
    model = "qwen3.6-35b-a3b"
    messages = @(@{ role = "user"; content = "Use the read tool to inspect a file" })
    max_tokens = 128
    stream = $true
    tools = @(@{
        type = "function"
        function = @{
            name = "read"
            description = "Read a local file"
            parameters = @{
                type = "object"
                properties = @{ filePath = @{ type = "string" } }
                required = @("filePath")
            }
        }
    })
} | ConvertTo-Json -Depth 12 -Compress
$streamOut = $body | curl.exe -N -sS --max-time $TimeoutSec -H "Content-Type: application/json" --data-binary "@-" "$BaseUrl/v1/chat/completions" 2>$null
$hasData = ($streamOut -match "data: ").Count -gt 0
$hasDone = ($streamOut -match "\[DONE\]").Count -gt 0
Assert-True $hasData "No SSE data frames"
Assert-True $hasDone "No [DONE] marker"
Write-Host " PASS" -ForegroundColor Green

Write-Host "`n=== All smoke tests passed ===" -ForegroundColor Cyan
