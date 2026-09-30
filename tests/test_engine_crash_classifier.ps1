# Cases for the crash classifier the integration harness retries on.
#
# Run by run_godot_integration.ps1 before it starts an engine, so a classifier
# that has drifted fails in a second rather than after a twenty minute run, and
# fails on the classifier rather than on whatever it later mislabels.

. (Join-Path $PSScriptRoot "engine_crash_classifier.ps1")

$engineCrash = @(
    "DIDI CRASH CAPTURE",
    "exception: 0xc000001d  ILLEGAL_INSTRUCTION",
    "flags: 0x00000000  (0 is first chance, 1 is non continuable)",
    "address: 0x00007ff60b3144b6  Godot_v4.7.2-stable_win64.exe+0x066744b6",
    "process: 2508",
    "thread: 5496  main thread: 5892  on main thread: no",
    "stack:",
    "  #00 0x00007ff60b3144b6  Godot_v4.7.2-stable_win64.exe+0x066744b6",
    "  #01 0x00007ff606446266  Godot_v4.7.2-stable_win64.exe+0x017a6266",
    "  #02 0x00007ff6085c8abd  Godot_v4.7.2-stable_win64.exe+0x03928abd",
    "  #03 0x00007ff609994b1a  Godot_v4.7.2-stable_win64.exe+0x04cf4b1a",
    "  #04 0x00007ffb6d6bf0ad  msvcrt.dll+0x0003f0ad",
    "  #05 0x00007ffb6d6bf17c  msvcrt.dll+0x0003f17c",
    "  #06 0x00007ffb6dc4e8d7  KERNEL32.DLL+0x0002e8d7",
    "  #07 0x00007ffb6e4ec53c  ntdll.dll+0x0008c53c",
    "loaded modules:",
    "  0x00007ff604ca0000  size 0x0ae59000  D:\a\didi\godot-bin\Godot_v4.7.2-stable_win64.exe",
    "  0x00007ffb00000000  size 0x00001000  C:\p\addons\didi\bin\didi_extension.dll",
    "END DIDI CRASH CAPTURE"
) -join "`r`n"

# The 4.6.2 capture from #1086's run (#1098), with the module list its report
# carries. Its offsets were checked against the release binary.
$engineCrash462 = @(
    "DIDI CRASH CAPTURE",
    "exception: 0xc000001d  ILLEGAL_INSTRUCTION",
    "flags: 0x00000000  (0 is first chance, 1 is non continuable)",
    "address: 0x00007ff7f02b78a6  Godot_v4.6.2-stable_win64.exe+0x064678a6",
    "process: 8532",
    "thread: 1644  main thread: 7680  on main thread: no",
    "stack:",
    "  #00 0x00007ff7f02b78a6  Godot_v4.6.2-stable_win64.exe+0x064678a6",
    "  #01 0x00007ff7eb430d96  Godot_v4.6.2-stable_win64.exe+0x015e0d96",
    "  #02 0x00007ff7ed6520ad  Godot_v4.6.2-stable_win64.exe+0x038020ad",
    "  #03 0x00007ff7ee9fe2fa  Godot_v4.6.2-stable_win64.exe+0x04bae2fa",
    "  #04 0x00007ffc0baff0ad  msvcrt.dll+0x0003f0ad",
    "  #05 0x00007ffc0baff17c  msvcrt.dll+0x0003f17c",
    "  #06 0x00007ffc0b51e987  KERNEL32.DLL+0x0002e987",
    "  #07 0x00007ffc0bd2c53c  ntdll.dll+0x0008c53c",
    "loaded modules:",
    "  0x00007ff7e9e50000  size 0x0a623000  D:\a\didi\godot-bin\Godot_v4.6.2-stable_win64.exe",
    "  0x00007ffb00000000  size 0x00001000  C:\p\addons\didi\bin\didi_extension.dll",
    "END DIDI CRASH CAPTURE"
) -join "`r`n"

function New-Report([string]$Find, [string]$ReplaceWith) {
    return ($engineCrash -replace [regex]::Escape($Find), $ReplaceWith)
}

$didiFrame = "  #01 0x00007ff606446266  didi_extension.dll+0x017a6266"
$otherDll = "  #01 0x00007ff606446266  nvwgf2umx.dll+0x017a6266"
$noModule = "  #01 0x00007ff606446266  <address is in no loaded module>"
$engineFrame = "  #01 0x00007ff606446266  Godot_v4.7.2-stable_win64.exe+0x017a6266"

