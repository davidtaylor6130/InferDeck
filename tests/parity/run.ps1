#requires -Version 5.1
<#
.SYNOPSIS
  Runs the parity harness against inferdeck-gateway.

.DESCRIPTION
  Loads the baseline JSONL (recorded from llama-server), sends the same
  prompts to inferdeck-gateway at $GatewayUrl, and uses the parity comparator
  logic (via inline C# - LCS-based token similarity) to score each response.

  Writes a JSON report to the output path. Exit code 0 if all prompts pass
  their per-prompt min_score, 1 otherwise.

.PARAMETER BaselinePath
  Path to baseline JSONL (from record_baseline.ps1).

.PARAMETER GatewayUrl
  inferdeck-gateway base URL (default http://127.0.0.1:11434).

.PARAMETER Model
  Model name to send in requests (default: derived from baseline filename).

.PARAMETER OutputPath
  Path to write the JSON report (default: stdout).

.EXAMPLE
  pwsh -File run.ps1 -BaselinePath tests/parity/baselines/qwen3.6-27b.jsonl -GatewayUrl http://127.0.0.1:11434
#>

param(
    [Parameter(Mandatory)] [string]$BaselinePath,
    [string]$GatewayUrl = "http://127.0.0.1:11434",
    [string]$Model,
    [string]$OutputPath
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $BaselinePath)) { throw "Baseline not found: $BaselinePath" }
if (-not $Model) { $Model = [System.IO.Path]::GetFileNameWithoutExtension($BaselinePath) }

"Running parity for model $Model" | Write-Host
"Baseline: $BaselinePath" | Write-Host
"Gateway:  $GatewayUrl" | Write-Host

$entries = Get-Content $BaselinePath | ForEach-Object { $_ | ConvertFrom-Json }
"Loaded $($entries.Count) baseline entries" | Write-Host

Add-Type -TypeDefinition @"
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;

public static class ParityCS {
    public static string Normalize(string text, bool stripThink, bool collapseWs, bool caseInsensitive) {
        if (stripThink) {
            text = Regex.Replace(text, @"\<think\>.*?\<\/think\>", "", RegexOptions.IgnoreCase | RegexOptions.Singleline);
            text = Regex.Replace(text, @"\<think\>.*$", "", RegexOptions.IgnoreCase | RegexOptions.Singleline);
        }
        if (collapseWs) {
            var sb = new StringBuilder(text.Length);
            bool lastSpace = true;
            foreach (var c in text) {
                if (char.IsWhiteSpace(c)) {
                    if (!lastSpace) { sb.Append(' '); lastSpace = true; }
                } else { sb.Append(c); lastSpace = false; }
            }
            if (sb.Length > 0 && sb[sb.Length-1] == ' ') sb.Length--;
            text = sb.ToString();
        }
        if (caseInsensitive) text = text.ToLowerInvariant();
        return text;
    }

    public static List<string> Tokenize(string text) {
        var toks = new List<string>();
        var cur = new StringBuilder();
        foreach (var c in text) {
            if (char.IsLetterOrDigit(c)) cur.Append(c);
            else { if (cur.Length > 0) { toks.Add(cur.ToString()); cur.Clear(); } }
        }
        if (cur.Length > 0) toks.Add(cur.ToString());
        return toks;
    }

    public class Result {
        public double Score;
        public int BaselineTokens;
        public int CandidateTokens;
        public int MatchedTokens;
    }

    public static Result Compare(string baseline, string candidate, bool stripThink, bool collapseWs, bool caseInsensitive) {
        var a = Normalize(baseline, stripThink, collapseWs, caseInsensitive);
        var b = Normalize(candidate, stripThink, collapseWs, caseInsensitive);
        var at = Tokenize(a);
        var bt = Tokenize(b);
        var r = new Result {
            BaselineTokens = at.Count,
            CandidateTokens = bt.Count
        };
        if (at.Count == 0 && bt.Count == 0) { r.Score = 1.0; r.MatchedTokens = 0; return r; }
        if (at.Count == 0 || bt.Count == 0) { r.Score = 0.0; r.MatchedTokens = 0; return r; }
        var lcs = new int[at.Count+1, bt.Count+1];
        for (int i = 1; i <= at.Count; i++)
            for (int j = 1; j <= bt.Count; j++)
                lcs[i,j] = at[i-1] == bt[j-1] ? lcs[i-1,j-1] + 1 : Math.Max(lcs[i-1,j], lcs[i,j-1]);
        r.MatchedTokens = lcs[at.Count, bt.Count];
        int n = Math.Max(at.Count, bt.Count);
        r.Score = n > 0 ? (double)r.MatchedTokens / n : 0.0;
        return r;
    }
}
"@

$report = [ordered]@{
    model = $Model
    baseline_path = (Resolve-Path $BaselinePath).Path
    gateway_url = $GatewayUrl
    started_unix_ms = [int64]([DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds())
    entries = New-Object System.Collections.Generic.List[object]
}
$passing = 0
$failing = 0
$totalScore = 0.0

foreach ($e in $entries) {
    $minScore = if ($e.PSObject.Properties.Match('min_score').Count -gt 0) { $e.min_score } else { 0.95 }
    $body = @{
        model = $Model
        stream = $false
        messages = @(
            if ($e.prompt) { @{ role = "user"; content = $e.prompt } }
        )
    } | ConvertTo-Json -Depth 8 -Compress
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    try {
        $resp = Invoke-RestMethod -Uri "$GatewayUrl/v1/chat/completions" `
                                  -Method Post `
                                  -ContentType "application/json" `
                                  -Body $body `
                                  -TimeoutSec 120
        $sw.Stop()
        $candidateText = $resp.choices[0].message.content
        $ok = $true
    } catch {
        $sw.Stop()
        $candidateText = ""
        $ok = $false
    }
    $r = [ParityCS]::Compare($e.response_text, $candidateText, $true, $true, $false)
    $passes = ($r.Score -ge $minScore) -and $ok
    if ($passes) { $passing++ } else { $failing++ }
    $totalScore += $r.Score
    $entry = [ordered]@{
        prompt_id = $e.id
        category = $e.category
        min_score = $minScore
        score = $r.Score
        baseline_tokens = $r.BaselineTokens
        candidate_tokens = $r.CandidateTokens
        matched_tokens = $r.MatchedTokens
        gateway_ms = [int64]$sw.ElapsedMilliseconds
        baseline_ms = $e.response_ms
        ok = $passes
    }
    $report.entries.Add([pscustomobject]$entry)
    $marker = if ($passes) { "PASS" } else { "FAIL" }
    "  $marker  id=$($e.id)  score=$($r.Score.ToString('0.000'))  min=$minScore  gateway_ms=$([int64]$sw.ElapsedMilliseconds)" | Write-Host
}

$report.finished_unix_ms = [int64]([DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds())
$report.overall_score = if ($entries.Count -gt 0) { [math]::Round($totalScore / $entries.Count, 4) } else { 0.0 }
$report.passing = $passing
$report.failing = $failing
$report.ok = ($failing -eq 0)

$json = $report | ConvertTo-Json -Depth 8
if ($OutputPath) {
    Set-Content -Path $OutputPath -Value $json -Encoding UTF8
    "Report written to $OutputPath" | Write-Host
} else {
    Write-Host "`n--- REPORT ---"
    Write-Host $json
}

"" | Write-Host
"Overall:  score=$($report.overall_score)  passing=$passing  failing=$failing" | Write-Host
if ($failing -gt 0) { exit 1 } else { exit 0 }
