param(
    [string]$Gateway = 'build\bin\Release\inferdeck-gateway.exe',
    [string]$Config = 'config\gateway.test-security.yml',
    [string]$LanAddress = '',
    [int]$Port = 11439,
    [int]$StartupTimeoutSeconds = 55,
    [switch]$RemoteControl,
    [switch]$TestSlowClient
)

$ErrorActionPreference = 'Stop'
if ($RemoteControl) {
    if (-not $PSBoundParameters.ContainsKey('Config')) {
        $Config = 'config\gateway.test-security-remote.yml'
    }
    if (-not $PSBoundParameters.ContainsKey('Port')) {
        $Port = 11440
    }
}
$gatewayPath = (Resolve-Path -LiteralPath $Gateway).Path
$configPath = (Resolve-Path -LiteralPath $Config).Path
if (-not $LanAddress) {
    $LanAddress = [Net.Dns]::GetHostAddresses([Net.Dns]::GetHostName()) |
        Where-Object {
            $_.AddressFamily -eq [Net.Sockets.AddressFamily]::InterNetwork -and
            -not [Net.IPAddress]::IsLoopback($_)
        } |
        Select-Object -First 1 -ExpandProperty IPAddressToString
    if (-not $LanAddress) {
        throw 'No non-loopback IPv4 address is available for the remote-control security probe'
    }
}
$logSuffix = [DateTime]::UtcNow.ToString('yyyyMMddHHmmssfff')
$logDirectory = Split-Path $gatewayPath -Parent
$stdoutPath = Join-Path $logDirectory "security-fixture-$Port-$logSuffix.stdout.log"
$stderrPath = Join-Path $logDirectory "security-fixture-$Port-$logSuffix.stderr.log"

function Test-LoopbackPort {
    param([int]$TargetPort)
    $client = [Net.Sockets.TcpClient]::new()
    try {
        $connect = $client.ConnectAsync('127.0.0.1', $TargetPort)
        return $connect.Wait(200) -and $client.Connected
    } catch {
        return $false
    } finally {
        $client.Dispose()
    }
}

if (Test-LoopbackPort $Port) {
    throw "Port $Port is already in use"
}

function Invoke-Status {
    param(
        [string]$Uri,
        [string]$Method = 'GET',
        [hashtable]$Headers = @{},
        [string]$ContentType = '',
        [string]$Body = ''
    )
    try {
        $parameters = @{
            UseBasicParsing = $true
            Uri = $Uri
            Method = $Method
            Headers = $Headers
            TimeoutSec = 10
        }
        if ($ContentType) { $parameters.ContentType = $ContentType }
        if ($Body) { $parameters.Body = $Body }
        return [int](Invoke-WebRequest @parameters).StatusCode
    } catch {
        if ($_.Exception.Response) {
            return [int]$_.Exception.Response.StatusCode
        }
        throw
    }
}

function Invoke-RawStatus {
    param(
        [int]$TargetPort,
        [string]$RequestText
    )
    $client = [Net.Sockets.TcpClient]::new()
    try {
        $client.ReceiveTimeout = 10000
        $client.Connect('127.0.0.1', $TargetPort)
        $stream = $client.GetStream()
        $bytes = [Text.Encoding]::ASCII.GetBytes($RequestText)
        $stream.Write($bytes, 0, $bytes.Length)
        $reader = [IO.StreamReader]::new($stream, [Text.Encoding]::ASCII, $false, 1024, $true)
        $statusLine = $reader.ReadLine()
        if ($statusLine -notmatch '^HTTP/1\.[01] ([0-9]{3}) ') {
            throw "Invalid HTTP status line: $statusLine"
        }
        return [int]$Matches[1]
    } finally {
        $client.Dispose()
    }
}

