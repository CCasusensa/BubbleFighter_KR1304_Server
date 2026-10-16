#Requires -Version 7.3
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$PackageRoot,
    [Parameter(Mandatory)][string]$AdminConfiguration,
    [Parameter(Mandatory)][string]$MySqlExecutable,
    [string]$Thumbprint
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$PackageRoot = (Resolve-Path -LiteralPath $PackageRoot).Path
$AdminConfiguration = (Resolve-Path -LiteralPath $AdminConfiguration).Path
$databaseName = 'KrStartupCheck_' + [guid]::NewGuid().ToString('N')
$temporaryRoot = Join-Path $PSScriptRoot "../Artifacts/Tests/$databaseName"
$null = New-Item -ItemType Directory -Path $temporaryRoot -Force
$databaseConfig = Join-Path $temporaryRoot 'database.ini'
$server = $null
function Invoke-TestSql([string]$Sql) {
    $info = [Diagnostics.ProcessStartInfo]::new($MySqlExecutable)
    foreach ($argument in @("--defaults-extra-file=$AdminConfiguration", '--batch', '--raw',
        '--skip-column-names', '--default-character-set=utf8mb4')) {
        $info.ArgumentList.Add($argument)
    }
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardInput = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.StandardInputEncoding = [Text.UTF8Encoding]::new($false)
    $process = [Diagnostics.Process]::Start($info)
    try {
        $output = $process.StandardOutput.ReadToEndAsync()
        $errors = $process.StandardError.ReadToEndAsync()
        $process.StandardInput.WriteLine($Sql)
        $process.StandardInput.Close()
        if (!$process.WaitForExit(30000)) { $process.Kill(); throw 'Test SQL timed out.' }
        if ($process.ExitCode -ne 0) { throw $errors.GetAwaiter().GetResult() }
        $output.GetAwaiter().GetResult().Trim()
    } finally { $process.Dispose() }
}
try {
    $install = [IO.File]::ReadAllText((Join-Path $PackageRoot 'database/install-new.sql'))
    if (!$install.Contains('CREATE DATABASE BubbleFighterServer ') -or !$install.Contains('USE BubbleFighterServer;')) {
        throw 'Unexpected install SQL.'
    }
    $null = Invoke-TestSql ($install.Replace('CREATE DATABASE BubbleFighterServer ', "CREATE DATABASE $databaseName ").
        Replace('USE BubbleFighterServer;', "USE $databaseName;"))
    $credentials = [IO.File]::ReadAllText($AdminConfiguration)
    if ($credentials -match '(?m)^\s*database\s*=') { throw 'Use an admin config without a database entry.' }
    [IO.File]::WriteAllText($databaseConfig, $credentials + "`ndatabase=$databaseName`n", [Text.UTF8Encoding]::new($false))
    $credentials = $null
    $reservation = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
    $reservation.Start()
    $gmPort = $reservation.LocalEndpoint.Port
    $reservation.Stop()
    $operations = Get-Content -LiteralPath (Join-Path $PackageRoot 'config/operations.json') -Raw | ConvertFrom-Json
    $operations.gm_address = '127.0.0.1'
    $operations.gm_port = $gmPort
    $operations.web_root = Join-Path $PackageRoot 'webgm'
    $operationsConfig = Join-Path $temporaryRoot 'operations.json'
    [IO.File]::WriteAllText($operationsConfig, ($operations | ConvertTo-Json), [Text.UTF8Encoding]::new($false))
    $info = [Diagnostics.ProcessStartInfo]::new((Join-Path $PackageRoot 'BubbleFighter.Server.exe'))
    $info.WorkingDirectory = $PackageRoot
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    foreach ($argument in @('--listen-port', '0', '--clan-port', '0', '--database-config', $databaseConfig,
        '--operations-config', $operationsConfig, '--shop-table', (Join-Path $PackageRoot 'data/shop'))) {
        $info.ArgumentList.Add($argument)
    }
    if ($Thumbprint) {
        $reservation = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
        $reservation.Start()
        $authPort = $reservation.LocalEndpoint.Port
        $reservation.Stop()
        foreach ($argument in @('--auth-port', [string]$authPort, '--auth-cert-thumbprint', $Thumbprint)) {
            $info.ArgumentList.Add($argument)
        }
    }
    $server = [Diagnostics.Process]::Start($info)
    $stdout = $server.StandardOutput.ReadToEndAsync()
    $stderr = $server.StandardError.ReadToEndAsync()
    $ready = $false
    for ($attempt = 0; $attempt -lt 60; $attempt++) {
        if ($server.HasExited) { throw "Fresh database startup failed: $($stderr.GetAwaiter().GetResult())" }
        $listeners = @(Get-NetTCPConnection -State Listen -OwningProcess $server.Id -ErrorAction SilentlyContinue)
        if ($listeners.Count -eq $(if ($Thumbprint) { 4 } else { 3 })) { $ready = $true; break }
        Start-Sleep -Milliseconds 200
    }
    if (!$ready) { throw 'Fresh database startup timed out.' }
    Write-Output "Fresh packaged database startup passed: $($listeners.Count) listeners (PID $($server.Id))."
    $server.Kill()
    $server.WaitForExit()
    $server.Dispose()
    $server = $null
    foreach ($scenario in @(
        @{ Sql = "ALTER TABLE $databaseName.Clans ALTER CHECK CK_Clans_RecruitmentMode NOT ENFORCED";
           Undo = "ALTER TABLE $databaseName.Clans ALTER CHECK CK_Clans_RecruitmentMode ENFORCED";
           Stage = 'clan schema and service lock'; Detail = 'Clan recruitment migration is required.' },
        @{ Sql = "ALTER TABLE $databaseName.ClanMembers ALTER CHECK CK_ClanMembers_TrainingRole NOT ENFORCED";
           Undo = "ALTER TABLE $databaseName.ClanMembers ALTER CHECK CK_ClanMembers_TrainingRole ENFORCED";
           Stage = 'clan schema and service lock'; Detail = 'Training clan migration is required.' },
        @{ Sql = "ALTER TABLE $databaseName.Clans DROP CHECK CK_Clans_RecruitmentMode";
           Undo = "ALTER TABLE $databaseName.Clans ADD CONSTRAINT CK_Clans_RecruitmentMode CHECK (IsRecruiting IN (0, 1, 2))";
           Stage = 'clan schema and service lock'; Detail = 'Clan recruitment migration is required.' },
        @{ Sql = "ALTER TABLE $databaseName.ClanMembers DROP CHECK CK_ClanMembers_TrainingRole";
           Undo = "ALTER TABLE $databaseName.ClanMembers ADD CONSTRAINT CK_ClanMembers_TrainingRole CHECK (ClanId <> 99 OR MembershipStatus = 100)";
           Stage = 'clan schema and service lock'; Detail = 'Training clan migration is required.' },
        @{ Sql = "ALTER TABLE $databaseName.Characters DROP CHECK CK_Characters_BattleRecords; ALTER TABLE $databaseName.Characters RENAME COLUMN BattleRecords TO TestMissingBattleRecords";
           Undo = "ALTER TABLE $databaseName.Characters RENAME COLUMN TestMissingBattleRecords TO BattleRecords; ALTER TABLE $databaseName.Characters ADD CONSTRAINT CK_Characters_BattleRecords CHECK (OCTET_LENGTH(BattleRecords) = 176 AND SUBSTRING(BattleRecords, 1, 4) = 0x01000000)";
           Stage = 'inventory schema'; Detail = 'Missing column: BattleRecords' },
        @{ Sql = "ALTER TABLE $databaseName.OperationSettings RENAME COLUMN Document TO TestMissingDocument";
           Undo = "ALTER TABLE $databaseName.OperationSettings RENAME COLUMN TestMissingDocument TO Document";
           Stage = 'operations configuration and schema'; Detail = 'Missing column: Document' }
    )) {
        $null = Invoke-TestSql $scenario.Sql
        $server = [Diagnostics.Process]::Start($info)
        $stdout = $server.StandardOutput.ReadToEndAsync()
        $stderr = $server.StandardError.ReadToEndAsync()
        if (!$server.WaitForExit(15000)) { throw 'Invalid-schema startup did not stop.' }
        $detail = $stderr.GetAwaiter().GetResult()
        if ($server.ExitCode -eq 0 -or !$detail.Contains("Server error [$($scenario.Stage)]") -or
            !$detail.Contains($scenario.Detail)) {
            throw "Invalid-schema diagnostic failed: $detail"
        }
        $server.Dispose()
        $server = $null
        $null = Invoke-TestSql $scenario.Undo
    }
    Write-Output 'Missing/disabled clan CHECK constraints and missing inventory/operations columns are rejected with the expected startup diagnostics.'
} finally {
    if ($server) {
        if (!$server.HasExited) { $server.Kill(); $server.WaitForExit() }
        $server.Dispose()
    }
    if (Test-Path -LiteralPath $databaseConfig) { Remove-Item -LiteralPath $databaseConfig }
    if (Test-Path -LiteralPath (Join-Path $temporaryRoot 'operations.json')) {
        Remove-Item -LiteralPath (Join-Path $temporaryRoot 'operations.json')
    }
    $null = Invoke-TestSql "DROP DATABASE IF EXISTS $databaseName;"
}
