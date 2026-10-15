$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$deploymentScript = Join-Path $PSScriptRoot '../scripts/New-KrAuthCertificate.ps1'
if (-not (Test-Path -LiteralPath $deploymentScript -PathType Leaf)) {
    throw 'Certificate deployment script is missing: scripts/New-KrAuthCertificate.ps1'
}

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Assert-Rejected([scriptblock]$Action, [string]$Message) {
    $rejected = $false
    try { & $Action | Out-Null } catch { $rejected = $true; $script:lastRejection = $_ }
    Assert-True $rejected $Message
}

function Remove-TestCertificate($Certificate) {
    $key = [System.Security.Cryptography.X509Certificates.RSACertificateExtensions]::GetRSAPrivateKey($Certificate)
    try {
        Remove-Item -LiteralPath "Cert:\LocalMachine\My\$($Certificate.Thumbprint)" -ErrorAction SilentlyContinue
        if ($key -is [System.Security.Cryptography.RSACng]) { $key.Key.Delete() }
        elseif ($key -is [System.Security.Cryptography.RSACryptoServiceProvider]) {
            $key.PersistKeyInCsp = $false
            $key.Clear()
        }
    }
    finally {
        if ($key) { $key.Dispose() }
        $Certificate.Dispose()
    }
}

function Remove-FaultResources {
    try {
        if ($faultState.Stream) {
            $faultState.Stream.Dispose()
            $faultState.Stream = $null
        }
        if ($faultState.Certificate) {
            try {
                $certificatePath = "Cert:\LocalMachine\My\$($faultState.Certificate.Thumbprint)"
                if (Test-Path -LiteralPath $certificatePath) {
                    Microsoft.PowerShell.Management\Remove-Item -LiteralPath $certificatePath
                }
            }
            finally {
                $faultState.Certificate.Dispose()
                $faultState.Certificate = $null
            }
        }
    }
    finally {
        if ($faultState.KeyName -and [System.Security.Cryptography.CngKey]::Exists($faultState.KeyName,
                [System.Security.Cryptography.CngProvider]::MicrosoftSoftwareKeyStorageProvider,
                [System.Security.Cryptography.CngKeyOpenOptions]::MachineKey)) {
            $orphanKey = [System.Security.Cryptography.CngKey]::Open($faultState.KeyName,
                [System.Security.Cryptography.CngProvider]::MicrosoftSoftwareKeyStorageProvider,
                [System.Security.Cryptography.CngKeyOpenOptions]::MachineKey)
            try { $orphanKey.Delete() } finally { $orphanKey.Dispose() }
        }
        $faultState.KeyName = $null
    }
}

