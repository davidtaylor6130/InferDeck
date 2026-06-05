#requires -Version 5.1
<#
.SYNOPSIS
  Records parity baseline by sending prompts to llama-server.exe.

.DESCRIPTION
  Reads prompts from tests/parity/prompts.yaml (or .json), sends each one to
  a running llama-server.exe at $BaseUrl, captures the response, and writes
  the results as JSONL to the output file. Each line: {id, category, prompt,
  reference, response_text, response_ms, timestamp_unix_ms}.

.PARAMETER BaseUrl
  llama-server base URL (default http://127.0.0.1:8080).

.PARAMETER Model
  Model name to send in the request body (e.g. qwen3.6-27b).

.PARAMETER OutputPath
  JSONL output file (default tests/parity/baselines/<model>.jsonl).

.PARAMETER PromptsPath
  Path to prompts.json (default tests/parity/prompts.json).

.EXAMPLE
  pwsh -File record_baseline.ps1 -BaseUrl http://127.0.0.1:8080 -Model qwen3.6-27b
#>

param(
    [string]$BaseUrl = "http://127.0.0.1:8080",
    [Parameter(Mandatory)] [string]$Model,
    [string]$OutputPath,
    [string]$PromptsPath = "$PSScriptRoot/prompts.json"
)

$ErrorActionPreference = "Stop"

if (-not $OutputPath) {
    $OutputPath = "$PSScriptRoot/baselines/$Model.jsonl"
}
$outDir = Split-Path -Parent $OutputPath
if (-not (Test-Path $outDir)) { New-Item -ItemType Directory -Path $outDir -Force | Out-Null }

if (-not (Test-Path $PromptsPath)) {
    throw "Prompts file not found: $PromptsPath"
}
$prompts = Get-Content $PromptsPath -Raw | ConvertFrom-Json

"Recording baseline for $Model to $OutputPath" | Write-Host
"Source: $BaseUrl" | Write-Host
"Prompts: $($prompts.Count)" | Write-Host

$results = New-Object System.Collections.Generic.List[object]
foreach ($p in $prompts) {
    $body = @{
        model    = $Model
        stream   = $false
        messages = @(
            if ($p.system) { @{ role = "system"; content = $p.system } }
            @{ role = "user"; content = $p.user }
        )
    } | ConvertTo-Json -Depth 8 -Compress
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    try {
        $resp = Invoke-RestMethod -Uri "$BaseUrl/v1/chat/completions" `
                                  -Method Post `
                                  -ContentType "application/json" `
                                  -Body $body `
                                  -TimeoutSec 120
        $sw.Stop()
        $text = $resp.choices[0].message.content
        $ok = $true
    } catch {
        $sw.Stop()
        $text = ""
        $ok = $false
        "  FAIL id=$($p.id): $_" | Write-Host
    }
    $entry = [ordered]@{
        id                  = $p.id
        category            = $p.category
        prompt              = $p.user
        reference           = $p.reference
        response_text       = $text
        response_ms         = [int64]$sw.ElapsedMilliseconds
        timestamp_unix_ms   = [int64]([DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds())
        ok                  = $ok
    }
    $results.Add([pscustomobject]$entry)
    "  id=$($p.id)  ms=$([int64]$sw.ElapsedMilliseconds)  len=$($text.Length)" | Write-Host
}

$jsonl = ($results | ForEach-Object { $_ | ConvertTo-Json -Compress -Depth 8 }) -join "`n"
Set-Content -Path $OutputPath -Value $jsonl -NoNewline -Encoding UTF8
"Done. Wrote $($results.Count) entries to $OutputPath" | Write-Host
