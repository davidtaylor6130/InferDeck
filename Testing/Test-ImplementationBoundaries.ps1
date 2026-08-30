param(
    [string]$Root = (Split-Path -Parent $PSScriptRoot)
)

$ErrorActionPreference = 'Stop'
$checks = @(
    @{
        Path = 'apps/inferdeck-gateway/src/main.cpp'
        MaximumLines = 800
        Required = @('process_runtime.ipp', 'dashboard_static.ipp', 'profile_benchmark_runtime.ipp')
        Forbidden = @('LONG WINAPI CrashHandler(', 'ProfileBenchmarkTrialMetrics>
run_profile_benchmark_trial(', 'void write_dashboard_file(')
    },
    @{
        Path = 'libs/gateway/src/openai_adapter.cpp'
        MaximumLines = 800
        Required = @('openai_content_parser.ipp')
        Forbidden = @('foundation::Result<std::vector<std::byte>> decode_base64(', 'foundation::Result<inference::Content> parse_image(', 'foundation::Result<inference::Message> parse_message(')
    },
    @{
        Path = 'libs/gateway/src/responses_adapter.cpp'
        MaximumLines = 800
        Required = @('responses_input_parser.ipp')
        Forbidden = @('foundation::Result<void> parse_input_item(', 'std::optional<std::pair<std::string, std::string>> input_capability(')
    },
    @{
        Path = 'libs/gateway/src/routes.cpp'
        MaximumLines = 800
        Required = @('chat_stream_serialization.ipp')
        Forbidden = @('std::string dump_json(', 'nlohmann::json delta_json(')
    },
    @{
        Path = 'libs/gateway/src/dashboard_routes.cpp'
        MaximumLines = 800
        Required = @('dashboard_config_yaml.ipp')
        Forbidden = @('std::string replace_top_level_yaml_section(', 'foundation::Result<std::string> render_aliases(')
    }
)

$results = foreach ($check in $checks) {
    $path = Join-Path $Root $check.Path
    if (-not (Test-Path -LiteralPath $path)) { throw "Implementation boundary file is missing: $($check.Path)" }
    $content = [IO.File]::ReadAllText($path)
    $lineCount = (Get-Content -LiteralPath $path).Count
    if ($lineCount -gt $check.MaximumLines) {
        throw "$($check.Path) has $lineCount lines; maximum is $($check.MaximumLines)"
    }
    foreach ($required in $check.Required) {
        if (-not $content.Contains($required)) { throw "$($check.Path) does not include $required" }
    }
    foreach ($forbidden in $check.Forbidden) {
        if ($content.Contains($forbidden)) { throw "$($check.Path) still owns extracted implementation: $forbidden" }
    }
    [ordered]@{ path = $check.Path; lines = $lineCount; maximum = $check.MaximumLines }
}

$results | ConvertTo-Json -Compress
