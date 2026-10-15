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
$database = 'BfGiftTests_' + $suffix
$testUser = 'bf_gift_' + $suffix
if ($database -cnotmatch '^BfGiftTests_[0-9a-f]{12}$' -or $testUser -cnotmatch '^bf_gift_[0-9a-f]{12}$') { throw 'Invalid generated scope.' }
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
$logPath = Join-Path $logDirectory ("gift_" + $Configuration + "_" + $suffix + ".log")
$reportPath = Join-Path $logDirectory ("gift_" + $Configuration + "_" + $suffix + ".json")
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
    if ($code -ne 0) {
        $codes = [regex]::Matches(($result -join ' '), 'ERROR [0-9]+') | ForEach-Object { $_.Value }
        throw "Isolated database administration failed at line $($MyInvocation.ScriptLineNumber): $($codes -join ', ')."
    }
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
    $migrations = @('create_accounts','create_characters','add_character_statistics', 'add_battle_records','create_inventory_slots','create_cash_inventory_slots','create_cash_bundles','create_weapon_presets','expand_cash_inventory_usage','create_match_settlements','add_weapon_preset_equipment','create_social_relationships','create_friend_groups','create_social_messages','create_cash_purchases')
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
    $schemaReport = Join-Path $logDirectory ('gift_' + $Configuration + '_' + $suffix + '_schema.json')
    & py ("-" + $PythonVersion) (Join-Path $repositoryRoot 'tests/gift_network_test.py') --executable $serverExe --database-config $appConfig --mysql $mysql --expect-missing-schema --report $schemaReport
    if ($LASTEXITCODE -ne 0) { throw 'Missing gift schema did not prevent listening.' }
    $schemaGuard = $true
    Admin @"
USE $database;
INSERT INTO Accounts (Username,PasswordSalt,PasswordHash,PasswordIterations) VALUES ('gift_migration',UNHEX(REPEAT('00',16)),UNHEX(REPEAT('00',32)),600000);
INSERT INTO Characters (AccountId,NameUtf16,CharacterType,ColorIndex,Cash) SELECT AccountId,UNHEX('4D00690067007200610074006500'),10,7,999999 FROM Accounts WHERE Username='gift_migration';
INSERT INTO CashInventorySlots (SlotId,CharacterId,StockId,ItemId,UsageKind,Amount,MaxAmount,CashPrice,PurchasedAtUnixSeconds) SELECT 99998000,CharacterId,1000139,205034,1,100,20000,1000,1800000000 FROM Characters WHERE NameUtf16=UNHEX('4D00690067007200610074006500');
"@ | Out-Null
    $before = @(Admin "SELECT * FROM $database.Characters; SELECT * FROM $database.CashInventorySlots; SELECT * FROM $database.CashInventorySequence;")
    $migration = Get-Content -Raw -LiteralPath (Join-Path $repositoryRoot 'src/persistence/migrations/create_gifts.sql')
    Admin ("USE $database;" + $lineBreak + $migration) | Out-Null
    Admin ("USE $database;" + $lineBreak + $migration) -ExpectExistingTable
    $after = @(Admin "SELECT * FROM $database.Characters; SELECT * FROM $database.CashInventorySlots; SELECT * FROM $database.CashInventorySequence;")
    if ($before.Count -ne 3 -or ($before -join '|') -ne ($after -join '|')) { throw 'Gift migration changed old rows.' }
    if ((Admin "SELECT COUNT(*) FROM $database.Gifts;") -ne '0') { throw 'Gift migration invented deliveries.' }
    $bundleMigration = Get-Content -Raw -LiteralPath (Join-Path $repositoryRoot 'src/persistence/migrations/create_gift_bundles.sql')
    Admin @"
