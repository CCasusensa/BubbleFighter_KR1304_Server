#Requires -Version 7.3
[CmdletBinding()]
param(
    [string]$ServerExe,
    [string]$CertificatePath,
    [string]$DatabaseConfig,
    [string]$GameClientPath,
    [int]$GamePort,
    [int]$AuthPort,
    [int]$ClanPort,
    [string]$FixedHost,
    [string]$LauncherRoot,
    [switch]$IsolatedClientCopy,
    [pscredential]$TestAccount
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'launcher_auth_owned.ps1')
if (-not ('KrAuthE2eCapture' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Concurrent;
using System.Diagnostics;
public sealed class KrAuthE2eLine {
    public long At;
    public string Text;
    public KrAuthE2eLine(string text) { At=DateTimeOffset.UtcNow.ToUnixTimeMilliseconds(); Text=text; }
}
public sealed class KrAuthE2eCapture {
    public readonly ConcurrentQueue<KrAuthE2eLine> Output = new ConcurrentQueue<KrAuthE2eLine>();
    public readonly ConcurrentQueue<KrAuthE2eLine> Error = new ConcurrentQueue<KrAuthE2eLine>();
    public KrAuthE2eCapture(Process process) {
        process.OutputDataReceived += (sender, e) => { if(e.Data!=null) Output.Enqueue(new KrAuthE2eLine(e.Data)); };
        process.ErrorDataReceived += (sender, e) => { if(e.Data!=null) Error.Enqueue(new KrAuthE2eLine(e.Data)); };
        process.BeginOutputReadLine();
        process.BeginErrorReadLine();
    }
}
'@
}
if (-not $ServerExe -or -not $CertificatePath -or -not $DatabaseConfig -or -not $GameClientPath -or
    -not $GamePort -or -not $AuthPort -or -not $ClanPort -or -not $FixedHost -or -not $LauncherRoot -or -not $IsolatedClientCopy) {
    throw 'Missing explicit e2e inputs: ServerExe, CertificatePath, DatabaseConfig, GameClientPath, GamePort, AuthPort, ClanPort, FixedHost, LauncherRoot and IsolatedClientCopy are required.'
}
$address = $null
if (-not [Net.IPAddress]::TryParse($FixedHost, [ref]$address) -or
    $address.AddressFamily -ne [Net.Sockets.AddressFamily]::InterNetwork -or $address.ToString() -cne $FixedHost -or
    $address.GetAddressBytes()[0] -eq 0 -or $address.GetAddressBytes()[0] -ge 224) { throw 'FixedHost must be a canonical unicast IPv4.' }
$ports = @($GamePort, $AuthPort, $ClanPort)
if (@($ports | Select-Object -Unique).Count -ne 3 -or @($ports | Where-Object { $_ -lt 1 -or $_ -gt 65535 }).Count) { throw 'E2e ports must be valid and distinct.' }
foreach ($file in @($ServerExe, $CertificatePath, $DatabaseConfig, $GameClientPath,
    (Join-Path $LauncherRoot 'node_modules/electron/dist/electron.exe'),
    (Join-Path $LauncherRoot 'runtime/kr-passport.js'))) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw 'An explicitly supplied e2e input file is missing.' }
}
$client = (Resolve-Path -LiteralPath $GameClientPath).Path
$forbidden = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../BubbleFighter')) + [IO.Path]::DirectorySeparatorChar
if ($client.StartsWith($forbidden, [StringComparison]::OrdinalIgnoreCase) -or
    [IO.Path]::GetFileName($client) -cne 'BubbleFighterU.exe' -or
    (Get-FileHash -LiteralPath $client -Algorithm SHA256).Hash -cne '33D2E2A9D7E3DA875A270C1A1973010ABA798A0FB59B89189C271854F704A529') {
    throw 'Only the exact supported target in an explicitly isolated client copy is permitted.'
}
$certificate = [Security.Cryptography.X509Certificates.X509Certificate2]::new([IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $CertificatePath).Path))
if ([Convert]::ToBase64String($certificate.RawData) -cne [Convert]::ToBase64String([IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $CertificatePath).Path)) -or
    [DateTime]::Now -lt $certificate.NotBefore -or [DateTime]::Now -ge $certificate.NotAfter) { throw 'Certificate must be current public DER.' }
