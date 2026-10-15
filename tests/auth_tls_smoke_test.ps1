param(
    [string]$ServerExe = "$PSScriptRoot/../Artifacts/Bin/x64/Debug/BubbleFighter.Server.exe",
    [string]$DatabaseConfig,
    [string]$Thumbprint,
    [string]$PublicCertificatePath,
    [switch]$TransportOnly,
    [string]$RegressionExe = "$PSScriptRoot/../Artifacts/Tests/x64/Debug/BubbleFighter.Regression.exe",
    [int]$AuthPort = 39000,
    [int]$GamePort = 39001
)
$ErrorActionPreference = 'Stop'
function Assert-Rejected([string[]]$Arguments, [string]$Expected = '') {
    $output = & $ServerExe @Arguments 2>&1 | Out-String
    if ($LASTEXITCODE -eq 0 -or $output -match 'Listening on') {
        throw 'Invalid auth configuration started listening.'
    }
    if ($output -match 'Invalid or duplicate argument: --auth-port') {
        throw 'Auth CLI is not implemented.'
    }
    if ($Expected -and $output -notmatch [regex]::Escape($Expected)) {
        throw "Expected rejection '$Expected'; got $output"
    }
}
Assert-Rejected @('--listen-port', "$GamePort", '--auth-port', "$AuthPort") 'Auth service requires'
Assert-Rejected @('--listen-port', "$GamePort", '--auth-port', "$GamePort") 'Auth service requires'
foreach ($badPort in @('0', '65536', '-1', '1junk')) {
    Assert-Rejected @('--listen-port', "$GamePort", '--auth-port', $badPort) 'Invalid auth port.'
}
Assert-Rejected @('--listen-port', "$GamePort", '--auth-cert-thumbprint', '00') 'Auth service requires'
Assert-Rejected @('--listen-port', "$GamePort", '--listen-address', '0.0.0.0') 'Wildcard listen requires'
foreach ($badAddress in @('0.0.0.0', 'localhost', '127.1', '1.2.3.256', '01.2.3.4')) {
    Assert-Rejected @('--listen-port', "$GamePort", '--advertise-address', $badAddress)
}
Assert-Rejected @('--listen-port', "$GamePort", '--auth-port', "$AuthPort", '--auth-cert-thumbprint', 'bad', '--database-config', 'missing-auth-test.ini') 'certificate'
Assert-Rejected @('--listen-port', "$GamePort", '--auth-port', "$AuthPort", '--auth-cert-thumbprint', ('0' * 40), '--database-config', 'missing-auth-test.ini') 'certificate'
if ($Thumbprint) {
    Assert-Rejected @('--listen-port', "$GamePort", '--auth-port', "$AuthPort", '--auth-cert-thumbprint', $Thumbprint, '--database-config', 'missing-auth-test.ini') 'Missing or oversized database configuration file.'
}
if ((!$DatabaseConfig -and !$TransportOnly) -or !$Thumbprint -or !$PublicCertificatePath) {
    Write-Output 'CLI rejection checks passed. TLS integration requires DatabaseConfig, Thumbprint and PublicCertificatePath.'
    exit 0
}
$tlsArguments = @(
    "$PSScriptRoot/auth_tls_integration_test.py",
    '--executable', $(if ($TransportOnly) { $RegressionExe } else { $ServerExe }),
    '--thumbprint', $Thumbprint, '--certificate', $PublicCertificatePath,
    '--port', "$AuthPort", '--game-port', "$GamePort"
)
if (!$TransportOnly) { $tlsArguments += @('--database-config', $DatabaseConfig) }
& python @tlsArguments
if ($LASTEXITCODE -ne 0) { throw 'TLS integration checks failed.' }
