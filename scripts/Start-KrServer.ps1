[CmdletBinding()]
param([switch]$Stop)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$packageRoot = $PSScriptRoot
$executable = Join-Path $packageRoot 'BubbleFighter.Server.exe'
if (!(Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw 'Run Start-Server.ps1 from the packaged server directory.'
}
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ('"' + $PSCommandPath + '"'))
    if ($Stop) { $arguments += '-Stop' }
    $helper = Start-Process powershell.exe -Verb RunAs -WindowStyle Hidden -PassThru -ArgumentList $arguments
    $helper.WaitForExit()
    if ($helper.ExitCode -ne 0) { throw 'Server action failed; see logs/startup-error.log.' }
    exit 0
}
$logs = Join-Path $packageRoot 'logs'
New-Item -ItemType Directory -Path $logs -Force | Out-Null
try {
    $pidFile = Join-Path $logs 'server.pid'
    if (Test-Path -LiteralPath $pidFile) {
        $serverProcessId = [int]([IO.File]::ReadAllText($pidFile))
        $existing = Get-Process -Id $serverProcessId -ErrorAction SilentlyContinue
        if ($existing) {
            if (![string]::Equals($existing.Path, $executable, [StringComparison]::OrdinalIgnoreCase)) {
                throw 'Saved PID belongs to another executable.'
            }
            if (!$Stop) { throw 'This packaged server is already running.' }
            Stop-Process -InputObject $existing
            if (!$existing.WaitForExit(10000)) { throw 'Server did not stop.' }
        }
    }
    if ($Stop) { exit 0 }
    $configuration = Join-Path $packageRoot 'config'
    $settings = Get-Content -LiteralPath (Join-Path $configuration 'server.json') -Raw | ConvertFrom-Json
    $operations = Get-Content -LiteralPath (Join-Path $configuration 'operations.json') -Raw | ConvertFrom-Json
    $listenAddress = if ($settings.PSObject.Properties.Name -contains 'listen_address') {
        [string]$settings.listen_address
    } else { [string]$settings.host }
    foreach ($addressText in @([string]$settings.host, $listenAddress, [string]$operations.gm_address)) {
        $parsedAddress = $null
        if (![Net.IPAddress]::TryParse($addressText, [ref]$parsedAddress) -or
            $parsedAddress.AddressFamily -ne [Net.Sockets.AddressFamily]::InterNetwork -or
            $parsedAddress.ToString() -cne $addressText) {
            throw 'Server, listen and GM addresses must be canonical IPv4 addresses.'
        }
    }
    $firstOctet = [int](([string]$settings.host).Split('.')[0])
    if ($firstOctet -eq 0 -or $firstOctet -ge 224) {
        throw 'The advertised host must be a fixed unicast IPv4 address.'
    }
    $ports = @($settings.game_port, $settings.auth_port, $settings.clan_port, $operations.gm_port)
    if (@($ports | Select-Object -Unique).Count -ne 4) { throw 'Server ports must be distinct.' }
    foreach ($port in $ports) {
        if (($port -isnot [int] -and $port -isnot [long]) -or $port -lt 1 -or $port -gt 65535) {
            throw 'Invalid server port: use a JSON integer between 1 and 65535.'
        }
        if (Get-NetTCPConnection -State Listen -LocalPort $port -ErrorAction SilentlyContinue) {
            throw "Port $port is already in use; stop the existing server first."
        }
    }
    $publicCertificate = [Security.Cryptography.X509Certificates.X509Certificate2]::new(
        [IO.File]::ReadAllBytes((Join-Path $configuration 'auth-server.cer')))
    try {
        $thumbprint = $settings.auth_cert_thumbprint
        if ($thumbprint -notmatch '^[A-Fa-f0-9]{40}$' -or $publicCertificate.Thumbprint -ine $thumbprint) {
            throw 'Public certificate does not match the configured thumbprint.'
        }
        $machineCertificate = Get-Item -LiteralPath "Cert:\LocalMachine\My\$thumbprint"
        if (!$machineCertificate.HasPrivateKey) { throw 'Machine certificate private key is unavailable.' }
        if ($machineCertificate.NotAfter -le (Get-Date)) { throw 'Machine certificate has expired.' }
    } finally { $publicCertificate.Dispose() }
    $arguments = @(
        '--listen-address', $listenAddress, '--advertise-address', [string]$settings.host,
        '--listen-port', [string]$settings.game_port,
        '--auth-port', [string]$settings.auth_port,
        '--auth-cert-thumbprint', $thumbprint,
        '--clan-port', [string]$settings.clan_port,
        '--database-config', ('"' + (Join-Path $configuration 'database.ini') + '"'),
        '--operations-config', ('"' + (Join-Path $configuration 'operations.json') + '"'),
        '--launcher-policy', ('"' + (Join-Path $configuration 'launcher_policy.ini') + '"'),
        '--channel-rules', ('"' + (Join-Path $configuration 'channel_rules.ini') + '"'),
        '--match-rules', ('"' + (Join-Path $configuration 'match_rules.ini') + '"'),
        '--shop-table', ('"' + (Join-Path $packageRoot 'data\shop') + '"')
    )
    $server = Start-Process -FilePath $executable -WorkingDirectory $packageRoot `
        -ArgumentList $arguments -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput (Join-Path $logs 'server.stdout.log') `
        -RedirectStandardError (Join-Path $logs 'server.stderr.log')
    [IO.File]::WriteAllText($pidFile, [string]$server.Id)
    for ($attempt = 0; $attempt -lt 30; $attempt++) {
        if ($server.HasExited) {
            $detail = (Get-Content -LiteralPath (Join-Path $logs 'server.stderr.log') -Tail 12) -join [Environment]::NewLine
            throw "Server exited; see logs/server.stderr.log.$([Environment]::NewLine)$detail"
        }
        $listeningPorts = @(Get-NetTCPConnection -State Listen -OwningProcess $server.Id -ErrorAction SilentlyContinue |
            Select-Object -ExpandProperty LocalPort)
        if (@($ports | Where-Object { $_ -notin $listeningPorts }).Count -eq 0) {
            Write-Output "Server PID $($server.Id) is ready; listen=$listenAddress, advertised=$($settings.host)."
            exit 0
        }
        Start-Sleep -Milliseconds 300
    }
    Stop-Process -InputObject $server
    throw 'Server startup timed out; see logs/server.stderr.log.'
} catch {
    [IO.File]::WriteAllText((Join-Path $logs 'startup-error.log'), $_.ToString())
    Write-Error $_
    exit 1
}
