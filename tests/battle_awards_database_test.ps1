[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug',
    [string]$PythonVersion = '3',
    [Parameter(Mandatory)][string]$MySqlRoot,
    [Parameter(Mandatory)][string]$AdminConfiguration,
    [Parameter(Mandatory)][ValidateRange(1, 65535)][int]$Port
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$mysql = Join-Path (Resolve-Path -LiteralPath $MySqlRoot).Path 'bin/mysql.exe'
$adminFile = (Resolve-Path -LiteralPath $AdminConfiguration).Path
$testExe = Join-Path $repositoryRoot "Artifacts/Tests/x64/$Configuration/BubbleFighter.Regression.exe"
$serverExe = Join-Path $repositoryRoot "Artifacts/Bin/x64/$Configuration/BubbleFighter.Server.exe"
if (-not (Test-Path -LiteralPath $testExe) -or -not (Test-Path -LiteralPath $serverExe)) { throw 'Build first.' }
$suffix = [Guid]::NewGuid().ToString('N').Substring(0, 12)
$database = 'BfAwardsTests_' + $suffix
$testUser = 'bf_awards_' + $suffix
if ($database -cnotmatch '^BfAwardsTests_[0-9a-f]{12}$' -or $testUser -cnotmatch '^bf_awards_[0-9a-f]{12}$') { throw 'Invalid generated scope.' }
$privateDirectory = Join-Path $repositoryRoot "Artifacts/database_tests/$suffix"
New-Item -ItemType Directory -Path $privateDirectory | Out-Null
$owner = [Security.Principal.WindowsIdentity]::GetCurrent().User
$acl = [Security.AccessControl.DirectorySecurity]::new()
$acl.SetOwner($owner)
$acl.SetAccessRuleProtection($true, $false)
foreach ($sid in @($owner.Value, 'S-1-5-18', 'S-1-5-32-544')) {
    $identity = [Security.Principal.SecurityIdentifier]::new($sid)
    $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($identity, 'FullControl', 'ContainerInherit,ObjectInherit', 'None', 'Allow'))
}
Set-Acl -LiteralPath $privateDirectory -AclObject $acl
$appConfig = Join-Path $privateDirectory 'application.cnf'
$ticketFile = Join-Path $privateDirectory 'login.ticket'
$networkTickets = @('host.ticket','guest.ticket','observer.ticket') | ForEach-Object { Join-Path $privateDirectory $_ }
$logDirectory = Join-Path $repositoryRoot 'Artifacts/Logs'
New-Item -ItemType Directory -Path $logDirectory -Force | Out-Null
$logPath = Join-Path $logDirectory ("awards_" + $Configuration + "_" + $suffix + ".log")
$reportPath = Join-Path $logDirectory ("awards_" + $Configuration + "_" + $suffix + ".json")
$utf8 = [Text.UTF8Encoding]::new($false)
$lineBreak = [Environment]::NewLine
$secretBytes = [byte[]]::new(32)
[Security.Cryptography.RandomNumberGenerator]::Fill($secretBytes)
$secret = [Convert]::ToBase64String($secretBytes)
$createdDatabase = $false
$createdUser = $false
$passed = $false
$cleaned = $false
$cliChecks = 0
$groupMigrationPreserved = $false
$afterGroups = @()
$afterMailbox = @()
$transcript = [Collections.Generic.List[string]]::new()
function Admin([string]$Sql, [switch]$ExpectDuplicateColumn, [switch]$ExpectExistingTable) {
    # Never echo SQL/raw errors: CREATE USER contains a secret.
    $result = $Sql | & $mysql "--defaults-file=$adminFile" --no-login-paths --batch --skip-column-names 2>&1
    $code = $LASTEXITCODE
    if ($ExpectExistingTable) {
        if ($code -eq 0 -or ($result -join ' ') -notmatch 'ERROR 1050') { throw 'Existing table migration did not reject.' }
        return
    }
    if ($ExpectDuplicateColumn) {
        # MySQL may detect the duplicate CHECK name before the duplicate column.
        if ($code -eq 0 -or ($result -join ' ') -notmatch 'ERROR (1060|3822)') {
            $number = [regex]::Match(($result -join ' '), 'ERROR [0-9]+').Value
            throw "Duplicate migration did not reject an existing column/constraint ($number)."
        }
        return
    }
    if ($code -ne 0) { throw 'Isolated database administration failed.' }
    return $result
}
function RunTest([string]$Action) {
    $result = & $testExe $Action $appConfig 2>&1
    $code = $LASTEXITCODE
    $text = ($result -join $lineBreak) + $lineBreak
    $transcript.Add($text.Trim())
    [IO.File]::AppendAllText($logPath, $text, $utf8)
    if ($code -ne 0) { throw "Test failed: $Action; see $logPath" }
    Write-Output $text.Trim()
}
try {
    Admin "CREATE DATABASE $database CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_as_cs;" | Out-Null
    $createdDatabase = $true
    Admin "CREATE USER '$testUser'@'localhost' IDENTIFIED BY '$secret' REQUIRE SSL;" | Out-Null
    $createdUser = $true
    Admin "GRANT SELECT, INSERT, UPDATE, DELETE ON $database.* TO '$testUser'@'localhost';" | Out-Null
    $migrations = @('create_accounts', 'create_characters', 'add_character_statistics', 'add_battle_records',
        'create_inventory_slots', 'create_cash_inventory_slots','create_cash_bundles', 'create_weapon_presets', 'add_weapon_preset_equipment', 'create_social_relationships', 'create_friend_groups', 'create_social_messages',
        'expand_cash_inventory_usage', 'create_match_settlements', 'create_cash_purchases', 'create_gifts','create_gift_bundles')
    foreach ($name in $migrations) {
        $sql = Get-Content -Raw -LiteralPath (Join-Path $repositoryRoot "src/persistence/migrations/$name.sql")
        Admin ("USE $database;" + $lineBreak + $sql) | Out-Null
    }
    [IO.File]::WriteAllText($appConfig, @"
[client]
host=127.0.0.1
port=$Port
protocol=TCP
user=$testUser
password="$secret"
ssl-mode=REQUIRED
[mysql]
database=$database
"@, $utf8)
    # Isolated fixture fails receipt persistence after character writes.
    Admin @"
USE $database;
DELIMITER //
CREATE TRIGGER AwardsRollbackFixture BEFORE UPDATE ON MatchSettlements FOR EACH ROW
BEGIN
 IF OCTET_LENGTH(OLD.ResultData)=0 AND OCTET_LENGTH(NEW.ResultData)>0 AND EXISTS(SELECT 1 FROM Characters c JOIN Accounts a ON a.AccountId=c.AccountId WHERE a.Username='awards_1' AND c.Cash=99) THEN
  SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='Injected receipt persistence failure';
 END IF;
END//
DELIMITER ;
"@ | Out-Null
    RunTest '--battle-awards-database-tests'
    $networkReport = Join-Path $logDirectory ('awards_' + $Configuration + '_' + $suffix + '_network.json')
    & py ("-" + $PythonVersion) (Join-Path $repositoryRoot 'tests/battle_awards_network_test.py') --executable $serverExe --database-config $appConfig --mysql $mysql --report $networkReport
    if ($LASTEXITCODE -ne 0) { throw 'Battle awards TCP checks failed.' }
    $passed = $true
}
finally {
    if ($createdDatabase) { Admin "DROP DATABASE $database;" | Out-Null }
    if ($createdUser) { Admin "DROP USER '$testUser'@'localhost';" | Out-Null }
    $remaining = @(Admin "SELECT COUNT(*) FROM information_schema.schemata WHERE schema_name='$database'; SELECT COUNT(*) FROM mysql.user WHERE User='$testUser' AND Host='localhost';")
    $cleaned = $remaining.Count -eq 2 -and $remaining[0] -eq '0' -and $remaining[1] -eq '0'
    foreach ($file in (@($appConfig, $ticketFile) + $networkTickets)) {
        $resolved = [IO.Path]::GetFullPath($file)
        $boundary = [IO.Path]::GetFullPath($privateDirectory) + [IO.Path]::DirectorySeparatorChar
        if (-not $resolved.StartsWith($boundary, [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid cleanup target.' }
        if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved }
    }
    $report = [ordered]@{
        utc=[DateTimeOffset]::UtcNow.ToString('o'); configuration=$Configuration
        database=$database; user=$testUser; passed=$passed
        database_and_user_removed=$cleaned; private_credentials_removed=(-not (Test-Path -LiteralPath $appConfig) -and -not (Test-Path -LiteralPath $ticketFile))
        business_database_modified=$false; cli_checks=$cliChecks
        network_ticket_files_removed=(@($networkTickets | Where-Object { Test-Path -LiteralPath $_ }).Count -eq 0)
        server_sha256=(Get-FileHash -LiteralPath $serverExe -Algorithm SHA256).Hash.ToLowerInvariant()
        regression_sha256=(Get-FileHash -LiteralPath $testExe -Algorithm SHA256).Hash.ToLowerInvariant()
        test_output=$transcript.ToArray(); log=$logPath
    }
    [IO.File]::WriteAllText($reportPath, ($report | ConvertTo-Json -Depth 4) + $lineBreak, $utf8)
    if (-not $cleaned) { throw 'Cleanup verification failed.' }
}
if (-not $passed) { throw 'Battle awards database checks did not complete.' }
Write-Output "PASS: $Configuration isolated MySQL, battle awards and TCP checks; generated database, user and credentials removed. Report: $reportPath"
