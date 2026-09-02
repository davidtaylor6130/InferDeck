param(
    [string]$Root = (Split-Path -Parent $PSScriptRoot)
)

$ErrorActionPreference = 'Stop'
function Read-ProjectFile {
    param([string]$Path)
    $fullPath = Join-Path $Root $Path
    if (-not (Test-Path -LiteralPath $fullPath)) { throw "Documentation file is missing: $Path" }
    return [IO.File]::ReadAllText($fullPath)
}
function Require-Text {
    param([string]$Path, [string[]]$Values)
    $content = Read-ProjectFile $Path
    foreach ($value in $Values) {
        if (-not $content.Contains($value)) { throw "$Path is missing required truth marker: $value" }
    }
}
function Reject-Text {
    param([string]$Path, [string[]]$Values)
    $content = Read-ProjectFile $Path
    foreach ($value in $Values) {
        if ($content.Contains($value)) { throw "$Path retains stale claim: $value" }
    }
}

Require-Text 'overhall_plan.md' @(
    '| Phase 13 - Implementation decomposition and documentation truth | #121 |',
    '- [ ] Close linked issues only with source, test, artifact and live evidence.',
    '- [ ] Close #142, Phase 14, and the epic only after the new live evidence is',
    '`implementation_boundaries` enforces this.'
)
Reject-Text 'overhall_plan.md' @(
    '| Phase 8 - Audio | #116 |',
    '- [x] Close the overhaul epic after every child issue is resolved.',
    '- [x] Every checkbox in Phases 0 through 14 is complete or explicitly removed by',
    '- [x] All new audit issues and the overhaul epic are closed with evidence.',
    '- [x] The matched release is built, published and live-verified when deployment is',
    'The matched gateway and dashboard were deployed`nfrom that revision'
)
Require-Text 'docs/DEPLOY.md' @(
    'inferdeck-gateway.exe --version',
    'build_revision',
    'dirty=false'
)
Require-Text 'docs/release-reproducibility.md' @(
    'INFERDECK_BUILD_REVISION',
    'GET /api/inferdeck/v1/health'
)
Require-Text 'docs/architecture.md' @(
    'benchmark implementation modules',
    'stream serialization and control YAML modules'
)
Require-Text 'docs/opencode-setup-guide.md' @(
    'InferDeck model aliases',
    'inferdeck/Normal',
    'inferdeck/n8n-model'
)
Reject-Text 'docs/opencode-setup-guide.md' @(
    'LlamaEngine (in-process inference)',
    '| `context` | 65536 |',
    'pnpm export:opencode',
    'The exporter',
    'exported provider'
)
foreach ($path in @(
    'scripts/export-opencode-config.mjs',
    'Testing/export-opencode-config.test.mjs'
)) {
    if (Test-Path -LiteralPath (Join-Path $Root $path)) {
        throw "Obsolete OpenCode exporter file remains: $path"
    }
}
$package = Read-ProjectFile 'package.json' | ConvertFrom-Json
foreach ($script in @('export:opencode', 'test:opencode-export')) {
    if ($package.scripts.PSObject.Properties.Name -contains $script) {
        throw "Obsolete OpenCode package script remains: $script"
    }
}
Reject-Text '.github/workflows/ci.yml' @('test:opencode-export')
Reject-Text '.github/workflows/release.yml' @('test:opencode-export')

'DOCUMENTATION_TRUTH_OK'
