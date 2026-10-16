[CmdletBinding()]
param(
    [string]$MySqlExecutable,
    [string]$DatabaseConfiguration = (Join-Path $PSScriptRoot 'config\database.ini')
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (!$MySqlExecutable) {
    $command = Get-Command mysql.exe -ErrorAction SilentlyContinue
    if ($command) { $MySqlExecutable = $command.Source }
    else {
        $MySqlExecutable = Join-Path $env:ProgramFiles 'MySQL\MySQL Server 8.0\bin\mysql.exe'
    }
}
$MySqlExecutable = (Resolve-Path -LiteralPath $MySqlExecutable).Path
$DatabaseConfiguration = (Resolve-Path -LiteralPath $DatabaseConfiguration).Path
# Read-only metadata only. Never print credentials, SQL data or account records.
$sql = @'
SELECT VERSION() AS DatabaseVersion, @@version_comment AS DatabaseEngine, DATABASE() AS SelectedDatabase;
SELECT CASE WHEN COUNT(*) = 1 THEN
    'AVAILABLE: per-constraint CHECK enforcement metadata'
    ELSE 'UNSUPPORTED: missing CHECK enforcement metadata; KR1304 requires MySQL 8.0.16+ (8.0.46 tested). Back up before upgrading/migrating. Do not add ENFORCED to game tables.'
    END AS KrCheckCompatibility
FROM information_schema.COLUMNS
WHERE TABLE_SCHEMA = 'information_schema' AND TABLE_NAME = 'TABLE_CONSTRAINTS'
    AND COLUMN_NAME = 'ENFORCED';
SELECT TABLE_NAME, COLUMN_NAME, COLUMN_TYPE
FROM information_schema.COLUMNS
WHERE TABLE_SCHEMA = DATABASE()
ORDER BY TABLE_NAME, ORDINAL_POSITION;
SELECT COLUMN_NAME AS ConstraintMetadataColumn
FROM information_schema.COLUMNS
WHERE TABLE_SCHEMA = 'information_schema' AND TABLE_NAME = 'TABLE_CONSTRAINTS'
ORDER BY ORDINAL_POSITION;
SELECT TABLE_NAME, CONSTRAINT_NAME, CONSTRAINT_TYPE
FROM information_schema.TABLE_CONSTRAINTS
WHERE CONSTRAINT_SCHEMA = DATABASE() AND CONSTRAINT_TYPE = 'CHECK'
ORDER BY TABLE_NAME, CONSTRAINT_NAME;
'@
$sql | & $MySqlExecutable "--defaults-extra-file=$DatabaseConfiguration" --batch --raw --default-character-set=utf8mb4
if ($LASTEXITCODE -ne 0) { throw 'Read-only database diagnostic failed; check the MySQL error above.' }