$testId = [Guid]::NewGuid().ToString('N')
$artifactRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../Artifacts/Tests/certificates'))
$testDirectory = Join-Path $artifactRoot $testId
$storeName = "KrAuthTest_$testId"
$testStore = [System.Security.Cryptography.X509Certificates.X509Store]::new($storeName, 'CurrentUser')
$certificate = $null
$publicCertificate = $null
$privateKey = $null
$faultState = [pscustomobject]@{
    Certificate = $null; KeyName = $null; Stream = $null; Mode = ''; Path = ''; PrimaryError = $null
}
$passed = 0
try {
    New-Item -ItemType Directory -Path $testDirectory | Out-Null
    foreach ($ip in @('0.0.0.0', 'invalid', '256.1.1.1', '127.1', '2130706433',
            '127.00.0.1', ' 127.0.0.1', '127.0.0.1 ', '::1', '0.1.2.3',
            '224.0.0.1', '255.255.255.255', '127.0.0.1&IPAddress=1.2.3.4')) {
        $invalidPath = Join-Path $testDirectory 'invalid.cer'
        Assert-Rejected { & $deploymentScript -ServerIp $ip -PublicCertificatePath $invalidPath } `
            "Invalid IP was accepted: $ip"
        Assert-True (-not (Test-Path -LiteralPath $invalidPath)) "Invalid IP wrote an output: $ip"
        $passed++
    }
    $existingPath = Join-Path $testDirectory 'existing.cer'
    [System.IO.File]::WriteAllText($existingPath, 'keep-existing-public-certificate')
    Assert-Rejected { & $deploymentScript -ServerIp '127.0.0.1' -PublicCertificatePath $existingPath } `
        'Existing output was accepted.'
    Assert-True ([System.IO.File]::ReadAllText($existingPath) -ceq 'keep-existing-public-certificate') `
        'Existing output content changed.'
    $passed++

    $publicPath = Join-Path $testDirectory 'loopback.cer'
    $thumbprint = & $deploymentScript -ServerIp '127.0.0.1' -PublicCertificatePath $publicPath
    Assert-True ($thumbprint -is [string] -and $thumbprint -cmatch '^[0-9A-F]{40}$') `
        'Deployment must return only a certificate thumbprint.'
    $certificate = Get-Item -LiteralPath "Cert:\LocalMachine\My\$thumbprint"
    Assert-True $certificate.HasPrivateKey 'Machine certificate has no private key.'
    $privateKey = [System.Security.Cryptography.X509Certificates.RSACertificateExtensions]::GetRSAPrivateKey($certificate)
    Assert-True ($privateKey -is [System.Security.Cryptography.RSACng]) 'Expected a CNG machine key.'
    Assert-True $privateKey.Key.IsMachineKey 'Private key was not retained as a machine key.'
    Assert-True ($privateKey.Key.ExportPolicy -eq [System.Security.Cryptography.CngExportPolicies]::None) `
        'Private key is exportable.'
    $passed++

    $bytes = [System.IO.File]::ReadAllBytes($publicPath)
    Assert-True ([System.Security.Cryptography.X509Certificates.X509Certificate2]::GetCertContentType($bytes) -eq
        [System.Security.Cryptography.X509Certificates.X509ContentType]::Cert) 'Output is not a DER certificate.'
    $publicCertificate = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new($bytes)
    Assert-True (-not $publicCertificate.HasPrivateKey) 'Public DER certificate contains a private key.'
    Assert-True ($publicCertificate.Thumbprint -ceq $thumbprint) 'Public DER differs from the machine certificate.'
    $san = [System.Security.Cryptography.X509Certificates.X509SubjectAlternativeNameExtension]::new()
    $san.CopyFrom($publicCertificate.Extensions['2.5.29.17'])
    $addresses = @($san.EnumerateIPAddresses())
    Assert-True ($addresses.Count -eq 1 -and $addresses[0].ToString() -ceq '127.0.0.1') `
        'Actual SAN IP does not match loopback.'
    Assert-True (@($san.EnumerateDnsNames()).Count -eq 0) 'Unexpected DNS SAN.'
    $eku = $publicCertificate.Extensions['2.5.29.37']
    Assert-True ($eku.EnhancedKeyUsages.Value -contains '1.3.6.1.5.5.7.3.1') 'Missing serverAuth EKU.'
    $passed++

    $keyUsage = $publicCertificate.Extensions['2.5.29.15']
    Assert-True ($null -ne $keyUsage -and
        ($keyUsage.KeyUsages -band [System.Security.Cryptography.X509Certificates.X509KeyUsageFlags]::KeyCertSign)) `
        'Self-signed TLS trust anchor is missing certificate-signing usage.'
    $constraints = $publicCertificate.Extensions['2.5.29.19']
    Assert-True ($null -ne $constraints -and $constraints.Critical -and
        $constraints.CertificateAuthority -and $constraints.HasPathLengthConstraint -and
        $constraints.PathLengthConstraint -eq 0) `
        'Self-signed TLS trust anchor must be a critical CA with path length zero.'
    $passed++

    $testStore.Open([System.Security.Cryptography.X509Certificates.OpenFlags]::ReadWrite)
    $imported = Import-Certificate -FilePath $publicPath -CertStoreLocation "Cert:\CurrentUser\$storeName"
    try {
        Assert-True (-not $imported.HasPrivateKey) 'Isolated store import contains a private key.'
        $testStore.Close()
        $testStore.Open([System.Security.Cryptography.X509Certificates.OpenFlags]::ReadWrite)
        $found = @($testStore.Certificates.Find(
                [System.Security.Cryptography.X509Certificates.X509FindType]::FindByThumbprint, $thumbprint, $false))
        Assert-True ($found.Count -eq 1 -and -not $found[0].HasPrivateKey) `
            'Public certificate cannot be read back from the isolated test store.'
        foreach ($item in $found) { $item.Dispose() }
    }
    finally { $imported.Dispose() }
    $passed++

    # Inject failures around the real PKI cmdlet to exercise cleanup deterministically.
    function New-SelfSignedCertificate {
        [CmdletBinding()]
        param($Subject, $CertStoreLocation, $KeyAlgorithm, $KeyLength, $HashAlgorithm,
            $KeyExportPolicy, $Provider, $Container, $Type, $KeyUsage, $TextExtension,
            $FriendlyName, $NotAfter)
        $faultState.KeyName = $Container
        if ($faultState.Mode -eq 'wrong-san') {
            $PSBoundParameters['TextExtension'] = @(
                '2.5.29.17={text}IPAddress=127.0.0.2',
                '2.5.29.37={text}1.3.6.1.5.5.7.3.1')
        }
        $generated = PKI\New-SelfSignedCertificate @PSBoundParameters
        $faultState.Certificate = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new($generated)
        if ($faultState.Mode -eq 'generation') {
            $generated.Dispose()
            throw 'Injected failure after certificate creation.'
        }
        if ($faultState.Mode -eq 'race') {
            [System.IO.File]::WriteAllText($faultState.Path, 'competing-writer')
        }
        return $generated
    }
    foreach ($failureMode in @('generation', 'wrong-san', 'race')) {
        $failurePath = Join-Path $testDirectory "$failureMode.cer"
        $faultState.Mode = $failureMode
        $faultState.Path = $failurePath
        Assert-Rejected { & $deploymentScript -ServerIp '127.0.0.1' -PublicCertificatePath $failurePath } `
            "Injected $failureMode failure was accepted."
        Assert-True ($null -ne $faultState.Certificate) "Fault injection did not create a real certificate: $lastRejection"
        Assert-True (-not (Test-Path -LiteralPath "Cert:\LocalMachine\My\$($faultState.Certificate.Thumbprint)")) `
            "Injected $failureMode failure left a machine certificate."
        Assert-True (-not [System.Security.Cryptography.CngKey]::Exists($faultState.KeyName,
                [System.Security.Cryptography.CngProvider]::MicrosoftSoftwareKeyStorageProvider,
                [System.Security.Cryptography.CngKeyOpenOptions]::MachineKey)) `
            "Injected $failureMode failure left a private key."
        Assert-True (Test-Path -LiteralPath "Cert:\LocalMachine\My\$thumbprint") `
            'Failure cleanup removed the earlier successful certificate.'
        if ($failureMode -eq 'race') {
            Assert-True ([System.IO.File]::ReadAllText($failurePath) -ceq 'competing-writer') `
                'Race-safe export overwrote the competing file.'
        }
        else {
            Assert-True (-not (Test-Path -LiteralPath $failurePath)) `
                "Injected $failureMode failure left an output."
        }
        $faultState.Certificate.Dispose()
        $faultState.Certificate = $null
        $faultState.KeyName = $null
        $passed++
    }

    # Replace only the I/O/handle operations under test, retaining real PKI and file creation.
    $tokens = $null
    $parseErrors = $null
    $deploymentAst = [System.Management.Automation.Language.Parser]::ParseFile(
        $deploymentScript, [ref]$tokens, [ref]$parseErrors)
    Assert-True ($parseErrors.Count -eq 0) 'Deployment script must parse before fault injection.'
    $replacements = @(
        @{ Match = { param($node)
                $node -is [System.Management.Automation.Language.InvokeMemberExpressionAst] -and
                $node.Static -and $node.Expression -is [System.Management.Automation.Language.TypeExpressionAst] -and
                $node.Expression.TypeName.FullName -eq 'System.IO.FileStream' -and $node.Member.Value -eq 'new'
            }; Text = '(New-TestOutputStream $outputPath)' },
        @{ Match = { param($node)
                $node -is [System.Management.Automation.Language.InvokeMemberExpressionAst] -and
                $node.Expression -is [System.Management.Automation.Language.VariableExpressionAst] -and
                $node.Expression.VariablePath.UserPath -eq 'outputStream' -and $node.Member.Value -eq 'Write'
            }; Text = 'Write-TestPartialExport $outputStream $publicDer' },
        @{ Match = { param($node)
                $node -is [System.Management.Automation.Language.InvokeMemberExpressionAst] -and
                $node.Expression -is [System.Management.Automation.Language.VariableExpressionAst] -and
                $node.Expression.VariablePath.UserPath -eq 'privateKey' -and $node.Member.Value -eq 'Dispose'
            }; Text = 'Close-TestPrivateKey $privateKey' }
    )
    $edits = foreach ($replacement in $replacements) {
        $nodes = @($deploymentAst.FindAll($replacement.Match, $true))
        Assert-True ($nodes.Count -eq 1) 'Fault injection must identify exactly one intended operation.'
        [pscustomobject]@{
            Start = $nodes[0].Extent.StartOffset
            Length = $nodes[0].Extent.EndOffset - $nodes[0].Extent.StartOffset
            Text = $replacement.Text
        }
    }
    $faultSource = $deploymentAst.Extent.Text
    foreach ($edit in ($edits | Sort-Object Start -Descending)) {
        $faultSource = $faultSource.Remove($edit.Start, $edit.Length).Insert($edit.Start, $edit.Text)
    }
    $faultDeployment = [scriptblock]::Create($faultSource)
    foreach ($cleanupFailure in @('stream-file-cleanup', 'certificate-cleanup', 'final-dispose')) {
        $faultState.Mode = $cleanupFailure
        $faultState.Path = Join-Path $testDirectory "$cleanupFailure.cer"
        $faultState.PrimaryError = [System.Management.Automation.ErrorRecord]::new(
            [System.IO.IOException]::new('Injected primary export failure.'),
            'KrAuth.Test.Export', [System.Management.Automation.ErrorCategory]::WriteError, $null)
        try {
            & {
                function New-TestOutputStream($Path) {
                    $faultState.Stream = [System.IO.FileStream]::new($Path, [System.IO.FileMode]::CreateNew,
                        [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
                    $stream = [pscustomobject]@{
                        Inner = $faultState.Stream
                        FailDispose = $faultState.Mode -eq 'stream-file-cleanup'
                    }
                    $stream | Add-Member ScriptMethod Write {
                        param($Bytes, $Offset, $Count)
                        $this.Inner.Write($Bytes, $Offset, $Count)
                    }
                    $stream | Add-Member ScriptMethod Dispose {
                        $this.Inner.Dispose()
                        if ($this.FailDispose) { throw 'Injected stream cleanup failure: secret-marker.' }
                    }
                    return $stream
                }
                function Write-TestPartialExport($Stream, $Bytes) {
                    $Stream.Write($Bytes, 0, [Math]::Min(16, $Bytes.Length))
                    throw $faultState.PrimaryError
                }
                function Close-TestPrivateKey($Key) {
                    $Key.Dispose()
                    if ($faultState.Mode -eq 'final-dispose') {
                        throw 'Injected handle cleanup failure: secret-marker.'
                    }
                }
                function Remove-Item {
                    [CmdletBinding()]
                    param([string]$LiteralPath, [switch]$Force)
                    if (($faultState.Mode -eq 'stream-file-cleanup' -and $LiteralPath -ceq $faultState.Path) -or
                        ($faultState.Mode -eq 'certificate-cleanup' -and
                            $LiteralPath -ceq "Cert:\LocalMachine\My\$($faultState.Certificate.Thumbprint)")) {
                        throw 'Injected removal cleanup failure: secret-marker.'
                    }
                    Microsoft.PowerShell.Management\Remove-Item @PSBoundParameters
                }
                $cleanupWarnings = @()
                $rejected = $false
                try {
                    & $faultDeployment -ServerIp '127.0.0.1' -PublicCertificatePath $faultState.Path `
                        -WarningVariable +cleanupWarnings -WarningAction Stop | Out-Null
                }
                catch { $rejected = $true; $lastRejection = $_ }
                Assert-True $rejected "Injected $cleanupFailure export failure was accepted."
                Assert-True ($lastRejection.Exception -eq $faultState.PrimaryError.Exception -and
                    $lastRejection.FullyQualifiedErrorId -like 'KrAuth.Test.Export*') `
                    "Secondary $cleanupFailure failure replaced the primary export error: $lastRejection"
                $warningText = $cleanupWarnings | Out-String
                Assert-True ($cleanupWarnings.Count -gt 0 -and $warningText -notmatch 'secret-marker' -and
                    $warningText -notmatch [regex]::Escape($faultState.Path) -and
                    $warningText -notmatch [regex]::Escape($faultState.KeyName)) `
                    'Secondary cleanup failures must be reported without exception details or resource identifiers.'
                Assert-True (-not [System.Security.Cryptography.CngKey]::Exists($faultState.KeyName,
                        [System.Security.Cryptography.CngProvider]::MicrosoftSoftwareKeyStorageProvider,
                        [System.Security.Cryptography.CngKeyOpenOptions]::MachineKey)) `
                    "Injected $cleanupFailure failure prevented invocation-owned key cleanup."
                $remainingCertificate = Test-Path -LiteralPath "Cert:\LocalMachine\My\$($faultState.Certificate.Thumbprint)"
                Assert-True ($remainingCertificate -eq ($faultState.Mode -eq 'certificate-cleanup')) `
                    "Injected $cleanupFailure failure did not independently attempt certificate cleanup."
                if ($faultState.Mode -eq 'stream-file-cleanup') {
                    Assert-True ((Get-Item -LiteralPath $faultState.Path).Length -gt 0) `
                        'Expected an owned partial output for the fixture to clean.'
                    Assert-True ($cleanupWarnings.Count -ge 2) `
                        'Both stream disposal and partial-file removal failures must be reported.'
                }
                else {
                    Assert-True (-not (Test-Path -LiteralPath $faultState.Path)) `
                        "Injected $cleanupFailure failure left a removable output."
                }
                Assert-True ((Test-Path -LiteralPath "Cert:\LocalMachine\My\$thumbprint") -and
                    [System.Security.Cryptography.CngKey]::Exists($privateKey.Key.KeyName,
                        [System.Security.Cryptography.CngProvider]::MicrosoftSoftwareKeyStorageProvider,
                        [System.Security.Cryptography.CngKeyOpenOptions]::MachineKey)) `
                    'Failure cleanup removed an unrelated successful certificate or key.'
                Assert-True ([System.IO.File]::ReadAllText($existingPath) -ceq 'keep-existing-public-certificate' -and
                    [Convert]::ToBase64String([System.IO.File]::ReadAllBytes($publicPath)) -ceq
                    [Convert]::ToBase64String($bytes)) 'Failure cleanup modified an unrelated output.'
            }
            $passed++
        }
        finally {
            $faultThumbprint = $faultState.Certificate.Thumbprint
            $faultKeyName = $faultState.KeyName
            Remove-FaultResources
            if (Test-Path -LiteralPath $faultState.Path) {
                Microsoft.PowerShell.Management\Remove-Item -LiteralPath $faultState.Path -Force
            }
            Assert-True (-not (Test-Path -LiteralPath "Cert:\LocalMachine\My\$faultThumbprint") -and
                -not [System.Security.Cryptography.CngKey]::Exists($faultKeyName,
                    [System.Security.Cryptography.CngProvider]::MicrosoftSoftwareKeyStorageProvider,
                    [System.Security.Cryptography.CngKeyOpenOptions]::MachineKey) -and
                -not (Test-Path -LiteralPath $faultState.Path)) `
                'Fault fixture did not remove its exact certificate, key, and partial output leftovers.'
        }
    }
    Remove-Item Function:\New-SelfSignedCertificate
    Write-Output "Certificate tests passed: $passed checks."
}
finally {
    Remove-FaultResources
    if ($privateKey) { $privateKey.Dispose() }
    if ($certificate) { Remove-TestCertificate $certificate }
    if ($publicCertificate) { $publicCertificate.Dispose() }
    $testStore.Dispose()
    # The unique custom store and artifact directory belong only to this invocation.
    # The certificate provider cannot delete CurrentUser stores; remove only this store's backing key.
    $storeRegistryPath = "HKCU:\Software\Microsoft\SystemCertificates\$storeName"
    if (Test-Path -LiteralPath $storeRegistryPath) { Remove-Item -LiteralPath $storeRegistryPath -Recurse }
    $resolvedTestDirectory = [System.IO.Path]::GetFullPath($testDirectory)
    if (-not $resolvedTestDirectory.StartsWith("$artifactRoot\", [System.StringComparison]::OrdinalIgnoreCase)) {
        throw 'Refusing cleanup outside the disposable certificate artifact directory.'
    }
    if (Test-Path -LiteralPath $resolvedTestDirectory) {
        Remove-Item -LiteralPath $resolvedTestDirectory -Recurse -Force
    }
}
