# Decides whether a crash report is the engine faulting in its own code, which
# is the one failure the integration harness retries rather than reports.
#
# Kept in its own file, and taking report text rather than a process, so the
# cases in test_engine_crash_classifier.ps1 exercise the function the harness
# actually calls instead of a copy of it that can drift.

# Every frame has to be the engine binary or one of the system libraries that
# sit under any thread. A frame anywhere else, including one in no loaded module
# at all, means this is some other crash.
$script:EngineCrashSystemModules = @(
    'ntdll.dll', 'kernel32.dll', 'kernelbase.dll', 'msvcrt.dll', 'ucrtbase.dll'
)

function Test-EngineWorkerCrashReport([string]$Report) {
    if ([string]::IsNullOrWhiteSpace($Report)) { return $false }
    if ($Report -notmatch 'DIDI CRASH CAPTURE') { return $false }
    if ($Report -notmatch 'exception: 0xc000001d') { return $false }
    if ($Report -notmatch 'on main thread: no') { return $false }

    $stack = [regex]::Match($Report, '(?ms)^stack:\r?$(.*?)^loaded modules:')
    if (-not $stack.Success) { return $false }

    # A positive fingerprint, not merely the absence of one of our frames. The
    # first version accepted any illegal instruction off the main thread with no
    # Didi frame, which is also what an unrelated native crash in some other
    # module looks like. Retrying past one of those publishes a green run over a
    # real fault, which is worse than failing.
    $engineFrames = 0
    $frames = 0
    foreach ($line in ($stack.Groups[1].Value -split "`n")) {
        if ($line.Trim().Length -eq 0) { continue }
        $match = [regex]::Match($line, '^\s*#\d+\s+0x[0-9a-f]+\s+(.+?)\+0x[0-9a-f]+\s*$')
        if (-not $match.Success) { return $false }
        $frames++
        $module = $match.Groups[1].Value.Trim()
        if ($module -match '^Godot.*\.exe$') { $engineFrames++; continue }
        if ($script:EngineCrashSystemModules -contains $module.ToLowerInvariant()) { continue }
        return $false
    }
    if ($frames -eq 0 -or $engineFrames -eq 0) { return $false }

    # And it has to bottom out where a thread the C runtime started does. A
    # fault on a thread that began some other way is a different animal.
    return ($stack.Groups[1].Value -match 'ntdll\.dll')
}

# What this deliberately cannot do: tell #285 apart from a different crash in
# another engine worker thread. The offsets that would pin the frame move
# between engine versions, so there is nothing portable to match on. The retry
# is bounded for that reason.
