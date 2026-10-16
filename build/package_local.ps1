[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$OutputDirectory,
    [Parameter(Mandatory)][string]$CertificatePath,
    [Parameter(Mandatory)][string]$MySqlRoot,
    [Parameter(Mandatory)][string]$RuntimeDirectory,
    [string]$DatabaseConfiguration,
    [string]$OperationsConfiguration
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path $PSScriptRoot -Parent
$binaryRoot = Join-Path $repositoryRoot 'Artifacts\Bin\x64\Release'
if (!$DatabaseConfiguration) { $DatabaseConfiguration = Join-Path $repositoryRoot 'Local\database.ini' }
if (!$OperationsConfiguration) { $OperationsConfiguration = Join-Path $repositoryRoot 'Local\operations.json' }
$output = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $output) { throw 'Use a new output directory to preserve existing packages.' }
$publicCertificate = [Security.Cryptography.X509Certificates.X509Certificate2]::new(
    [IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $CertificatePath).Path))
try {
    $thumbprint = $publicCertificate.Thumbprint
    if ($publicCertificate.HasPrivateKey -or $publicCertificate.NotAfter -le (Get-Date)) {
        throw 'A valid public certificate is required.'
    }
} finally { $publicCertificate.Dispose() }
$required = @(
    $DatabaseConfiguration, $OperationsConfiguration,
    (Join-Path $repositoryRoot 'webgm\index.html'),
    (Join-Path $repositoryRoot 'webgm\app.js'),
    (Join-Path $repositoryRoot 'webgm\style.css')
)
$programFiles = @('BubbleFighter.Server.exe', 'libmysql.dll', 'libssl-3-x64.dll', 'libcrypto-3-x64.dll')
foreach ($file in $programFiles) { $required += Join-Path $binaryRoot $file }
foreach ($file in @('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')) {
    $required += Join-Path $RuntimeDirectory $file
}
foreach ($file in @('LICENSE', 'README', 'docs\INFO_BIN', 'docs\INFO_SRC')) {
    $required += Join-Path $MySqlRoot $file
}
foreach ($file in $required) {
    if (!(Test-Path -LiteralPath $file -PathType Leaf)) { throw "Required package input is missing: $file" }
}
$operations = Get-Content -LiteralPath $OperationsConfiguration -Raw | ConvertFrom-Json
$operations.gm_address = '127.0.0.1'
$operations.web_root = '../webgm'
$settings = Get-Content -LiteralPath (Join-Path $repositoryRoot 'config\examples\server.json') -Raw | ConvertFrom-Json
$settings.auth_cert_thumbprint = $thumbprint
$migrations = @(
    'create_accounts', 'create_characters', 'add_character_statistics', 'create_inventory_slots',
    'create_cash_inventory_slots', 'create_weapon_presets', 'expand_cash_inventory_usage',
    'create_match_settlements', 'add_weapon_preset_equipment', 'create_social_relationships',
    'create_clans', 'create_clan_creation_receipts', 'expand_clan_recruitment',
    'create_clan_disband_notices', 'create_training_clan_history', 'create_friend_groups',
    'create_social_messages', 'create_cash_purchases', 'create_gifts', 'create_cash_bundles',
    'create_gift_bundles', 'add_battle_records', 'expand_character_name_width', 'create_operations'
)
foreach ($migration in $migrations) {
    if (!(Test-Path -LiteralPath (Join-Path $repositoryRoot "src\persistence\migrations\$migration.sql"))) {
        throw "Migration is missing: $migration"
    }
}
$ports = @($settings.game_port, $settings.auth_port, $settings.clan_port, $operations.gm_port)
if (@($ports | Select-Object -Unique).Count -ne 4) { throw 'Packaged server ports must be distinct.' }
$configuration = Join-Path $output 'config'
$migrationRoot = Join-Path $output 'database\migrations'
$licenses = Join-Path $output 'licenses'
New-Item -ItemType Directory -Force -Path $configuration, $migrationRoot, $licenses | Out-Null
$utf8 = [Text.UTF8Encoding]::new($false)
foreach ($file in $programFiles) { Copy-Item -LiteralPath (Join-Path $binaryRoot $file) -Destination $output }
Get-ChildItem -LiteralPath $RuntimeDirectory -Filter '*.dll' -File |
    ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $output }
Get-ChildItem -LiteralPath (Join-Path $repositoryRoot 'config\examples') -Filter '*.ini' -File |
    ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $configuration }
Copy-Item -LiteralPath $DatabaseConfiguration -Destination (Join-Path $configuration 'database.ini') -Force
Copy-Item -LiteralPath $CertificatePath -Destination (Join-Path $configuration 'auth-server.cer')
[IO.File]::WriteAllText((Join-Path $configuration 'server.json'), ($settings | ConvertTo-Json), $utf8)
[IO.File]::WriteAllText((Join-Path $configuration 'operations.json'), ($operations | ConvertTo-Json -Depth 20), $utf8)
New-Item -ItemType Directory -Path (Join-Path $output 'data') -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'data\shop') -Destination (Join-Path $output 'data\shop') -Recurse
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'webgm') -Destination (Join-Path $output 'webgm') -Recurse
Get-ChildItem -LiteralPath (Join-Path $repositoryRoot 'src\persistence\migrations') -Filter '*.sql' -File |
    ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $migrationRoot }
[IO.File]::WriteAllText((Join-Path $output 'database\migration-order.json'), ($migrations | ConvertTo-Json), $utf8)
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'scripts\Start-KrServer.ps1') -Destination (Join-Path $output 'Start-Server.ps1')
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'scripts\Test-KrDatabase.ps1') -Destination $output
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'scripts\Start-KrServer.cmd') -Destination (Join-Path $output 'Start-Server.cmd')
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'scripts\New-KrAuthCertificate.ps1') -Destination $output
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'LICENSE') -Destination $output
foreach ($file in @('httplib.LICENSE', 'json.LICENSE.MIT')) {
    Copy-Item -LiteralPath (Join-Path $repositoryRoot "Artifacts\Dependencies\web\$file") -Destination $licenses
}
$mysqlLicense = Join-Path $licenses 'mysql'
New-Item -ItemType Directory -Path $mysqlLicense -Force | Out-Null
foreach ($file in @('LICENSE', 'README', 'docs\INFO_BIN', 'docs\INFO_SRC')) {
    Copy-Item -LiteralPath (Join-Path $MySqlRoot $file) -Destination (Join-Path $mysqlLicense (Split-Path $file -Leaf))
}
Write-Output "Local server package: $output"