function Invoke-RawResponse {
    param(
        [int]$TargetPort,
        [string]$RequestText
    )
    $client = [Net.Sockets.TcpClient]::new()
    try {
        $client.ReceiveTimeout = 10000
        $client.SendTimeout = 10000
        $client.Connect('127.0.0.1', $TargetPort)
        $stream = $client.GetStream()
        $bytes = [Text.Encoding]::ASCII.GetBytes($RequestText)
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush()
        $reader = [IO.StreamReader]::new(
            $stream, [Text.Encoding]::UTF8, $false, 4096, $true)
        $statusLine = $reader.ReadLine()
        if ($statusLine -notmatch '^HTTP/1\.[01] ([0-9]{3}) ') {
            throw "Invalid HTTP status line: $statusLine"
        }
        $status = [int]$Matches[1]
        $headers = @{}
        while (($line = $reader.ReadLine()) -ne '') {
            if ($null -eq $line) { throw 'HTTP response ended before its headers' }
            $colon = $line.IndexOf(':')
            if ($colon -gt 0) {
                $headers[$line.Substring(0, $colon).Trim()] =
                    $line.Substring($colon + 1).Trim()
            }
        }
        $contentLength = 0
        if ($headers.ContainsKey('Content-Length')) {
            $contentLength = [int]$headers['Content-Length']
        }
        if ($headers.ContainsKey('Content-Length')) {
            $characters = [char[]]::new($contentLength)
            $read = 0
            while ($read -lt $contentLength) {
                $count = $reader.Read($characters, $read, $contentLength - $read)
                if ($count -le 0) { throw 'HTTP response ended before its body was complete' }
                $read += $count
            }
            $body = [string]::new($characters)
        } else {
            $builder = [Text.StringBuilder]::new()
            while ($builder.Length -lt 65536) {
                $value = $reader.Read()
                if ($value -lt 0) { break }
                [void]$builder.Append([char]$value)
                if ($value -eq [int][char]'}') {
                    try {
                        [void]($builder.ToString() | ConvertFrom-Json -ErrorAction Stop)
                        break
                    } catch {
                    }
                }
            }
            $body = $builder.ToString()
        }
        return [pscustomobject]@{
            Status = $status
            Headers = $headers
            Body = $body
        }
    } finally {
        $client.Dispose()
    }
}

$process = Start-Process -FilePath $gatewayPath `
    -ArgumentList @('-c', $configPath) `
    -WorkingDirectory (Get-Location).Path `
    -WindowStyle Hidden `
    -RedirectStandardOutput $stdoutPath `
    -RedirectStandardError $stderrPath `
    -PassThru

