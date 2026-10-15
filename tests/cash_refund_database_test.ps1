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
$database = 'BfRefundTests_' + $suffix
$testUser = 'bf_refund_' + $suffix
if ($database -cnotmatch '^BfRefundTests_[0-9a-f]{12}$' -or $testUser -cnotmatch '^bf_refund_[0-9a-f]{12}$') { throw 'Invalid generated scope.' }
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
$logDirectory = Join-Path $repositoryRoot 'Artifacts/Logs'
New-Item -ItemType Directory -Path $logDirectory -Force | Out-Null
$logPath = Join-Path $logDirectory ("refund_" + $Configuration + "_" + $suffix + ".log")
$reportPath = Join-Path $logDirectory ("refund_" + $Configuration + "_" + $suffix + ".json")
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
$transcript = [Collections.Generic.List[string]]::new()
function Admin([string]$Sql, [switch]$ExpectExistingTable) {
    # Never echo SQL/raw errors: CREATE USER contains a secret.
    $result = $Sql | & $mysql "--defaults-file=$adminFile" --no-login-paths --batch --skip-column-names 2>&1
    $code = $LASTEXITCODE
    if ($ExpectExistingTable) {
        if ($code -eq 0 -or ($result -join ' ') -notmatch 'ERROR 1050') { throw 'Existing table migration did not reject.' }
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
$migrationPreserved = $false
$schemaGuard = $false
try {
    Admin "CREATE DATABASE $database CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_as_cs;" | Out-Null
    $createdDatabase = $true
    Admin "CREATE USER '$testUser'@'localhost' IDENTIFIED BY '$secret' REQUIRE SSL;" | Out-Null
    $createdUser = $true
    Admin "GRANT SELECT, INSERT, UPDATE, DELETE ON $database.* TO '$testUser'@'localhost';" | Out-Null
    $migrations = @('create_accounts','create_characters','add_character_statistics', 'add_battle_records','create_inventory_slots','create_cash_inventory_slots','create_cash_bundles','create_weapon_presets','expand_cash_inventory_usage','create_match_settlements','add_weapon_preset_equipment','create_social_relationships','create_friend_groups','create_social_messages','create_gifts','create_gift_bundles')
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
    $schemaReport = Join-Path $logDirectory ('refund_' + $Configuration + '_' + $suffix + '_schema.json')
    & py ("-" + $PythonVersion) (Join-Path $repositoryRoot 'tests/cash_refund_network_test.py') --executable $serverExe --database-config $appConfig --mysql $mysql --expect-missing-schema --report $schemaReport
    if ($LASTEXITCODE -ne 0) { throw 'Missing cash purchase schema did not prevent listening.' }
    $schemaGuard = $true
    Admin @"
USE $database;
INSERT INTO Accounts (Username,PasswordSalt,PasswordHash,PasswordIterations) VALUES ('refund_migration',UNHEX(REPEAT('00',16)),UNHEX(REPEAT('00',32)),600000);
INSERT INTO Characters (AccountId,NameUtf16,CharacterType,ColorIndex,Cash) SELECT AccountId,UNHEX('4D00690067007200610074006500'),10,7,999999 FROM Accounts WHERE Username='refund_migration';
INSERT INTO CashInventorySlots (SlotId,CharacterId,StockId,ItemId,UsageKind,Amount,MaxAmount,CashPrice,PurchasedAtUnixSeconds) SELECT 99998000,CharacterId,1010816,102613,1,65535,1000000000,999999,1800000000 FROM Characters WHERE NameUtf16=UNHEX('4D00690067007200610074006500');
INSERT INTO CashInventorySlots (SlotId,CharacterId,StockId,ItemId,UsageKind,Amount,MaxAmount,CashPrice,PurchasedAtUnixSeconds) SELECT 99998001,CharacterId,1010816,102613,1,34465,1000000000,0,1800000000 FROM Characters WHERE NameUtf16=UNHEX('4D00690067007200610074006500');
"@ | Out-Null
    $before = @(Admin "SELECT * FROM $database.CashInventorySlots ORDER BY SlotId; SELECT CharacterId,Cash FROM $database.Characters ORDER BY CharacterId; SELECT * FROM $database.CashInventorySequence;")
    $migration = Get-Content -Raw -LiteralPath (Join-Path $repositoryRoot 'src/persistence/migrations/create_cash_purchases.sql')
    Admin ("USE $database;" + $lineBreak + $migration) | Out-Null
    Admin ("USE $database;" + $lineBreak + $migration) -ExpectExistingTable
    $after = @(Admin "SELECT * FROM $database.CashInventorySlots ORDER BY SlotId; SELECT CharacterId,Cash FROM $database.Characters ORDER BY CharacterId; SELECT * FROM $database.CashInventorySequence;")
    if ($before.Count -ne 4 -or ($before -join '|') -ne ($after -join '|')) { throw 'Cash purchase migration changed old rows.' }
    if ((Admin "SELECT COUNT(*) FROM $database.CashPurchases;") -ne '0') { throw 'Migration inferred historical ownership.' }
    $migrationPreserved = $true
    Admin @"
USE $database;
DELIMITER //
CREATE TRIGGER CashReceiptInsertFailure BEFORE INSERT ON CashPurchases FOR EACH ROW
BEGIN
 IF EXISTS (SELECT 1 FROM Characters c JOIN Accounts a ON a.AccountId=c.AccountId WHERE c.CharacterId=NEW.CharacterId AND a.Username='refund_fail_receipt') THEN
 SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='Synthetic cash receipt insert failure';
 END IF;
END//
CREATE TRIGGER CashPurchasePartFailure BEFORE INSERT ON CashPurchaseSlots FOR EACH ROW
BEGIN
 IF NEW.SlotId<>NEW.PurchaseId AND EXISTS (SELECT 1 FROM CashPurchases p JOIN Characters c ON c.CharacterId=p.CharacterId JOIN Accounts a ON a.AccountId=c.AccountId WHERE p.PurchaseId=NEW.PurchaseId AND a.Username='refund_fail_part') THEN
 SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='Synthetic cash purchase second part failure';
 END IF;
END//
CREATE TRIGGER CashReceiptStateFailure BEFORE UPDATE ON CashPurchases FOR EACH ROW
BEGIN
 IF EXISTS (SELECT 1 FROM Characters c JOIN Accounts a ON a.AccountId=c.AccountId WHERE c.CharacterId=NEW.CharacterId AND ((a.Username='refund_fail_claim' AND NEW.PurchaseState=1) OR (a.Username='refund_fail_mark' AND NEW.PurchaseState=2))) THEN
 SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='Synthetic cash receipt state failure';
 END IF;
END//
CREATE TRIGGER CashRefundDeleteFailure BEFORE DELETE ON CashInventorySlots FOR EACH ROW
BEGIN
 IF OLD.CashPrice=0 AND EXISTS (SELECT 1 FROM Characters c JOIN Accounts a ON a.AccountId=c.AccountId WHERE c.CharacterId=OLD.CharacterId AND a.Username='refund_fail_delete') THEN
 SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='Synthetic cash refund second part failure';
 END IF;
END//
CREATE TRIGGER CashRefundCreditFailure BEFORE UPDATE ON Characters FOR EACH ROW
BEGIN
 IF OLD.Cash>0 AND NEW.Cash>OLD.Cash AND EXISTS (SELECT 1 FROM Accounts a WHERE a.AccountId=OLD.AccountId AND a.Username='refund_fail_credit') THEN
 SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='Synthetic cash refund credit failure';
 END IF;
END//
DELIMITER ;
"@ | Out-Null
    RunTest '--cash-refund-database-tests'
    $networkReport = Join-Path $logDirectory ('refund_' + $Configuration + '_' + $suffix + '_network.json')
    & py ("-" + $PythonVersion) (Join-Path $repositoryRoot 'tests/cash_refund_network_test.py') --executable $serverExe --database-config $appConfig --mysql $mysql --report $networkReport
    if ($LASTEXITCODE -ne 0) { throw 'Cash refund TCP verification failed.' }
    $passed = $true
}

finally {
    if ($createdDatabase) { Admin "DROP DATABASE $database;" | Out-Null }
    if ($createdUser) { Admin "DROP USER '$testUser'@'localhost';" | Out-Null }
    $remaining = @(Admin "SELECT COUNT(*) FROM information_schema.schemata WHERE schema_name='$database'; SELECT COUNT(*) FROM mysql.user WHERE User='$testUser' AND Host='localhost';")
    $cleaned = $remaining.Count -eq 2 -and $remaining[0] -eq '0' -and $remaining[1] -eq '0'
    $networkTickets = @(Get-ChildItem -LiteralPath $privateDirectory -File -Filter '*.ticket' | ForEach-Object { $_.FullName })
    foreach ($file in (@($appConfig) + $networkTickets)) {
        $resolved = [IO.Path]::GetFullPath($file)
        $boundary = [IO.Path]::GetFullPath($privateDirectory) + [IO.Path]::DirectorySeparatorChar
        if (-not $resolved.StartsWith($boundary, [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid cleanup target.' }
        if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved }
    }
    $report = [ordered]@{
        utc=[DateTimeOffset]::UtcNow.ToString('o'); configuration=$Configuration
        database=$database; user=$testUser; passed=$passed
        database_and_user_removed=$cleaned; private_credentials_removed=(-not (Test-Path -LiteralPath $appConfig))
        business_database_modified=$false; cli_checks=$cliChecks
        cash_migration_preserved_existing_slots=$migrationPreserved
        refund_schema_guard_passed=$schemaGuard
        network_ticket_files_removed=(@(Get-ChildItem -LiteralPath $privateDirectory -File -Filter '*.ticket').Count -eq 0)
        test_output=$transcript.ToArray(); log=$logPath
    }
    [IO.File]::WriteAllText($reportPath, ($report | ConvertTo-Json -Depth 4) + $lineBreak, $utf8)
    if (-not $cleaned) { throw 'Cleanup verification failed.' }
}
if (-not $passed) { throw 'Cash refund database checks did not complete.' }
Write-Output "PASS: $Configuration isolated refund MySQL/TCP; generated database, user and credentials removed. Report: $reportPath"
