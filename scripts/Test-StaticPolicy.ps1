param([string]$Root = (Split-Path -Parent $PSScriptRoot))

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path -LiteralPath $Root).Path
& git -C $repoRoot diff --check
if ($LASTEXITCODE -ne 0) { throw 'git whitespace validation failed' }

Get-ChildItem -LiteralPath (Join-Path $repoRoot 'scripts') -Filter *.ps1 -File -Recurse |
    ForEach-Object {
        $tokens = $null
        $errors = $null
        [System.Management.Automation.Language.Parser]::ParseFile($_.FullName, [ref]$tokens, [ref]$errors) | Out-Null
        if ($errors.Count -gt 0) { throw "PowerShell parse failed: $($_.FullName): $($errors[0].Message)" }
    }

& git -C $repoRoot ls-files '*.json' | ForEach-Object {
        $jsonPath = Join-Path $repoRoot $_
        try {
            Get-Content -LiteralPath $jsonPath -Raw | ConvertFrom-Json | Out-Null
        } catch {
            throw "JSON parse failed: ${jsonPath}: $($_.Exception.Message)"
        }
    }

Write-Output 'STATIC_POLICY_OK'
