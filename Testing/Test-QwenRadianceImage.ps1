param(
    [string]$BaseUrl = "http://127.0.0.1:11434",
    [string]$Model = "deep",
    [string]$ApiKey = ""
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

$bitmap = New-Object System.Drawing.Bitmap 64, 64
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$stream = New-Object System.IO.MemoryStream
try {
    $graphics.Clear([System.Drawing.Color]::Red)
    $bitmap.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
    $imageData = [Convert]::ToBase64String($stream.ToArray())
} finally {
    $stream.Dispose()
    $graphics.Dispose()
    $bitmap.Dispose()
}

$payload = @{
    model = $Model
    messages = @(
        @{
            role = "user"
            content = @(
                @{ type = "text"; text = "What is the solid color shown? Reply with the color only." }
                @{ type = "image_url"; image_url = @{ url = "data:image/png;base64,$imageData"; detail = "high" } }
            )
        }
    )
    max_tokens = 32
    temperature = 0
} | ConvertTo-Json -Depth 10

$headers = @{}
if ($ApiKey) {
    $headers.Authorization = "Bearer $ApiKey"
}

$response = Invoke-RestMethod `
    -Method Post `
    -Uri "$($BaseUrl.TrimEnd('/'))/v1/chat/completions" `
    -ContentType "application/json" `
    -Headers $headers `
    -Body $payload

$answer = [string]$response.choices[0].message.content
Write-Host "Model: $($response.model)"
Write-Host "Answer: $answer"
if ($answer -notmatch "(?i)red") {
    throw "The image request returned no answer identifying the red image."
}
