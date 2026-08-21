param(
    [string]$Root = (Split-Path -Parent $PSScriptRoot),
    [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path -LiteralPath $Root).Path
$failures = [System.Collections.Generic.List[string]]::new()

function Add-Failure([string]$Message) {
    $failures.Add($Message)
}

function Test-CoreText([string]$RelativePath, [string]$Text) {
    $normalized = $RelativePath.Replace('\', '/')
    if ($Text -match '(?i)\b(system|popen|_popen|CreateProcess[AW]?|ShellExecute[AW]?|WinExec)\s*\(' -or
        $Text -match '(?i)llama-server\.exe|subprocess\.') {
        Add-Failure "${normalized}: forbidden process or proxy launch"
    }
    if ($normalized -match '^libs/(model|llama_cpp_wrapper|native_runtimes)/' -and
        $Text -match '(?i)httplib|openai_body_json|chat\.completion|stream_options') {
        Add-Failure "${normalized}: protocol leaked below the gateway adapter"
    }
    if ($normalized -match '^libs/(model|native_runtimes)/' -and
        $Text -match 'nlohmann::json::parse\s*\(') {
        Add-Failure "${normalized}: backend parses wire JSON"
    }
    if ($normalized -match '^libs/foundation/' -and
        $Text -match '#include\s*[<\"](?:inference|model|llama_cpp_wrapper|native_runtimes|observability|gateway)/') {
        Add-Failure "${normalized}: foundation dependency points upward"
    }
    if ($normalized -match '^libs/inference/' -and
        $Text -match '#include\s*[<\"](?:model|llama_cpp_wrapper|native_runtimes|observability|gateway)/') {
        Add-Failure "${normalized}: inference dependency points upward"
    }
    if ($normalized -match '^libs/model/' -and
        $Text -match '#include\s*[<\"](?:llama_cpp_wrapper|native_runtimes|observability|gateway)/') {
        Add-Failure "${normalized}: model dependency points upward"
    }
}

function Test-StrictPath([string]$Path) {
    $allowed = @(
        '/v1/models',
        '/v1/chat/completions',
        '/v1/responses',
        '/v1/embeddings',
        '/v1/images/generations',
        '/v1/audio/speech',
        '/v1/audio/transcriptions'
    )
    if ($Path.StartsWith('/v1/') -and $Path -notin $allowed) {
        Add-Failure "${Path}: non-OpenAI route in strict Core"
    }
}

function Test-ControlClassification([string]$Path, [string]$Principal) {
    if ($Path.StartsWith('/api/') -and $Principal -eq 'PublicStatus') {
        Add-Failure "${Path}: control route is unauthenticated"
    }
}

function Test-SseTerminator([string]$Chunk) {
    if ($Chunk -notmatch 'data: \[DONE\](?:\r?\n){2}$') {
        Add-Failure 'strict SSE stream has no valid terminal chunk'
    }
}

function Test-ReleaseDefinition([string]$Text) {
    if ($Text -match '(?im)^\s*Copy-Item\s+ops\\' -or
        $Text -match '(?i)forced-aligner|apps/forced-aligner') {
        Add-Failure 'release package includes a non-Core launcher or companion'
    }
    if ($Text -notmatch [regex]::Escape("Where-Object Name -NotIn @('fmtd.dll', 'spdlogd.dll')")) {
        Add-Failure 'release package does not exclude debug-only runtime DLLs'
    }
}

$scanRoots = @(
    'apps/inferdeck-gateway/src',
    'libs/foundation/include', 'libs/foundation/src',
    'libs/inference/include', 'libs/inference/src',
    'libs/model/include', 'libs/model/src',
    'libs/llama_cpp_wrapper/include', 'libs/llama_cpp_wrapper/src',
    'libs/native_runtimes/include', 'libs/native_runtimes/src',
    'libs/observability/include', 'libs/observability/src',
    'libs/gateway/include', 'libs/gateway/src'
)
foreach ($relativeRoot in $scanRoots) {
    $path = Join-Path $repoRoot $relativeRoot
    if (!(Test-Path -LiteralPath $path)) { continue }
    Get-ChildItem -LiteralPath $path -Recurse -File | Where-Object Extension -in '.cpp', '.hpp', '.ipp' | ForEach-Object {
        $relative = $_.FullName.Substring($repoRoot.Length).TrimStart('\', '/')
        Test-CoreText $relative (Get-Content -LiteralPath $_.FullName -Raw)
    }
}

$manifest = Get-Content -LiteralPath (Join-Path $repoRoot 'tests/fixtures/openai_route_manifest.json') -Raw | ConvertFrom-Json
foreach ($route in $manifest.routes) { Test-StrictPath $route.path }
if ($manifest.profile -ne 'strict_openai' -or $manifest.routes.Count -ne 7) {
    Add-Failure 'strict route manifest identity or count changed'
}

$authText = Get-Content -LiteralPath (Join-Path $repoRoot 'libs/gateway/include/gateway/auth.hpp') -Raw
if ($authText -notmatch 'path\.starts_with\("/api/"\)' -or
    $authText -notmatch 'RoutePrincipal::ControlRead' -or
    $authText -notmatch 'RoutePrincipal::ControlWrite') {
    Add-Failure 'control route classification is incomplete'
}
Test-ControlClassification '/api/inferdeck/v1/status' 'DashboardSession'

$routeTests = Get-Content -LiteralPath (Join-Path $repoRoot 'libs/gateway/tests/test_routes.cpp') -Raw
if ($routeTests -notmatch 'Chat stream serializers preserve exact OpenAI event ordering') {
    Add-Failure 'strict SSE golden test is missing'
}
Test-SseTerminator "data: [DONE]`n`n"
Test-ReleaseDefinition (Get-Content -LiteralPath (
    Join-Path $repoRoot '.github/workflows/release.yml') -Raw)

if ($SelfTest) {
    $before = $failures.Count
    Test-CoreText 'libs/model/src/broken.cpp' 'CreateProcessW(nullptr, command, nullptr, nullptr, false, 0, nullptr, nullptr, nullptr, nullptr);'
    Test-CoreText 'libs/model/src/broken.cpp' 'auto body = nlohmann::json::parse(request.body);'
    Test-StrictPath '/v1/vendor/messages'
    Test-ControlClassification '/api/inferdeck/v1/unprotected' 'PublicStatus'
    Test-SseTerminator "data: {}`n"
    Test-ReleaseDefinition @"
Copy-Item ops\*.ps1 dist\ops\
Where-Object Name -NotIn @('fmtd.dll', 'spdlogd.dll')
"@
    Test-ReleaseDefinition 'Copy-Item build\bin\Release\*.dll dist\'
    if ($failures.Count -ne $before + 7) {
        throw "Architecture policy self-test expected seven violations; observed $($failures.Count - $before)"
    }
    $failures.RemoveRange($before, 7)
}

if ($failures.Count -gt 0) {
    $failures | ForEach-Object { Write-Error $_ }
    exit 1
}
Write-Output 'ARCHITECTURE_POLICY_OK'
