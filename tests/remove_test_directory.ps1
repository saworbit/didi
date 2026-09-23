# Removes a directory a test is about to rebuild, and fails when it cannot.
#
# An editor that has just been stopped leaves its copy of the extension,
# addons/didi/bin/~didi_extension.dll, locked for a moment after the process
# has exited. These removals used to be -ErrorAction SilentlyContinue, so a
# fixture rebuilt straight after an editor run was not removed and nobody was
# told. Copy-Item then put the fresh copy inside the old folder instead of in
# its place. On Windows PowerShell the old fixture survived whole and the next
# editor ran against it; on CI's PowerShell 7 the editor that followed opened a
# folder with no project in it and never published a session ("Run 1 did not
# publish an editor descriptor", 4.5.1, twice on 2026-09-23). Measured locally:
# the lock is there on every seam-then-production signal bridge run.
function Remove-TestDirectory {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [int]$TimeoutSeconds = 30
    )
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while (Test-Path -LiteralPath $Path) {
        try {
            Remove-Item -LiteralPath $Path -Recurse -Force -ErrorAction Stop
        }
        catch {
            if ([DateTime]::UtcNow -ge $deadline) {
                throw "Could not remove $Path within $TimeoutSeconds seconds: $($_.Exception.Message)"
            }
            Start-Sleep -Milliseconds 250
        }
    }
}
