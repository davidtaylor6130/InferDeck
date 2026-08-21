param(
    [string]$Executable,
    [string]$Manifest,
    [string]$CertificateBase64,
    [string]$CertificatePassword
)

$ErrorActionPreference = 'Stop'
if (!$Executable -and !$Manifest) { throw 'Executable or Manifest is required' }
if (!$CertificateBase64) {
    Write-Output 'Signing credentials unavailable; artifact remains unsigned'
    exit 0
}
if (!$CertificatePassword) { throw 'Signing certificate password is required' }
$certificatePath = [System.IO.Path]::GetTempFileName()
try {
    [System.IO.File]::WriteAllBytes($certificatePath, [Convert]::FromBase64String($CertificateBase64))
    $certificate = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new(
        $certificatePath,
        $CertificatePassword,
        [System.Security.Cryptography.X509Certificates.X509KeyStorageFlags]::Exportable)
    if (!$certificate.HasPrivateKey) { throw 'Signing certificate has no private key' }

    if ($Executable) {
        $resolvedExecutable = (Resolve-Path -LiteralPath $Executable).Path
        $signtool = Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\bin' -Filter signtool.exe -Recurse |
            Where-Object FullName -Match '\\x64\\signtool\.exe$' |
            Sort-Object FullName -Descending |
            Select-Object -First 1
        if (!$signtool) { throw 'signtool.exe was not found' }
        & $signtool.FullName sign /fd SHA256 /td SHA256 /tr http://timestamp.digicert.com /f $certificatePath /p $CertificatePassword $resolvedExecutable
        if ($LASTEXITCODE -ne 0) { throw 'Executable signing failed' }
        & $signtool.FullName verify /pa /v $resolvedExecutable
        if ($LASTEXITCODE -ne 0) { throw 'Executable signature verification failed' }
    }

    if ($Manifest) {
        Add-Type -AssemblyName System.Security.Cryptography.Pkcs
        $resolvedManifest = (Resolve-Path -LiteralPath $Manifest).Path
        $content = [System.Security.Cryptography.Pkcs.ContentInfo]::new([System.IO.File]::ReadAllBytes($resolvedManifest))
        $signed = [System.Security.Cryptography.Pkcs.SignedCms]::new($content, $true)
        $signer = [System.Security.Cryptography.Pkcs.CmsSigner]::new($certificate)
        $signer.IncludeOption = [System.Security.Cryptography.X509Certificates.X509IncludeOption]::EndCertOnly
        $signed.ComputeSignature($signer)
        [System.IO.File]::WriteAllBytes("$resolvedManifest.p7s", $signed.Encode())
        $verified = [System.Security.Cryptography.Pkcs.SignedCms]::new($content, $true)
        $verified.Decode([System.IO.File]::ReadAllBytes("$resolvedManifest.p7s"))
        $verified.CheckSignature($true)
    }
} finally {
    if (Test-Path -LiteralPath $certificatePath) {
        Remove-Item -LiteralPath $certificatePath -Force
    }
}
