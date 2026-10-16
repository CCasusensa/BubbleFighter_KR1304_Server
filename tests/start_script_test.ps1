Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$tokens = $null
$errors = $null
$path = Join-Path $PSScriptRoot '../scripts/Start-KrServer.ps1'
$ast = [Management.Automation.Language.Parser]::ParseFile($path, [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw 'Startup script syntax error.' }
$start = $ast.Find({ param($node)
    $node -is [Management.Automation.Language.AssignmentStatementAst] -and
    $node.Left.Extent.Text -eq '$ports'
}, $true).Extent.StartOffset
$end = $ast.Find({ param($node)
    $node -is [Management.Automation.Language.AssignmentStatementAst] -and
    $node.Left.Extent.Text -eq '$publicCertificate'
}, $true).Extent.StartOffset
$validation = [scriptblock]::Create([IO.File]::ReadAllText($path).Substring($start, $end - $start))
$script:occupied = $false
function Get-NetTCPConnection { param($State, $LocalPort, $ErrorAction)
    if ($script:occupied) { [pscustomobject]@{ LocalPort = $LocalPort } }
}
function Check-Ports($Game, $Auth, $Clan, $Gm, [bool]$Accepted) {
    $settings = [pscustomobject]@{ game_port = $Game; auth_port = $Auth; clan_port = $Clan }
    $operations = [pscustomobject]@{ gm_port = $Gm }
    $passed = $true
    try { & $validation } catch { $passed = $false }
    if ($passed -ne $Accepted) { throw "Unexpected port validation (accepted=$passed)." }
}
$config = '{"game":7201,"auth":7200,"clan":7202,"gm":21582}' | ConvertFrom-Json
Check-Ports $config.game $config.auth $config.clan $config.gm $true
Check-Ports ([long]7201) ([int]7200) ([long]7202) ([int]21582) $true
Check-Ports 1 2 3 65535 $true
foreach ($invalid in @($null, $true, '7201', 7201.5, 0, -1, 65536, [long]::MaxValue)) {
    Check-Ports $invalid 7200 7202 21582 $false
}
Check-Ports 7200 7200 7202 21582 $false
Check-Ports 7201 7200 7202 7201 $false
$script:occupied = $true
Check-Ports 7201 7200 7202 21582 $false
Write-Output "14 startup port checks passed ($($PSVersionTable.PSVersion))."
