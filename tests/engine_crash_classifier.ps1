# Recognizes the captured #285 documentation-worker stack. An engine-only
# stack is not enough: other fatal checks can also raise illegal instructions.
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
    if ([regex]::Matches($Report, '(?m)^DIDI CRASH CAPTURE\r?$').Count -ne 1) { return $false }
    if ($Report -notmatch '(?m)^DIDI CRASH CAPTURE\r?$') { return $false }
    if ($Report -notmatch '(?m)^END DIDI CRASH CAPTURE\r?$') { return $false }
    if ($Report -notmatch '(?m)^exception: 0xc000001d\s+ILLEGAL_INSTRUCTION\r?$') { return $false }
    if ($Report -notmatch '(?m)^thread: \d+  main thread: \d+  on main thread: no\r?$') { return $false }

    # These four RVAs and image size come from run 34089765718, job
    # 101640706240, recorded in #285. ASLR changes absolute addresses, not RVAs.
    # An unrecognized release must fail normally until its stack is verified.
    $engineModule = 'Godot_v4.7.2-stable_win64.exe'
    $workerOffsets = @('066744b6', '017a6266', '03928abd', '04cf4b1a')
    if ($Report -notmatch '(?m)^address: 0x[0-9a-f]+\s+Godot_v4\.7\.2-stable_win64\.exe\+0x066744b6\r?$') { return $false }
    $modules = [regex]::Match($Report, '(?ms)^loaded modules:\r?$(.*?)^END DIDI CRASH CAPTURE\r?$')
    if (-not $modules.Success -or $modules.Groups[1].Value -notmatch '(?m)^\s+0x[0-9a-f]+\s+size 0x0ae59000\s+[^\r\n]*[\\/]Godot_v4\.7\.2-stable_win64\.exe\r?$') { return $false }

    $stack = [regex]::Match($Report, '(?ms)^stack:\r?$(.*?)^loaded modules:')
    if (-not $stack.Success) { return $false }

    $frames = 0
    foreach ($line in ($stack.Groups[1].Value -split "`n")) {
        if ($line.Trim().Length -eq 0) { continue }
        $match = [regex]::Match($line, '^\s*#(\d+)\s+0x[0-9a-f]+\s+(.+?)\+0x([0-9a-f]+)\s*$', 'IgnoreCase')
        if (-not $match.Success) { return $false }
        if ([int]$match.Groups[1].Value -ne $frames) { return $false }
        $module = $match.Groups[2].Value.Trim()
        $offset = $match.Groups[3].Value
        if ($frames -lt $workerOffsets.Count) {
            if ($module -ne $engineModule -or $offset -ne $workerOffsets[$frames]) { return $false }
            $frames++
            continue
        }
        $frames++
        if ($script:EngineCrashSystemModules -contains $module.ToLowerInvariant()) { continue }
        return $false
    }
    if ($frames -le $workerOffsets.Count) { return $false }

    # And it has to bottom out where a thread the C runtime started does. A
    # fault on a thread that began some other way is a different animal.
    return $module -eq 'ntdll.dll'
}

# This identifies the observed failure path, not its cause. A Didi action can
# still trigger an engine race without appearing on the faulting worker stack.
