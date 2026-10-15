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
$database = 'BfSocialTests_' + $suffix
$testUser = 'bf_social_' + $suffix
if ($database -cnotmatch '^BfSocialTests_[0-9a-f]{12}$' -or $testUser -cnotmatch '^bf_social_[0-9a-f]{12}$') { throw 'Invalid generated scope.' }
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
$logPath = Join-Path $logDirectory ("social_" + $Configuration + "_" + $suffix + ".log")
$reportPath = Join-Path $logDirectory ("social_" + $Configuration + "_" + $suffix + ".json")
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
        'create_inventory_slots', 'create_cash_inventory_slots','create_cash_bundles', 'create_weapon_presets',
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
    RunTest '--before-preset-migration'
    $migration = Get-Content -Raw -LiteralPath (Join-Path $repositoryRoot 'src/persistence/migrations/add_weapon_preset_equipment.sql')
    Admin ("USE $database;" + $lineBreak + $migration) | Out-Null
    Admin ("USE $database;" + $lineBreak + $migration) -ExpectDuplicateColumn
    RunTest '--after-preset-migration'
    Admin @"
USE $database;
DELIMITER //
CREATE TRIGGER PresetInsertFailure BEFORE INSERT ON WeaponPresetSlots FOR EACH ROW
BEGIN
    IF NEW.PresetIndex = 4 AND EXISTS (
        SELECT 1 FROM Characters c JOIN Accounts a ON a.AccountId = c.AccountId
        WHERE c.CharacterId = NEW.CharacterId AND a.Username = 'preset_rollback') THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Synthetic last preset insert failure';
    END IF;
END//
DELIMITER ;
"@ | Out-Null
    RunTest '--database-tests'
    RunTest '--settlement-profile-database-tests'
    $schemaReport = Join-Path $logDirectory ('social_' + $Configuration + '_' + $suffix + '_schema.json')
    & py ("-" + $PythonVersion) (Join-Path $repositoryRoot 'tests/social_network_test.py') --executable $serverExe --database-config $appConfig --expect-missing-schema --report $schemaReport
    if ($LASTEXITCODE -ne 0) { throw 'Missing social schema did not prevent listening.' }
    $socialMigration = Get-Content -Raw -LiteralPath (Join-Path $repositoryRoot 'src/persistence/migrations/create_social_relationships.sql')
    Admin ("USE $database;" + $lineBreak + $socialMigration) | Out-Null
    # Seed pre-migration relationships in the generated schema and preserve them.
    Admin "INSERT INTO $database.SocialFriendships (FirstCharacterId, SecondCharacterId, RequestedBy, IsAccepted) SELECT a.CharacterId, b.CharacterId, a.CharacterId, 1 FROM $database.Characters a JOIN $database.Characters b ON a.CharacterId < b.CharacterId ORDER BY a.CharacterId, b.CharacterId LIMIT 1; INSERT INTO $database.SocialChatBlocks (OwnerCharacterId, TargetCharacterId) SELECT FirstCharacterId, SecondCharacterId FROM $database.SocialFriendships;" | Out-Null
    $beforeGroups = @(Admin "SELECT FirstCharacterId, SecondCharacterId, RequestedBy, IsAccepted FROM $database.SocialFriendships; SELECT OwnerCharacterId, TargetCharacterId FROM $database.SocialChatBlocks;")
    if ($beforeGroups.Count -ne 2) { throw 'Missing pre-group relationship fixture.' }
    $groupSchemaReport = Join-Path $logDirectory ('social_' + $Configuration + '_' + $suffix + '_group_schema.json')
    & py ("-" + $PythonVersion) (Join-Path $repositoryRoot 'tests/social_network_test.py') --executable $serverExe --database-config $appConfig --expect-missing-schema --report $groupSchemaReport
    if ($LASTEXITCODE -ne 0) { throw 'Missing friend group schema did not prevent listening.' }
    $groupMigration = Get-Content -Raw -LiteralPath (Join-Path $repositoryRoot 'src/persistence/migrations/create_friend_groups.sql')
    Admin ("USE $database;" + $lineBreak + $groupMigration) | Out-Null
    Admin ("USE $database;" + $lineBreak + $groupMigration) -ExpectExistingTable
    $afterGroups = @(Admin "SELECT FirstCharacterId, SecondCharacterId, RequestedBy, IsAccepted FROM $database.SocialFriendships; SELECT OwnerCharacterId, TargetCharacterId FROM $database.SocialChatBlocks;")
    if (($beforeGroups -join '|') -ne ($afterGroups -join '|')) { throw 'Friend group migration changed existing relationships.' }
    $groupMigrationPreserved = $true
    $mailboxSchemaReport = Join-Path $logDirectory ('social_' + $Configuration + '_' + $suffix + '_mailbox_schema.json')
    & py ("-" + $PythonVersion) (Join-Path $repositoryRoot 'tests/social_network_test.py') --executable $serverExe --database-config $appConfig --expect-missing-schema --report $mailboxSchemaReport
    if ($LASTEXITCODE -ne 0) { throw 'Missing mailbox schema did not prevent listening.' }
    $mailboxMigration = Get-Content -Raw -LiteralPath (Join-Path $repositoryRoot 'src/persistence/migrations/create_social_messages.sql')
    Admin ("USE $database;" + $lineBreak + $mailboxMigration) | Out-Null
    Admin ("USE $database;" + $lineBreak + $mailboxMigration) -ExpectExistingTable
    $afterMailbox = @(Admin "SELECT FirstCharacterId, SecondCharacterId, RequestedBy, IsAccepted FROM $database.SocialFriendships; SELECT OwnerCharacterId, TargetCharacterId FROM $database.SocialChatBlocks;")
    if (($afterGroups -join '|') -ne ($afterMailbox -join '|')) { throw 'Mailbox migration changed existing relationships.' }
    Admin "ALTER TABLE $database.SocialMessages AUTO_INCREMENT = 9007199254740993;" | Out-Null
    Admin @"