$san = @($certificate.Extensions | Where-Object { $_.Oid.Value -eq '2.5.29.17' })
$typedSan = [Security.Cryptography.X509Certificates.X509SubjectAlternativeNameExtension]::new($san[0].RawData, $san[0].Critical)
if (@($typedSan.EnumerateIPAddresses() | Where-Object { $_.ToString() -ceq $FixedHost }).Count -ne 1) { throw 'Certificate IP SAN must match FixedHost.' }
$machine = Get-Item -LiteralPath "Cert:\LocalMachine\My\$($certificate.Thumbprint)"
if (-not $machine.HasPrivateKey) { throw 'Server certificate private key is not available in LocalMachine/My.' }
foreach ($port in $ports) {
    $listener = [Net.Sockets.TcpListener]::new($address, $port)
    try { $listener.Start() } finally { $listener.Stop() }
}
$serverRoot = Split-Path $PSScriptRoot -Parent
$artifactRoot = Join-Path $serverRoot 'Artifacts/Tests/launcher-e2e'
$artifact = Join-Path $artifactRoot ([guid]::NewGuid().ToString('N'))
$server = $null
$electron = $null
$pipe = $null
$writer = $null
$ownedClients = [Collections.Generic.Dictionary[int,object]]::new()
$electronCapture = $null
$serverCapture = $null
$wrong = $null
$key = $null
$ini = Join-Path (Split-Path $client -Parent) 'login.ini'
$backup = "$ini.bak"
$original = if (Test-Path -LiteralPath $ini) { [IO.File]::ReadAllBytes($ini) } else { $null }
$hadBackup = Test-Path -LiteralPath $backup
$parentPhase = 'setup'
New-Item -ItemType Directory -Path $artifact -Force | Out-Null
try {
    $wrongPath = Join-Path $artifact 'wrong.cer'
    $thumbprint = & (Join-Path $serverRoot 'scripts/New-KrAuthCertificate.ps1') -ServerIp $FixedHost -PublicCertificatePath $wrongPath
    $wrong = Get-Item -LiteralPath "Cert:\LocalMachine\My\$thumbprint"
    $key = [Security.Cryptography.X509Certificates.RSACertificateExtensions]::GetRSAPrivateKey($wrong)
    $start = [Diagnostics.ProcessStartInfo]::new((Resolve-Path -LiteralPath $ServerExe).Path)
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    foreach ($argument in @('--listen-address', $FixedHost, '--advertise-address', $FixedHost,
        '--listen-port', "$GamePort", '--auth-port', "$AuthPort", '--auth-cert-thumbprint', $certificate.Thumbprint,
        '--database-config', (Resolve-Path -LiteralPath $DatabaseConfig).Path, '--idle-timeout-seconds', '45')) { $start.ArgumentList.Add($argument) }
    $server = [Diagnostics.Process]::Start($start)
    $serverCapture = [KrAuthE2eCapture]::new($server)
    $ready = $false
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    do {
        if ($server.HasExited) { throw 'DB server exited before readiness; DB/game checks failed.' }
        $probe = [Net.Sockets.TcpClient]::new()
        try { if ($probe.ConnectAsync($FixedHost, $AuthPort).Wait(100)) { $ready = $true; break } }
        catch {} finally { $probe.Dispose() }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    if (-not $ready) { throw 'DB server readiness timeout; DB/game checks failed.' }
    Start-Sleep -Milliseconds 200
    $start = [Diagnostics.ProcessStartInfo]::new((Join-Path $LauncherRoot 'node_modules/electron/dist/electron.exe'))
    $start.ArgumentList.Add((Join-Path $PSScriptRoot 'launcher_auth_e2e.cjs'))
    $pipeName = 'kr1304-e2e-' + [guid]::NewGuid().ToString('N')
    $pipe = [IO.Pipes.NamedPipeServerStream]::new($pipeName, [IO.Pipes.PipeDirection]::InOut, 1,
        [IO.Pipes.PipeTransmissionMode]::Byte, [IO.Pipes.PipeOptions]::Asynchronous -bor [IO.Pipes.PipeOptions]::CurrentUserOnly)
    $connected = $pipe.WaitForConnectionAsync()
    $start.ArgumentList.Add("\\.\pipe\$pipeName")
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.Environment.Remove('ELECTRON_RUN_AS_NODE') | Out-Null
    $start.Environment.Remove('NODE_OPTIONS') | Out-Null
    $electron = [Diagnostics.Process]::Start($start)
    $electronCapture = [KrAuthE2eCapture]::new($electron)
    $parentPhase = 'controlled-input'
    if (-not $connected.Wait(10000)) { throw 'Owned Electron controlled input connection timed out.' }
    # Accounts are 6-18 characters and passwords 6-18 UTF-8 bytes (2026-10-15).
    $username = if ($TestAccount) { $TestAccount.UserName } else { 'e2e_' + [Convert]::ToHexString([Security.Cryptography.RandomNumberGenerator]::GetBytes(6)).ToLowerInvariant() }
    $password = if ($TestAccount) { $TestAccount.GetNetworkCredential().Password } else { [Convert]::ToHexString([Security.Cryptography.RandomNumberGenerator]::GetBytes(8)) }
    $inputJson = @{ launcherRoot = $LauncherRoot; host = $FixedHost; authPort = $AuthPort; gamePort = $GamePort; clanPort = $ClanPort;
        certificate = (Resolve-Path -LiteralPath $CertificatePath).Path; wrongCertificate = $wrongPath; gameClient = $client;
        username = $username; password = $password } | ConvertTo-Json -Compress
    $writer = [IO.StreamWriter]::new($pipe, [Text.UTF8Encoding]::new($false), 1024, $true)
    $writer.WriteLine($inputJson)
    $writer.Flush()
    $inputJson = $null
    $password = $null
    $deadline = [DateTime]::UtcNow.AddSeconds(70)
    $parentPhase = 'supervision'
    do {
        Receive-KrOwnedSpawns -Capture $electronCapture -OwnedClients $ownedClients -Acknowledge {
            param($OwnedPid)
            $writer.WriteLine((@{command='owned';pid=$OwnedPid}|ConvertTo-Json -Compress))
            $writer.Flush()
        }
        if ($electron.WaitForExit(50)) { break }
    } while ([DateTime]::UtcNow -lt $deadline)
    $timedOut = -not $electron.HasExited
    $parentPhase = 'final-drain'
    Stop-KrOwnedElectron -Child $electron -Capture $electronCapture -OwnedClients $ownedClients -Cancel {
        if ($writer) { $writer.WriteLine('{"command":"cancel"}'); $writer.Flush() }
    }
    foreach ($record in $electronCapture.Output.ToArray()) {
        if ($record.Text -match '^\{"event":"e2e-(auth-passed|client-proof|owned-cleanup|phase|injection-failed|launch-failed|diagnostic)"') { Write-Output $record.Text }
    }
    $nativeError = ($electronCapture.Error.ToArray() | ForEach-Object Text) -join "`n"
    Write-Output "Electron stderr bytes: $([Text.Encoding]::UTF8.GetByteCount($nativeError))"
    if ($timedOut -or $electron.ExitCode -ne 0) {
        foreach ($line in $nativeError -split '\r?\n') { if ($line -match '^\{"event":"e2e-failed","phase":"[a-z-]+"\}$') { Write-Output $line } }
        throw 'Actual production auth/launch failed; game gate did not pass.'
    }
    $server.Kill()
    $server.WaitForExit(10000) | Out-Null
    $server.WaitForExit()
    $transcript = ($serverCapture.Output.ToArray() | ForEach-Object Text) -join "`n"
    $serverError = ($serverCapture.Error.ToArray() | ForEach-Object Text) -join "`n"
    $firstCleanup = @($electronCapture.Output.ToArray() | Where-Object { $_.Text -match '^\{"event":"e2e-cleanup-begin","attempt":"first",' })
    if ($firstCleanup.Count -ne 1) { throw 'First production cleanup boundary was not observed.' }
    $cleanupAt = ($firstCleanup[0].Text | ConvertFrom-Json).at
    $errorsBeforeCleanup = @($serverCapture.Error.ToArray() | Where-Object { $_.At -lt $cleanupAt }).Count
    Write-Output "Server error lines before first owned cleanup: $errorsBeforeCleanup"
    if ($errorsBeforeCleanup -ne 0) { throw 'Server rejected/disconnected before initial owned cleanup.' }
    $loginCount = [regex]::Matches($transcript, 'ClientToServer MessageId=3 PayloadBytes=298').Count
    $accepted = [regex]::Matches($transcript, 'ServerToClient MessageId=12 PayloadBytes=0').Count
    $rejected = [regex]::Matches($serverError, '(?m)^Connection rejected: Local ticket rejected\.\r?$').Count
    $reset = [regex]::Matches($serverError, '(?m)^Connection rejected: recv failed: Winsock 10054\r?$').Count
    Write-Output "Protocol metadata: login298=$loginCount createCharacter=$accepted ticketRejected=$rejected ownedCleanupReset=$reset stderrBytes=$([Text.Encoding]::UTF8.GetByteCount($serverError))"
    if ($loginCount -ne 2 -or $accepted -ne 1 -or $rejected -ne 1) { throw 'Game acceptance or one-use ticket rejection was not proven.' }
    Write-Output 'PASS: actual KrAuthClient TLS/register/login, production launchGame/injector/runtime, C3/S12 and consumed-ticket reuse rejection. No diagnostic hook.'
}
catch {
    Write-Output "Owned e2e parent failed at: $parentPhase; exception=$($_.Exception.GetType().Name)"
    if ($electronCapture) {
        foreach ($record in $electronCapture.Error.ToArray()) {
            if ($record.Text -match '^\{"event":"e2e-failed","phase":"[a-z-]+"\}$') { Write-Output $record.Text }
        }
    }
    if ($serverCapture) {
        $safeTranscript = ($serverCapture.Output.ToArray() | ForEach-Object Text) -join "`n"
        $safeErrors = ($serverCapture.Error.ToArray() | ForEach-Object Text) -join "`n"
        $firstMessage = [regex]::Matches($safeTranscript, 'ClientToServer MessageId=1 PayloadBytes=').Count
        $loginMessage = [regex]::Matches($safeTranscript, 'ClientToServer MessageId=3 PayloadBytes=298').Count
        $acceptedMessage = [regex]::Matches($safeTranscript, 'ServerToClient MessageId=12 PayloadBytes=0').Count
        Write-Output "Failure protocol metadata: C1=$firstMessage C3payload298=$loginMessage S12=$acceptedMessage stderrBytes=$([Text.Encoding]::UTF8.GetByteCount($safeErrors))"
    }
    throw
}
finally {
    $cleanupErrors = [Collections.Generic.List[string]]::new()
    function Clean-Owned([scriptblock]$Action, [string]$Name) {
        try { & $Action } catch { $cleanupErrors.Add($Name); Write-Warning "Owned e2e cleanup failed: $Name" }
    }
    Clean-Owned {
        Stop-KrOwnedElectron -Child $electron -Capture $electronCapture -OwnedClients $ownedClients -Cancel {
            if ($writer) { $writer.WriteLine('{"command":"cancel"}'); $writer.Flush() }
        }
    } 'Electron stop and final ownership drain'
    foreach ($child in @($server)) {
        if ($child) { Clean-Owned { if (-not $child.HasExited) { $child.Kill(); if (-not $child.WaitForExit(10000)) { throw 'Timeout' } } } 'child process' }
    }
    Clean-Owned { Close-KrOwnedClients -OwnedClients $ownedClients } 'spawn-owned clients'
    Clean-Owned {
        if ($null -ne $original) { [IO.File]::WriteAllBytes($ini, $original) }
        elseif (Test-Path -LiteralPath $ini) { Remove-Item -LiteralPath $ini -Force }
    } 'isolated login.ini restore'
    Clean-Owned { if (-not $hadBackup -and (Test-Path -LiteralPath $backup)) { Remove-Item -LiteralPath $backup -Force } } 'isolated login.ini backup'
    if ($writer) { Clean-Owned {
        try { $writer.Dispose() }
        catch [IO.IOException] { if (-not $electron -or -not $electron.HasExited) { throw } }
    } 'controlled input writer' }
    if ($pipe) { Clean-Owned { $pipe.Dispose() } 'controlled input pipe' }
    if ($wrong) { Clean-Owned { Remove-Item -LiteralPath "Cert:\LocalMachine\My\$($wrong.Thumbprint)" -ErrorAction Stop } 'wrong certificate' }
    if ($key) {
        Clean-Owned { $key.Key.Delete() } 'wrong certificate key'
        Clean-Owned { $key.Dispose() } 'wrong certificate key handle'
    }
    foreach ($resource in @($wrong, $machine, $certificate, $electron, $server)) {
        if ($resource) { Clean-Owned { $resource.Dispose() } 'resource handle' }
    }
    Clean-Owned {
        if ([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($artifact)) -cne [IO.Path]::GetFullPath($artifactRoot)) { throw 'Invalid owned artifact cleanup target.' }
        Remove-Item -LiteralPath $artifact -Recurse -Force
    } 'artifact directory'
    if ($cleanupErrors.Count) { throw 'One or more owned e2e resources could not be cleaned; acceptance failed.' }
}
