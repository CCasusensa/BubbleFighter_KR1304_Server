param(
    [int]$AuthPort = 39000,
    [string]$RegressionExe = "$PSScriptRoot/../Artifacts/Tests/x64/Debug/BubbleFighter.Regression.exe"
)
$ErrorActionPreference = 'Stop'
# An isolated, short-lived machine certificate validates Schannel without a DB.
$rsa = [System.Security.Cryptography.RSA]::Create(2048)
$request = [System.Security.Cryptography.X509Certificates.CertificateRequest]::new(
    'CN=localhost', $rsa, [System.Security.Cryptography.HashAlgorithmName]::SHA256,
    [System.Security.Cryptography.RSASignaturePadding]::Pkcs1)
$usage = [System.Security.Cryptography.OidCollection]::new()
$usage.Add([System.Security.Cryptography.Oid]::new('1.3.6.1.5.5.7.3.1')) | Out-Null
$request.CertificateExtensions.Add(
    [System.Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension]::new($usage, $true))
$san = [System.Security.Cryptography.X509Certificates.SubjectAlternativeNameBuilder]::new()
$san.AddDnsName('localhost')
$san.AddIpAddress([System.Net.IPAddress]::Loopback)
$request.CertificateExtensions.Add($san.Build())
$ephemeral = $request.CreateSelfSigned([DateTimeOffset]::Now.AddMinutes(-1), [DateTimeOffset]::Now.AddHours(1))
$certificate = $null
$privateKey = $null
$store = [System.Security.Cryptography.X509Certificates.X509Store]::new('My', 'LocalMachine')
try {
    $certificate = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new(
        $ephemeral.Export([System.Security.Cryptography.X509Certificates.X509ContentType]::Pfx),
        [string]::Empty,
        [System.Security.Cryptography.X509Certificates.X509KeyStorageFlags]::MachineKeySet -bor
        [System.Security.Cryptography.X509Certificates.X509KeyStorageFlags]::PersistKeySet)
    $privateKey = [System.Security.Cryptography.X509Certificates.RSACertificateExtensions]::GetRSAPrivateKey($certificate)
    $store.Open([System.Security.Cryptography.X509Certificates.OpenFlags]::ReadWrite)
    $store.Add($certificate)
    $outputDirectory = Join-Path $PSScriptRoot '../Artifacts/Tests/tls'
    New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
    $publicPath = Join-Path $outputDirectory 'transport-test.cer'
    [System.IO.File]::WriteAllBytes($publicPath,
        $certificate.Export([System.Security.Cryptography.X509Certificates.X509ContentType]::Cert))
    & "$PSScriptRoot/auth_tls_smoke_test.ps1" -TransportOnly `
        -Thumbprint $certificate.Thumbprint -PublicCertificatePath $publicPath `
        -AuthPort $AuthPort -RegressionExe $RegressionExe
    $unkeyedRsa = [System.Security.Cryptography.RSA]::Create(2048)
    $unkeyedRequest = [System.Security.Cryptography.X509Certificates.CertificateRequest]::new(
        'CN=unkeyed-auth-test', $unkeyedRsa, [System.Security.Cryptography.HashAlgorithmName]::SHA256,
        [System.Security.Cryptography.RSASignaturePadding]::Pkcs1)
    $unkeyed = $unkeyedRequest.CreateSelfSigned([DateTimeOffset]::Now.AddMinutes(-1), [DateTimeOffset]::Now.AddHours(1))
    $publicOnly = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new(
        $unkeyed.Export([System.Security.Cryptography.X509Certificates.X509ContentType]::Cert))
    try {
        $store.Add($publicOnly)
        $server = Join-Path $PSScriptRoot '../Artifacts/Bin/x64/Debug/BubbleFighter.Server.exe'
        $output = & $server --listen-port 39001 --auth-port "$AuthPort" `
            --auth-cert-thumbprint $publicOnly.Thumbprint `
            --database-config 'missing-auth-test.ini' 2>&1 | Out-String
        if ($LASTEXITCODE -eq 0 -or $output -match 'Listening on' -or
            $output -notmatch 'Invalid auth TLS certificate or private key') {
            throw "Certificate without private key was not rejected before listening: $output"
        }
        Write-Output 'Certificate without private key: preflight rejection passed.'
    }
    finally {
        $store.Remove($publicOnly)
        $publicOnly.Dispose()
        $unkeyed.Dispose()
        $unkeyedRsa.Dispose()
    }
}
finally {
    if ($certificate) { $store.Remove($certificate) }
    $store.Dispose()
    if ($privateKey) {
        if ($privateKey -is [System.Security.Cryptography.RSACng]) {
            $privateKey.Key.Delete()
        } elseif ($privateKey -is [System.Security.Cryptography.RSACryptoServiceProvider]) {
            $privateKey.PersistKeyInCsp = $false
            $privateKey.Clear()
        }
        $privateKey.Dispose()
    }
    if ($certificate) { $certificate.Dispose() }
    $ephemeral.Dispose()
    $rsa.Dispose()
}