USE $database;
INSERT INTO Accounts (Username,PasswordSalt,PasswordHash,PasswordIterations) VALUES ('gift_migration_peer',UNHEX(REPEAT('00',16)),UNHEX(REPEAT('00',32)),600000);
INSERT INTO Characters (AccountId,NameUtf16,CharacterType,ColorIndex,Cash) SELECT AccountId,UNHEX('5000650065007200'),10,7,999999 FROM Accounts WHERE Username='gift_migration_peer';
INSERT INTO Gifts (GiftId,SenderCharacterId,RecipientCharacterId,SenderNameUtf16,RecipientNameUtf16,StockId,ItemId,UsageKind,Amount,MaxAmount,Price,IsCash,SentAtUnixSeconds,MessageUtf16) SELECT 99998000,s.CharacterId,r.CharacterId,s.NameUtf16,r.NameUtf16,1000139,205034,1,100,20000,1000,1,1800000000,X'' FROM Characters s JOIN Accounts a ON a.AccountId=s.AccountId CROSS JOIN Characters r JOIN Accounts b ON b.AccountId=r.AccountId WHERE a.Username='gift_migration' AND b.Username='gift_migration_peer';
"@ | Out-Null
    $bundleBefore = @(Admin "SELECT * FROM $database.Characters; SELECT * FROM $database.CashInventorySlots; SELECT * FROM $database.CashInventorySequence; SELECT * FROM $database.Gifts; SELECT * FROM $database.GiftSequence;")
    Admin ("USE $database;" + $lineBreak + $bundleMigration) | Out-Null
    Admin ("USE $database;" + $lineBreak + $bundleMigration) -ExpectExistingTable
    $bundleAfter = @(Admin "SELECT * FROM $database.Characters; SELECT * FROM $database.CashInventorySlots; SELECT * FROM $database.CashInventorySequence; SELECT * FROM $database.Gifts; SELECT * FROM $database.GiftSequence;")
    if ($bundleBefore.Count -ne 6 -or ($bundleBefore -join '|') -ne ($bundleAfter -join '|')) { throw 'Gift bundle migration changed old rows.' }
    foreach ($table in @('GiftBundles', 'GiftBundleComponents')) {
        Admin "RENAME TABLE $database.$table TO $database.$($table)Missing;" | Out-Null
        $guardReport = Join-Path $logDirectory ('gift_' + $Configuration + '_' + $suffix + '_' + $table + '_schema.json')
        & py ("-" + $PythonVersion) (Join-Path $repositoryRoot 'tests/gift_network_test.py') --executable $serverExe --database-config $appConfig --mysql $mysql --expect-missing-schema --report $guardReport
        if ($LASTEXITCODE -ne 0) { throw "Missing $table did not prevent listening." }
        Admin "RENAME TABLE $database.$($table)Missing TO $database.$table;" | Out-Null
    }
    $migrationPreserved = $true
    Admin "RENAME TABLE $database.GiftSequence TO $database.GiftSequenceMissing;" | Out-Null
    $sequenceReport = Join-Path $logDirectory ('gift_' + $Configuration + '_' + $suffix + '_sequence_schema.json')
    & py ("-" + $PythonVersion) (Join-Path $repositoryRoot 'tests/gift_network_test.py') --executable $serverExe --database-config $appConfig --mysql $mysql --expect-missing-schema --report $sequenceReport
    if ($LASTEXITCODE -ne 0) { throw 'Missing gift sequence did not prevent listening.' }
    Admin "RENAME TABLE $database.GiftSequenceMissing TO $database.GiftSequence;" | Out-Null
    # The preserved legacy fixture is independent of the lifecycle's empty scope.
    Admin "USE $database; DELETE FROM $database.Gifts WHERE GiftId=99998000; DELETE FROM $database.CashInventorySlots WHERE SlotId=99998000; DELETE c FROM $database.Characters c JOIN $database.Accounts a ON a.AccountId=c.AccountId WHERE a.Username IN ('gift_migration','gift_migration_peer'); DELETE FROM $database.Accounts WHERE Username IN ('gift_migration','gift_migration_peer');" | Out-Null
    Admin @"
USE $database;
DELIMITER //
CREATE TRIGGER GiftInsertFailure BEFORE INSERT ON Gifts FOR EACH ROW
BEGIN
 IF EXISTS (SELECT 1 FROM Characters c JOIN Accounts a ON a.AccountId=c.AccountId WHERE c.CharacterId=NEW.SenderCharacterId AND a.Username='gift_fail_insert') THEN
 SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='Synthetic gift insert failure';
 END IF;
