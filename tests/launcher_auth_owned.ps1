function Receive-KrOwnedSpawns {
    param($Capture, $OwnedClients, [scriptblock]$Acknowledge, [scriptblock]$ResolveProcess = { param($OwnedPid) [Diagnostics.Process]::GetProcessById($OwnedPid) })
    if (-not $Capture) { return }
    foreach ($record in $Capture.Output.ToArray()) {
        if ($record.Text -notmatch '^\{"event":"e2e-owned-spawn","pid":([1-9]\d*)\}$') { continue }
        $ownedPid = [int]$Matches[1]
        if (-not $OwnedClients.ContainsKey($ownedPid)) {
            try {
                $handle = & $ResolveProcess $ownedPid
                # Retain the native handle before acknowledging; do not reopen this PID later.
                if ($handle -is [Diagnostics.Process]) { $null = $handle.Handle }
                $OwnedClients.Add($ownedPid, $handle)
            } catch [ArgumentException] { continue }
            if ($Acknowledge) { & $Acknowledge $ownedPid }
        }
    }
}
function Stop-KrOwnedElectron {
    param($Child, $Capture, $OwnedClients, [scriptblock]$Cancel, [scriptblock]$ResolveProcess = { param($OwnedPid) [Diagnostics.Process]::GetProcessById($OwnedPid) }, [int]$GraceMilliseconds = 15000)
    if (-not $Child) { return }
    $forced = $false
    $failed = $false
    try {
        if (-not $Child.HasExited) {
            if ($Cancel) { try { & $Cancel } catch { $failed = $true } }
            $deadline = [DateTime]::UtcNow.AddMilliseconds($GraceMilliseconds)
            while (-not $Child.HasExited -and [DateTime]::UtcNow -lt $deadline) {
                try { Receive-KrOwnedSpawns -Capture $Capture -OwnedClients $OwnedClients -ResolveProcess $ResolveProcess } catch { $failed = $true }
                $null = $Child.WaitForExit(50)
            }
            if (-not $Child.HasExited) {
                $forced = $true
                $Child.Kill()
                if (-not $Child.WaitForExit(10000)) { throw 'Owned Electron termination timed out.' }
            }
        }
        $Child.WaitForExit()
    }
    finally {
        try { Receive-KrOwnedSpawns -Capture $Capture -OwnedClients $OwnedClients -ResolveProcess $ResolveProcess } catch { $failed = $true }
    }
    if ($forced) { throw 'Owned Electron forced termination; an unresolved native spawn may require manual isolated-test inspection.' }
    if ($failed) { throw 'Owned Electron control or final ownership drain failed.' }
}
function Close-KrOwnedClients {
    param($OwnedClients)
    $failed = $false
    foreach ($handle in $OwnedClients.Values) {
        try {
            if (-not $handle.HasExited) {
                $handle.Kill()
                if (-not $handle.WaitForExit(10000)) { throw 'Owned client termination timed out.' }
            }
        } catch { $failed = $true }
        finally { try { $handle.Dispose() } catch { $failed = $true } }
    }
    if ($failed) { throw 'One or more registered owned clients could not be cleaned.' }
}
