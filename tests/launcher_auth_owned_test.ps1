#Requires -Version 7.3
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'launcher_auth_owned.ps1')
$script:resolved = [Collections.Generic.List[int]]::new()
$script:stopped = [Collections.Generic.List[int]]::new()
$resolve = {
    param($OwnedPid)
    $script:resolved.Add($OwnedPid)
    $handle = [pscustomobject]@{ Id = $OwnedPid; HasExited = $false }
    $handle | Add-Member ScriptMethod Kill { $script:stopped.Add($this.Id); $this.HasExited = $true }
    $handle | Add-Member ScriptMethod WaitForExit { param($Milliseconds) return $true }
    $handle | Add-Member ScriptMethod Dispose {}
    return $handle
}
function Assert-True($Condition, $Message) { if (-not $Condition) { throw $Message } }
function New-FakeChild($Queue, [bool]$ForceKill) {
    $child = [pscustomobject]@{ HasExited = $false; Queue = $Queue; ForceKill = $ForceKill; Flushed = $false }
    $child | Add-Member ScriptMethod Kill {
        $this.HasExited = $true
        $this.Queue.Enqueue([pscustomobject]@{ Text = '{"event":"e2e-owned-spawn","pid":42}' })
    }
    $child | Add-Member ScriptMethod WaitForExit {
        param($Milliseconds)
        if ($null -eq $Milliseconds) {
            $this.Flushed = $true
            $this.Queue.Enqueue([pscustomobject]@{ Text = '{"event":"e2e-owned-spawn","pid":43}' })
            return
        }
        if (-not $this.ForceKill) { $this.HasExited = $true }
        return $this.HasExited
    }
    return $child
}
foreach ($scenario in @('late-normal-exit', 'early-input-error', 'forced-timeout', 'broken-cancel-pipe')) {
    $queue = [Collections.Concurrent.ConcurrentQueue[object]]::new()
    $capture = [pscustomobject]@{ Output = $queue }
    $owned = [Collections.Generic.Dictionary[int,object]]::new()
    $child = New-FakeChild $queue ($scenario -eq 'forced-timeout')
    $failed = $false
    try {
        Stop-KrOwnedElectron -Child $child -Capture $capture -OwnedClients $owned -ResolveProcess $resolve `
            -Cancel $(if ($scenario -eq 'broken-cancel-pipe') { { throw 'Expected controlled-pipe failure' } } else { $null }) `
            -GraceMilliseconds $(if ($scenario -eq 'forced-timeout') { 0 } else { 200 })
    } catch { $failed = $true }
    Assert-True $child.Flushed "$scenario did not perform final output flush"
    Assert-True ($owned.ContainsKey(43)) "$scenario lost final-flush spawn event"
    if ($scenario -eq 'forced-timeout') {
        Assert-True $failed 'Forced termination must report the unresolved native-spawn boundary'
        Assert-True ($owned.ContainsKey(42)) 'Timeout lost spawn published during termination'
    } elseif ($scenario -eq 'broken-cancel-pipe') { Assert-True $failed 'Broken cancellation must report failure after final drain' }
    else { Assert-True (-not $failed) "$scenario unexpectedly failed" }
    Close-KrOwnedClients -OwnedClients $owned
    Assert-True ($script:stopped.Contains(43)) "$scenario did not kill the late owned client"
}
$owned = [Collections.Generic.Dictionary[int,object]]::new()
$queue = [Collections.Concurrent.ConcurrentQueue[object]]::new()
$queue.Enqueue([pscustomobject]@{ Text = '{"event":"e2e-owned-spawn","pid":46}' })
$script:acknowledgments = 0
foreach ($iteration in 1..2) {
    Receive-KrOwnedSpawns -Capture ([pscustomobject]@{Output=$queue}) -OwnedClients $owned -ResolveProcess $resolve -Acknowledge { param($OwnedPid) $script:acknowledgments++ }
}
Assert-True ($script:acknowledgments -eq 1) 'Snapshot re-read repeated the ownership acknowledgment'
Close-KrOwnedClients -OwnedClients $owned
$owned = [Collections.Generic.Dictionary[int,object]]::new()
$bad = & $resolve 44
$bad | Add-Member ScriptMethod Kill { throw 'Expected fake kill failure' } -Force
$owned.Add(44, $bad)
$owned.Add(45, (& $resolve 45))
$failed = $false
try { Close-KrOwnedClients -OwnedClients $owned } catch { $failed = $true }
Assert-True $failed 'Owned cleanup failure was hidden'
Assert-True ($script:stopped.Contains(45)) 'One failure prevented independent cleanup'
Write-Output 'PASS: 6 ownership cleanup scenarios; only published fake PIDs resolved.'
