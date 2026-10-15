[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$ServerIp,
    [Parameter(Mandatory)]
    [string]$PublicCertificatePath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Invoke-OwnedCleanup([scriptblock]$Action, [string]$Resource) {
    try { & $Action }
    catch {
        # Cleanup diagnostics must neither disclose exception details nor stop later cleanup.
        try { Write-Warning "Could not clean up invocation-owned $Resource." -WarningAction Continue }
        catch { }
    }
}

$address = $null
if ($ServerIp -cnotmatch '^(0|[1-9][0-9]{0,2})(\.(0|[1-9][0-9]{0,2})){3}$' -or
    -not [System.Net.IPAddress]::TryParse($ServerIp, [ref]$address) -or
    $address.AddressFamily -ne [System.Net.Sockets.AddressFamily]::InterNetwork -or
    $address.ToString() -cne $ServerIp) {
    throw 'ServerIp must be a canonical dotted IPv4 address.'
}
$firstOctet = $address.GetAddressBytes()[0]
if ($firstOctet -eq 0 -or $firstOctet -ge 224) {
    throw 'ServerIp must be a unicast IPv4 SAN target, not an unspecified, multicast, or reserved address.'
}

$providerInfo = $null
$driveInfo = $null
$outputPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath(
    $PublicCertificatePath, [ref]$providerInfo, [ref]$driveInfo)
if ($providerInfo.Name -ne 'FileSystem' -or [System.IO.Path]::GetExtension($outputPath) -ine '.cer') {
    throw 'PublicCertificatePath must be a filesystem path ending in .cer.'
}
if (Test-Path -LiteralPath $outputPath) { throw 'Public certificate output already exists.' }
if (-not [System.IO.Directory]::Exists([System.IO.Path]::GetDirectoryName($outputPath))) {
    throw 'Public certificate output directory must already exist.'
}

$invocationId = [Guid]::NewGuid().ToString('N')
$keyName = "KrAuth_$invocationId"
$friendlyName = "KR1304 auth $invocationId"
$keyProvider = [System.Security.Cryptography.CngProvider]::MicrosoftSoftwareKeyStorageProvider
$certificate = $null
$privateKey = $null
$outputStream = $null
$outputCreated = $false
try {
    # BoringSSL requires coherent CA/signing constraints for this self-signed trust anchor.
    $certificate = New-SelfSignedCertificate -Subject "CN=$ServerIp" `
        -CertStoreLocation 'Cert:\LocalMachine\My' -Type Custom `
        -KeyAlgorithm RSA -KeyLength 3072 -HashAlgorithm SHA256 `
        -Provider $keyProvider.Provider -Container $keyName -KeyExportPolicy NonExportable `
        -KeyUsage DigitalSignature, KeyEncipherment, CertSign -FriendlyName $friendlyName `
        -NotAfter (Get-Date).AddYears(1) `
        -TextExtension @("2.5.29.17={text}IPAddress=$ServerIp", '2.5.29.37={text}1.3.6.1.5.5.7.3.1',
            '2.5.29.19={critical}{text}CA=true&pathlength=0')
    if (-not $certificate.HasPrivateKey) { throw 'Generated machine certificate has no private key.' }
    $privateKey = [System.Security.Cryptography.X509Certificates.RSACertificateExtensions]::GetRSAPrivateKey($certificate)
    if ($privateKey -isnot [System.Security.Cryptography.RSACng] -or
        -not $privateKey.Key.IsMachineKey -or $privateKey.Key.KeyName -cne $keyName -or
        $privateKey.Key.ExportPolicy -ne [System.Security.Cryptography.CngExportPolicies]::None) {
        throw 'Generated certificate must use the invocation-owned nonexportable machine key.'
    }
    $expectedSan = [System.Security.Cryptography.X509Certificates.SubjectAlternativeNameBuilder]::new()
    $expectedSan.AddIpAddress($address)
    $actualSan = $certificate.Extensions['2.5.29.17']
    # Compare DER extension bytes, independent of localized Subject/Format output.
    if ($null -eq $actualSan -or
        [Convert]::ToBase64String($actualSan.RawData) -cne
        [Convert]::ToBase64String($expectedSan.Build().RawData)) {
        throw 'Generated certificate SAN does not contain exactly the requested IPv4 address.'
    }
    $eku = $certificate.Extensions['2.5.29.37']
    if ($null -eq $eku -or $eku.EnhancedKeyUsages.Value -notcontains '1.3.6.1.5.5.7.3.1') {
        throw 'Generated certificate is missing the serverAuth EKU.'
    }

    $publicDer = $certificate.Export([System.Security.Cryptography.X509Certificates.X509ContentType]::Cert)
    # CreateNew atomically refuses an output created after the preflight check.
    $outputStream = [System.IO.FileStream]::new($outputPath, [System.IO.FileMode]::CreateNew,
        [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
    $outputCreated = $true
    $outputStream.Write($publicDer, 0, $publicDer.Length)
    $outputStream.Flush($true)
    $outputStream.Dispose()
    $outputStream = $null
    Write-Output $certificate.Thumbprint
}
catch {
    $operationFailure = $_
    if ($outputStream) {
        Invoke-OwnedCleanup { $outputStream.Dispose() } 'output stream'
        $outputStream = $null
    }
    if ($outputCreated) {
        Invoke-OwnedCleanup { Remove-Item -LiteralPath $outputPath -Force } 'partial public certificate file'
    }
    # The unique name also finds a certificate if the PKI cmdlet failed after creating it.
    $ownedCertificates = @(Invoke-OwnedCleanup {
        Get-ChildItem 'Cert:\LocalMachine\My' | Where-Object { $_.FriendlyName -ceq $friendlyName }
    } 'certificate lookup')
    foreach ($ownedCertificate in $ownedCertificates) {
        Invoke-OwnedCleanup {
            Remove-Item -LiteralPath "Cert:\LocalMachine\My\$($ownedCertificate.Thumbprint)"
        } 'machine certificate'
        Invoke-OwnedCleanup { $ownedCertificate.Dispose() } 'certificate handle'
    }
    Invoke-OwnedCleanup {
        if ([System.Security.Cryptography.CngKey]::Exists(
                $keyName, $keyProvider, [System.Security.Cryptography.CngKeyOpenOptions]::MachineKey)) {
            $ownedKey = [System.Security.Cryptography.CngKey]::Open(
                $keyName, $keyProvider, [System.Security.Cryptography.CngKeyOpenOptions]::MachineKey)
            try { $ownedKey.Delete() }
            finally { Invoke-OwnedCleanup { $ownedKey.Dispose() } 'machine key handle' }
        }
    } 'machine key'
    throw $operationFailure
}
finally {
    if ($outputStream) { Invoke-OwnedCleanup { $outputStream.Dispose() } 'output stream' }
    if ($privateKey) { Invoke-OwnedCleanup { $privateKey.Dispose() } 'private key handle' }
    if ($certificate) { Invoke-OwnedCleanup { $certificate.Dispose() } 'certificate handle' }
}