USE $database;
DELIMITER //
CREATE TRIGGER MessageInsertFailure BEFORE INSERT ON SocialMessages FOR EACH ROW
BEGIN
    IF NEW.TextUtf16 = X'4600610069006C0049006E007300650072007400' THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Synthetic message insert failure';
    END IF;
END//
CREATE TRIGGER MessageDeleteFailure BEFORE DELETE ON SocialMessages FOR EACH ROW
BEGIN
    IF OLD.TextUtf16 = X'4600610069006C00410063006B00' THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Synthetic message delete failure';
    END IF;
END//
DELIMITER ;
"@ | Out-Null
    Admin @"
USE $database;
DELIMITER //
CREATE TRIGGER SocialBlockInsertFailure BEFORE INSERT ON SocialChatBlocks FOR EACH ROW
BEGIN
    IF EXISTS (SELECT 1 FROM Characters c JOIN Accounts a ON a.AccountId = c.AccountId
               WHERE c.CharacterId = NEW.OwnerCharacterId AND a.Username = 'social_carol') THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Synthetic social block failure';
    END IF;
END//
DELIMITER ;
"@ | Out-Null
    RunTest '--social-database-tests'
    Admin @"
USE $database;
DELIMITER //
CREATE TRIGGER FriendGroupInsertFailure BEFORE INSERT ON SocialFriendGroupAssignments FOR EACH ROW
BEGIN
    IF EXISTS (SELECT 1 FROM SocialFriendGroups g JOIN Characters c ON c.CharacterId = g.OwnerCharacterId JOIN Accounts a ON a.AccountId = c.AccountId
               WHERE g.GroupId = NEW.GroupId AND a.Username = 'group_alice' AND g.NameUtf16 = X'52006F006C006C006200610063006B00') THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Synthetic friend group second-write failure';
    END IF;
END//
CREATE TRIGGER FriendGroupRenameFailure BEFORE UPDATE ON SocialFriendGroups FOR EACH ROW
BEGIN
    IF NEW.NameUtf16 = X'4500780070006C006F0064006500' THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Synthetic friend group rename failure';
    END IF;
