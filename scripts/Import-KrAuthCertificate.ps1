[CmdletBinding()]
param(
    [string]$PackageDirectory = $PSScriptRoot,
    [Security.SecureString]$Password
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this script from an administrator PowerShell on the destination server.'
}
$configuration = Join-Path $PackageDirectory 'config'
$settings = Get-Content -LiteralPath (Join-Path $configuration 'server.json') -Raw | ConvertFrom-Json
$public = [Security.Cryptography.X509Certificates.X509Certificate2]::new(
    [IO.File]::ReadAllBytes((Join-Path $configuration 'auth-server.cer')))
$private = $null
try {
    if ($public.Thumbprint -ine $settings.auth_cert_thumbprint) {
        throw 'Public certificate and server.json thumbprint do not match.'
    }
    $existing = Get-Item -LiteralPath "Cert:\LocalMachine\My\$($public.Thumbprint)" -ErrorAction SilentlyContinue
    if ($existing -and $existing.HasPrivateKey) {
        Write-Output "Certificate already installed: $($public.Thumbprint)"
        return
    }
    if (!$Password) { $Password = Read-Host 'Server certificate PFX password' -AsSecureString }
    $pfxPath = Join-Path $configuration 'auth-server.pfx'
    $private = [Security.Cryptography.X509Certificates.X509Certificate2]::new(
        [IO.File]::ReadAllBytes($pfxPath), $Password,
        [Security.Cryptography.X509Certificates.X509KeyStorageFlags]::EphemeralKeySet)
    if (!$private.HasPrivateKey -or $private.Thumbprint -ine $public.Thumbprint -or
        $private.NotBefore -gt (Get-Date) -or $private.NotAfter -le (Get-Date)) {
        throw 'PFX must contain the current matching certificate and private key.'
    }
    # Omit -Exportable: the destination machine keeps a nonexportable key.
    $installed = Import-PfxCertificate -FilePath $pfxPath -Password $Password -CertStoreLocation Cert:\LocalMachine\My
    if (@($installed | Where-Object { $_.Thumbprint -ieq $public.Thumbprint -and $_.HasPrivateKey }).Count -ne 1) {
        throw 'The matching private certificate was not installed.'
    }
    Write-Output "Server authentication certificate installed: $($public.Thumbprint)"
} finally {
    if ($private) { $private.Dispose() }
    $public.Dispose()
}