try {
    $deadline = [DateTime]::UtcNow.AddSeconds($StartupTimeoutSeconds)
    do {
        Start-Sleep -Milliseconds 100
        if ($process.HasExited) {
            $diagnostic = (Get-Content -LiteralPath $stderrPath -Tail 30 `
                -ErrorAction SilentlyContinue) -join "`n"
            throw "Security fixture exited with code $($process.ExitCode): $diagnostic"
        }
        $ready = Test-LoopbackPort $Port
    } while (-not $ready -and [DateTime]::UtcNow -lt $deadline)
    if (-not $ready) {
        $diagnostic = (Get-Content -LiteralPath $stderrPath -Tail 30 `
            -ErrorAction SilentlyContinue) -join "`n"
        throw "Security fixture did not listen: $diagnostic"
    }

    $loopback = "http://127.0.0.1:$Port"
    $lan = "http://${LanAddress}:$Port"
    $dataHeader = @{Authorization = 'Bearer test-data-token'}
    $controlHeader = @{Authorization = 'Bearer test-control-token-0123456789abcdef'}
    $preflightOrigin = if ($RemoteControl) {
        'http://admin.example'
    } else {
        "http://127.0.0.1:$Port"
    }
    $expectResponse = Invoke-RawResponse $Port `
        "POST /v1/chat/completions HTTP/1.1`r`nHost: 127.0.0.1:$Port`r`nAuthorization: Bearer test-data-token`r`nOrigin: https://client.example`r`nX-Request-Id: client.expect_1`r`nContent-Type: application/json`r`nContent-Length: 1000000`r`nExpect: 100-continue`r`nConnection: close`r`n`r`n"
    $mixedExpectResponse = Invoke-RawResponse $Port `
        "POST /v1/chat/completions HTTP/1.1`r`nHost: 127.0.0.1:$Port`r`nAuthorization: Bearer test-data-token`r`nContent-Type: application/json`r`nContent-Length: 1000000`r`nExpect: 100-Continue`r`nConnection: close`r`n`r`n"
    $unsupportedExpectResponse = Invoke-RawResponse $Port `
        "POST /v1/chat/completions HTTP/1.1`r`nHost: 127.0.0.1:$Port`r`nAuthorization: Bearer test-data-token`r`nContent-Type: application/json`r`nContent-Length: 1000000`r`nExpect: unsupported`r`nConnection: close`r`n`r`n"
    $duplicateExpectResponse = Invoke-RawResponse $Port `
        "POST /v1/chat/completions HTTP/1.1`r`nHost: 127.0.0.1:$Port`r`nAuthorization: Bearer test-data-token`r`nContent-Type: application/json`r`nContent-Length: 1000000`r`nExpect:`r`nExpect: 100-Continue`r`nConnection: close`r`n`r`n"
    $eagerBody = 'x' * 65536
    $eagerRejectedResponse = Invoke-RawResponse $Port `
        "POST /v1/chat/completions HTTP/1.1`r`nHost: 127.0.0.1:$Port`r`nContent-Type: application/json`r`nContent-Length: $($eagerBody.Length)`r`nConnection: close`r`n`r`n$eagerBody"

    $results = [ordered]@{
        data_without_token = Invoke-Status "$loopback/v1/models"
        data_with_token = Invoke-Status "$loopback/v1/models" -Headers $dataHeader
        legacy_health = Invoke-Status "$loopback/v1/health" -Headers $dataHeader
        legacy_swap = Invoke-Status "$loopback/v1/swap/status" -Headers $dataHeader
        legacy_messages = Invoke-Status "$loopback/v1/messages" `
            -Method POST -Headers $dataHeader -ContentType 'application/json' `
            -Body '{"model":"legacy","messages":[],"max_tokens":1}'
        disabled_derivative = Invoke-Status `
            "$loopback/compat/openai-derivative/v1/chat/completions"
        control_health_lan = Invoke-Status "$lan/api/inferdeck/v1/health"
        control_loopback = Invoke-Status "$loopback/api/inferdeck/v1/config"
        control_lan_without_token = Invoke-Status "$lan/api/inferdeck/v1/config"
        control_lan_data_token = Invoke-Status "$lan/api/inferdeck/v1/config" -Headers $dataHeader
        control_lan_control_token = Invoke-Status "$lan/api/inferdeck/v1/config" -Headers $controlHeader
        dashboard_lan_control_token = Invoke-Status "$lan/api/inferdeck/v1/status" -Headers $controlHeader
        control_preflight_lan = Invoke-Status "$lan/api/inferdeck/v1/config" `
            -Method OPTIONS `
            -Headers @{
                Origin = $preflightOrigin
                'Access-Control-Request-Method' = 'PUT'
            }
        control_loopback_cross_origin = Invoke-Status "$loopback/api/inferdeck/v1/swap/cancel" `
            -Method POST `
            -Headers @{
                Origin = 'https://evil.example'
                'Sec-Fetch-Site' = 'cross-site'
            } `
            -ContentType 'application/json'
        control_loopback_missing_content_type = Invoke-Status `
            "$loopback/api/inferdeck/v1/swap/cancel" -Method POST
        wrong_content_type = Invoke-Status "$loopback/v1/chat/completions" `
            -Method POST `
            -Headers $dataHeader `
            -ContentType 'text/plain' `
            -Body '{}'
        unauthenticated_large_body = Invoke-RawStatus $Port `
            "POST /v1/chat/completions HTTP/1.1`r`nHost: 127.0.0.1:$Port`r`nContent-Type: application/json`r`nContent-Length: 500000000`r`nConnection: close`r`n`r`n"
        authenticated_oversized_json = Invoke-RawStatus $Port `
            "POST /v1/chat/completions HTTP/1.1`r`nHost: 127.0.0.1:$Port`r`nAuthorization: Bearer test-data-token`r`nContent-Type: application/json`r`nContent-Length: 17825792`r`nConnection: close`r`n`r`n"
        authenticated_oversized_audio = Invoke-RawStatus $Port `
            "POST /v1/audio/transcriptions HTTP/1.1`r`nHost: 127.0.0.1:$Port`r`nAuthorization: Bearer test-data-token`r`nContent-Type: multipart/form-data; boundary=test`r`nContent-Length: 28311552`r`nConnection: close`r`n`r`n"
        chunked_json = Invoke-RawStatus $Port `
            "POST /v1/chat/completions HTTP/1.1`r`nHost: 127.0.0.1:$Port`r`nAuthorization: Bearer test-data-token`r`nContent-Type: application/json`r`nTransfer-Encoding: chunked`r`nConnection: close`r`n`r`n"
        expect_continue = $expectResponse.Status
        mixed_case_expect = $mixedExpectResponse.Status
        unsupported_expect = $unsupportedExpectResponse.Status
        duplicate_expect = $duplicateExpectResponse.Status
        eager_body_rejection = $eagerRejectedResponse.Status
        loopback_proxy_bypass = Invoke-RawStatus $Port `
            "GET /api/inferdeck/v1/config HTTP/1.1`r`nHost: admin.example`r`nForwarded: for=192.0.2.10`r`nConnection: close`r`n`r`n"
    }

    $echo = Invoke-WebRequest -UseBasicParsing -Uri "$loopback/api/inferdeck/v1/health" `
        -Headers @{'X-Request-Id' = 'client.probe_1'} -TimeoutSec 10
    $generated = Invoke-WebRequest -UseBasicParsing -Uri "$loopback/api/inferdeck/v1/health" `
        -Headers @{'X-Request-Id' = 'invalid request id'} -TimeoutSec 10
    $health = $echo.Content | ConvertFrom-Json
    if ($health.version -notmatch '^\d+\.\d+\.\d+$' -or
        $health.build_revision -notmatch '^[0-9a-f]{40}$' -or
        -not ($health.PSObject.Properties.Name -contains 'build_dirty')) {
        throw "Health response has no trustworthy build provenance: $($echo.Content)"
    }
    $results.build_revision = $health.build_revision
    $results.build_dirty = [bool]$health.build_dirty
    $results.request_id_echo = $echo.Headers['X-Request-Id']
    $results.request_id_generated = $generated.Headers['X-Request-Id']

    $expected = @{
        data_without_token = 401
        data_with_token = 200
        legacy_health = 404
        legacy_swap = 404
        legacy_messages = 404
        disabled_derivative = 404
        control_health_lan = if ($RemoteControl) { 401 } else { 403 }
        control_loopback = 200
        control_loopback_cross_origin = 403
        control_loopback_missing_content_type = 415
        wrong_content_type = 415
        dashboard_lan_control_token = if ($RemoteControl) { 200 } else { 403 }
        unauthenticated_large_body = 401
        authenticated_oversized_json = 413
        authenticated_oversized_audio = 413
        chunked_json = 411
        expect_continue = 417
        mixed_case_expect = 417
        unsupported_expect = 417
        duplicate_expect = 417
        eager_body_rejection = 401
    }
    if ($RemoteControl) {
        $expected.control_lan_without_token = 401
        $expected.control_lan_data_token = 401
        $expected.control_lan_control_token = 200
        $expected.control_preflight_lan = 204
        $expected.loopback_proxy_bypass = 401
    } else {
        $expected.control_lan_without_token = 403
        $expected.control_lan_data_token = 403
        $expected.control_lan_control_token = 403
        $expected.control_preflight_lan = 403
        $expected.loopback_proxy_bypass = 403
    }
    foreach ($entry in $expected.GetEnumerator()) {
        if ($results[$entry.Key] -ne $entry.Value) {
            throw "$($entry.Key) expected $($entry.Value), got $($results[$entry.Key])"
        }
    }
    if ($results.request_id_echo -ne 'client.probe_1') {
        throw 'Request ID was not echoed'
    }
    if ($results.request_id_generated -notmatch '^req_[0-9a-f]+_[0-9a-f]+$') {
        throw "Generated request ID is invalid: $($results.request_id_generated)"
    }
    if ($expectResponse.Headers['X-Request-Id'] -ne 'client.expect_1') {
        throw 'Expect rejection did not echo its request ID'
    }
    if ($expectResponse.Headers['Access-Control-Allow-Origin'] -ne '*') {
        throw 'Expect rejection did not apply data-plane CORS'
    }
    $expectError = $expectResponse.Body | ConvertFrom-Json
    if ($expectError.error.code -ne 'expectation_failed') {
        throw "Expect rejection did not return the structured error body: $($expectResponse.Headers | ConvertTo-Json -Compress) body=$($expectResponse.Body)"
    }
    $eagerError = $eagerRejectedResponse.Body | ConvertFrom-Json
    if ($eagerError.error.code -ne 'unauthorized') {
        throw 'Eager-body rejection did not return the complete structured error body'
    }
    if ($RemoteControl) {
        $allowed = Invoke-WebRequest -UseBasicParsing -Uri "$lan/api/inferdeck/v1/config" `
            -Method OPTIONS `
            -Headers @{
                Origin = 'http://admin.example'
                'Access-Control-Request-Method' = 'PUT'
            } `
            -TimeoutSec 10
        if ($allowed.Headers['Access-Control-Allow-Origin'] -ne 'http://admin.example') {
            throw 'Allowlisted control origin was not returned'
        }
        $rejected = Invoke-Status "$lan/api/inferdeck/v1/config" `
            -Method OPTIONS `
            -Headers @{
                Origin = 'http://untrusted.example'
                'Access-Control-Request-Method' = 'PUT'
            }
        if ($rejected -ne 403) {
            throw "Untrusted control origin returned $rejected"
        }
    }
    if ($TestSlowClient) {
        $slowClient = [Net.Sockets.TcpClient]::new()
        try {
            $slowClient.ReceiveTimeout = 5000
            $slowClient.Connect('127.0.0.1', $Port)
            $stream = $slowClient.GetStream()
            $headers = [Text.Encoding]::ASCII.GetBytes(
                "POST /v1/chat/completions HTTP/1.1`r`nHost: 127.0.0.1:$Port`r`nAuthorization: Bearer test-data-token`r`nContent-Type: application/json`r`nContent-Length: 1024`r`nConnection: close`r`n`r`n")
            $stream.Write($headers, 0, $headers.Length)
            $timer = [Diagnostics.Stopwatch]::StartNew()
            while ($timer.Elapsed.TotalSeconds -lt 45) {
                Start-Sleep -Seconds 3
                if ($slowClient.Client.Poll(0, [Net.Sockets.SelectMode]::SelectRead) -and
                    $slowClient.Available -eq 0) {
                    break
                }
                try {
                    $stream.WriteByte([byte][char]' ')
                    $stream.Flush()
                } catch [IO.IOException] {
                    break
                }
            }
            $timer.Stop()
            if ($timer.Elapsed.TotalSeconds -lt 25 -or
                $timer.Elapsed.TotalSeconds -gt 45) {
                throw "Slow-client deadline was $($timer.Elapsed.TotalSeconds) seconds"
            }
            $results.slow_client_deadline_seconds =
                [Math]::Round($timer.Elapsed.TotalSeconds, 3)
        } finally {
            $slowClient.Dispose()
        }
    }
    Start-Sleep -Milliseconds 100
    $requestLog = (Get-Content -LiteralPath $stdoutPath -ErrorAction Stop) -join "`n"
    if ($requestLog -notmatch 'event=http_request_begin request_id=client\.probe_1' -or
        $requestLog -notmatch 'event=http_response_committed request_id=client\.probe_1') {
        throw 'Request ID was not present in begin and response-commit records'
    }
    if ($requestLog -notmatch 'event=http_request_begin request_id=client\.expect_1' -or
        $requestLog -notmatch 'event=http_response_committed request_id=client\.expect_1') {
        throw 'Expect request ID was not present in begin and response-commit records'
    }
    if ($requestLog -match 'event=(?:http_request_begin|http_response_committed) request_id=\s') {
        throw 'A structured request record has an empty request ID'
    }
    [pscustomobject]$results | ConvertTo-Json -Compress
} finally {
    $running = Get-Process -Id $process.Id -ErrorAction SilentlyContinue
    if ($running) {
        $actualPath = $running.Path
        if ($actualPath -ne $gatewayPath) {
            throw "Refusing to stop unexpected process $($running.Id): $actualPath"
        }
        Stop-Process -Id $running.Id -ErrorAction Stop
        Wait-Process -Id $running.Id -Timeout 10 -ErrorAction SilentlyContinue
    }
}