$cases = [ordered]@{
    # The one failure this is allowed to retry.
    "captured 4.7.2 documentation worker" = @{ report = $engineCrash; expect = $true }

    "another trap in the same engine" =
        @{ report = (New-Report '066744b6' '066744b8'); expect = $false }

    "same shared trap reached from another caller" =
        @{ report = (New-Report '017a6266' '017a6268'); expect = $false }

    "different worker entry" =
        @{ report = (New-Report '03928abd' '03928abf'); expect = $false }

    "unknown engine version with coincident offsets" =
        @{ report = (New-Report '4.7.2' '4.5.1'); expect = $false }

    "different engine image size" =
        @{ report = (New-Report '0ae59000' '0ae5a000'); expect = $false }

    "incomplete crash report" =
        @{ report = (New-Report 'END DIDI CRASH CAPTURE' ''); expect = $false }

    "multiple reports cannot be combined into a fingerprint" =
        @{ report = ($engineCrash + "`r`n" + $engineCrash); expect = $false }

    "missing worker frames" =
        @{ report = ($engineCrash -replace '(?m)^  #0[23].*\r?\n', ''); expect = $false }

    "ASLR changes absolute addresses but not the fingerprint" =
        @{ report = ($engineCrash -replace '0x00007ff6', '0x00006ff6'); expect = $true }

    # A frame of ours means the fault is ours to answer for.
    "a Didi frame on the stack" =
        @{ report = (New-Report $engineFrame $didiFrame); expect = $false }

    # An unrelated native module crashing is not #285, and retrying past it
    # would publish a green run over a real fault.
    "a frame in an unrelated module" =
        @{ report = (New-Report $engineFrame $otherDll); expect = $false }

    # Code in no loaded module is the least explicable of all.
    "a frame in no loaded module" =
        @{ report = (New-Report $engineFrame $noModule); expect = $false }

    # The main thread is the one the harness drives.
    "a fault on the main thread" =
        @{ report = (New-Report "on main thread: no" "on main thread: yes"); expect = $false }

    "a different exception" =
        @{ report = (New-Report "exception: 0xc000001d" "exception: 0xc0000005"); expect = $false }

    "no engine frame at all" =
        @{ report = (New-Report $engineFrame "  #01 0x00007ff606446266  ntdll.dll+0x017a6266" `
                     -replace [regex]::Escape("  #00 0x00007ff60b3144b6  Godot_v4.7.2-stable_win64.exe+0x066744b6"),
                               "  #00 0x00007ff60b3144b6  ntdll.dll+0x066744b6"); expect = $false }

    "captured 4.6.2 documentation worker" = @{ report = $engineCrash462; expect = $true }

    # Each build is held to its own offsets and image size, not another's.
    "4.6.2 offsets under the 4.7.2 name" =
        @{ report = ($engineCrash462 -replace '4\.6\.2', '4.7.2'); expect = $false }
    "4.6.2 with the 4.7.2 image size" =
        @{ report = ($engineCrash462 -replace '0a623000', '0ae59000'); expect = $false }
    "4.6.2 with a Didi frame on the stack" =
        @{ report = ($engineCrash462 -replace [regex]::Escape('Godot_v4.6.2-stable_win64.exe+0x015e0d96'), 'didi_extension.dll+0x015e0d96'); expect = $false }
    "4.6.2 on the main thread" =
        @{ report = ($engineCrash462 -replace 'on main thread: no', 'on main thread: yes'); expect = $false }

    "not a capture report at all" = @{ report = "some other file"; expect = $false }
    "nothing at all" = @{ report = ""; expect = $false }
}

$failures = 0
foreach ($name in $cases.Keys) {
    $got = Test-EngineWorkerCrashReport $cases[$name].report
    $want = $cases[$name].expect
    if ($got -ne $want) {
        $failures++
        Write-Output ("FAIL  {0}: retried={1} expected={2}" -f $name, $got, $want)
    }
}

if ($failures -ne 0) {
    throw "Engine crash classifier: $failures of $($cases.Count) cases wrong. The harness must not retry on a crash it cannot identify."
}
Write-Output "Engine crash classifier: $($cases.Count) cases correct."