END//
DELIMITER ;
"@ | Out-Null
    RunTest '--friend-group-database-tests'
    RunTest '--offline-message-database-tests'
    RunTest '--seed-social-network'
    $networkUsers = @('net_host','net_guest','net_observer')
    for ($i = 0; $i -lt 3; ++$i) {
        $issueOutput = 'Synthetic-network-password' | & $serverExe --login $networkUsers[$i] --database-config $appConfig --ticket-file $networkTickets[$i] 2>&1
        if ($LASTEXITCODE -ne 0) { throw 'Network ticket issue failed.' }
    }
    $networkReport = Join-Path $logDirectory ('social_' + $Configuration + '_' + $suffix + '_network.json')
    & py ("-" + $PythonVersion) (Join-Path $repositoryRoot 'tests/social_network_test.py') --executable $serverExe --database-config $appConfig --tickets $networkTickets[0] $networkTickets[1] $networkTickets[2] --report $networkReport
    if ($LASTEXITCODE -ne 0) { throw 'Authenticated social TCP checks failed.' }
    $consumed = Admin "SELECT COUNT(*) FROM $database.LoginTickets t JOIN $database.Accounts a ON a.AccountId=t.AccountId WHERE a.Username IN ('net_host','net_guest','net_observer') AND t.ConsumedAt IS NOT NULL;"
    if ($consumed -ne '3') { throw 'Network did not consume exactly three authenticated tickets.' }
    $publicLogs = [IO.File]::ReadAllText($networkReport) + [IO.File]::ReadAllText([IO.Path]::ChangeExtension($networkReport, '.server.log'))
    foreach ($path in $networkTickets) {
        if ($publicLogs.Contains([IO.File]::ReadAllText($path).Trim())) { throw 'Private ticket appeared in a public network log.' }
    }
    $mailboxNetworkReport = Join-Path $logDirectory ('social_' + $Configuration + '_' + $suffix + '_mailbox_network.json')
    & py ("-" + $PythonVersion) (Join-Path $repositoryRoot 'tests/offline_message_network_test.py') --executable $serverExe --database-config $appConfig --mysql $mysql --report $mailboxNetworkReport
    if ($LASTEXITCODE -ne 0) { throw 'Durable mailbox authenticated TCP checks failed.' }
    $password = 'Synthetic-cli-password-42'
    $output = $password | & $serverExe --create-account preset_cli --database-config $appConfig 2>&1
    if ($LASTEXITCODE -ne 0 -or ($output -join ' ').Contains($password)) { throw 'CLI creation failed.' }
    ++$cliChecks
    $output = 'Wrong-password-42' | & $serverExe --login preset_cli --database-config $appConfig --ticket-file $ticketFile 2>&1
    if ($LASTEXITCODE -ne 3 -or (Test-Path -LiteralPath $ticketFile)) { throw 'Wrong password issued a ticket.' }
    ++$cliChecks
    $output = $password | & $serverExe --login preset_cli --database-config $appConfig --ticket-file $ticketFile 2>&1
    if ($LASTEXITCODE -ne 0) { throw 'CLI login failed.' }
    $ticket = [IO.File]::ReadAllText($ticketFile)
    if ($ticket -cnotmatch '^BFS1\.[0-9a-f]{64}$' -or ($output -join ' ').Contains($ticket) -or ($output -join ' ').Contains($password)) { throw 'CLI ticket format/output failed.' }
    ++$cliChecks
    $ticketAcl = Get-Acl -LiteralPath $ticketFile
    if (-not $ticketAcl.AreAccessRulesProtected) { throw 'Ticket ACL inherits permissions.' }
    foreach ($rule in $ticketAcl.Access) {
        if ($rule.IdentityReference.Translate([Security.Principal.SecurityIdentifier]).Value -notin @($owner.Value, 'S-1-5-18', 'S-1-5-32-544')) { throw 'Unexpected ticket principal.' }
    }
    ++$cliChecks
    $hash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($ticket)))
    $stored = Admin "SELECT COUNT(*) FROM $database.LoginTickets WHERE TicketHash=UNHEX('$hash') AND ConsumedAt IS NULL;"
    if ($stored -ne '1') { throw 'Ticket digest was not persisted.' }
    ++$cliChecks
    $output = $password | & $serverExe --login preset_cli --database-config $appConfig --ticket-file $ticketFile 2>&1
    if ($LASTEXITCODE -ne 1 -or [IO.File]::ReadAllText($ticketFile) -ne $ticket) { throw 'Existing private ticket was overwritten.' }
    ++$cliChecks
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
        friend_group_migration_preserved_relationships=$groupMigrationPreserved
        mailbox_migration_preserved_relationships=($passed -and (($afterGroups -join '|') -eq ($afterMailbox -join '|')))
        network_ticket_files_removed=(@($networkTickets | Where-Object { Test-Path -LiteralPath $_ }).Count -eq 0)
        test_output=$transcript.ToArray(); log=$logPath
    }
    [IO.File]::WriteAllText($reportPath, ($report | ConvertTo-Json -Depth 4) + $lineBreak, $utf8)
    if (-not $cleaned) { throw 'Cleanup verification failed.' }
}
if (-not $passed) { throw 'Preset database checks did not complete.' }
Write-Output "PASS: $Configuration isolated MySQL, $cliChecks CLI checks; generated database, user and credentials removed. Report: $reportPath"
