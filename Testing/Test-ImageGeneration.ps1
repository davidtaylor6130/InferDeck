[CmdletBinding()]
param(
    [string]$BaseUrl = 'http://127.0.0.1:11434/v1',
    [Parameter(Mandatory = $true)]
    [string]$Model,
    [string]$Prompt = 'A small red fox beside a blue camping mug, studio lighting',
    [ValidatePattern('^\d+x\d+$')]
    [string]$Size = '512x512',
    [string]$Output = 'image-validation.png',
    [string]$ApiKey = '',
    [ValidateRange(1, 3600)]
    [int]$TimeoutSeconds = 300
)

$ErrorActionPreference = 'Stop'
[System.Net.ServicePointManager]::Expect100Continue = $false

if ($Size -notmatch '^(\d+)x(\d+)$') {
    throw "Invalid image size: $Size"
}
$expectedWidth = [int]$Matches[1]
$expectedHeight = [int]$Matches[2]
$headers = @{}
if (![string]::IsNullOrWhiteSpace($ApiKey)) {
    $headers.Authorization = "Bearer $ApiKey"
}
$body = @{
    model = $Model
    prompt = $Prompt
    size = $Size
    n = 1
    response_format = 'b64_json'
} | ConvertTo-Json -Compress

$watch = [System.Diagnostics.Stopwatch]::StartNew()
$response = Invoke-WebRequest `
    -UseBasicParsing `
    -Uri ($BaseUrl.TrimEnd('/') + '/images/generations') `
    -Method Post `
    -ContentType 'application/json' `
    -Headers $headers `
    -Body $body `
    -TimeoutSec $TimeoutSeconds
$watch.Stop()

if ($response.StatusCode -ne 200) {
    throw "Unexpected image response status: $($response.StatusCode)"
}
$payload = $response.Content | ConvertFrom-Json
$images = @($payload.data)
if ($payload.output_format -ne 'png' -or $images.Count -ne 1 -or
    [string]::IsNullOrWhiteSpace($images[0].b64_json)) {
    throw 'OpenAI image response schema is invalid'
}
$bytes = [Convert]::FromBase64String($images[0].b64_json)
$signature = [byte[]](137, 80, 78, 71, 13, 10, 26, 10)
for ($index = 0; $index -lt $signature.Length; ++$index) {
    if ($bytes[$index] -ne $signature[$index]) {
        throw 'Returned image is not a PNG'
    }
}
$width = [System.Net.IPAddress]::NetworkToHostOrder(
    [BitConverter]::ToInt32($bytes, 16))
$height = [System.Net.IPAddress]::NetworkToHostOrder(
    [BitConverter]::ToInt32($bytes, 20))
if ($width -ne $expectedWidth -or $height -ne $expectedHeight) {
    throw "Unexpected PNG dimensions: ${width}x${height}"
}

$outputPath = [IO.Path]::GetFullPath($Output)
$outputDirectory = [IO.Path]::GetDirectoryName($outputPath)
[IO.Directory]::CreateDirectory($outputDirectory) | Out-Null
[IO.File]::WriteAllBytes($outputPath, $bytes)
$hash = (Get-FileHash -LiteralPath $outputPath -Algorithm SHA256).Hash.ToLowerInvariant()

[pscustomobject]@{
    Status = 'passed'
    Model = $Model
    DurationSeconds = [math]::Round($watch.Elapsed.TotalSeconds, 2)
    Bytes = $bytes.Length
    Width = $width
    Height = $height
    Sha256 = $hash
    Output = $outputPath
}