END//
CREATE TRIGGER BundleGiftHeaderFailure BEFORE INSERT ON GiftBundles FOR EACH ROW
BEGIN
 IF EXISTS (SELECT 1 FROM Gifts g JOIN Characters c ON c.CharacterId=g.SenderCharacterId JOIN Accounts a ON a.AccountId=c.AccountId WHERE g.GiftId=NEW.GiftId AND a.Username='bgift_header') THEN
 SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='Synthetic gift bundle header failure';
 END IF;
END//
CREATE TRIGGER BundleGiftComponentFailure BEFORE INSERT ON GiftBundleComponents FOR EACH ROW
BEGIN
 IF NEW.ComponentIndex=2 AND EXISTS (SELECT 1 FROM Gifts g JOIN Characters c ON c.CharacterId=g.SenderCharacterId JOIN Accounts a ON a.AccountId=c.AccountId WHERE g.GiftId=NEW.GiftId AND a.Username='bgift_component') THEN
 SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='Synthetic gift bundle component failure';
 END IF;
END//
CREATE TRIGGER BundleGiftGrantFailure BEFORE INSERT ON InventorySlots FOR EACH ROW
BEGIN
 IF NEW.ItemId=100241 AND EXISTS (SELECT 1 FROM Characters c JOIN Accounts a ON a.AccountId=c.AccountId WHERE c.CharacterId=NEW.CharacterId AND a.Username='bgift_grant') THEN
 SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='Synthetic gift bundle third grant failure';
 END IF;
END//
CREATE TRIGGER BundleGiftBonusFailure BEFORE UPDATE ON Characters FOR EACH ROW
BEGIN
 IF NEW.Lucci=OLD.Lucci+60000 AND EXISTS (SELECT 1 FROM Accounts a WHERE a.AccountId=NEW.AccountId AND a.Username='bgift_bonus') THEN
 SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='Synthetic gift bundle bonus failure';
 END IF;
END//
CREATE TRIGGER BundleGiftStateFailure BEFORE UPDATE ON Gifts FOR EACH ROW
BEGIN
 IF NEW.GiftState=1 AND EXISTS (SELECT 1 FROM Characters c JOIN Accounts a ON a.AccountId=c.AccountId WHERE c.CharacterId=NEW.RecipientCharacterId AND a.Username='bgift_state') THEN
 SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='Synthetic gift bundle final state failure';
 END IF;
END//
CREATE TRIGGER GiftStateFailure BEFORE UPDATE ON Gifts FOR EACH ROW
BEGIN
 IF EXISTS (SELECT 1 FROM Characters c JOIN Accounts a ON a.AccountId=c.AccountId WHERE c.CharacterId=NEW.RecipientCharacterId AND ((a.Username='gift_fail_claim' AND NEW.GiftState=1) OR (a.Username='gift_fail_thanks' AND NEW.ThanksPayload IS NOT NULL))) THEN
 SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='Synthetic gift state failure';
 END IF;
END//
DELIMITER ;
"@ | Out-Null
    RunTest '--gift-database-tests'
    $networkReport = Join-Path $logDirectory ('gift_' + $Configuration + '_' + $suffix + '_network.json')
    & py ("-" + $PythonVersion) (Join-Path $repositoryRoot 'tests/gift_network_test.py') --executable $serverExe --database-config $appConfig --mysql $mysql --report $networkReport
    if ($LASTEXITCODE -ne 0) { throw 'Gift TCP verification failed.' }
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
        gift_migration_preserved_existing_rows=$migrationPreserved
        gift_schema_guard_passed=$schemaGuard
        network_ticket_files_removed=(@(Get-ChildItem -LiteralPath $privateDirectory -File -Filter '*.ticket').Count -eq 0)
        test_output=$transcript.ToArray(); log=$logPath
    }
    [IO.File]::WriteAllText($reportPath, ($report | ConvertTo-Json -Depth 4) + $lineBreak, $utf8)
    if (-not $cleaned) { throw 'Cleanup verification failed.' }
}
if (-not $passed) { throw 'Gift database checks did not complete.' }
Write-Output "PASS: $Configuration isolated gift MySQL/TCP; generated database, user and credentials removed. Report: $reportPath"
