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
    "  #02 0x00007ffb6d6bf0ad  msvcrt.dll+0x0003f0ad",
    "  #03 0x00007ffb6dc4e8d7  KERNEL32.DLL+0x0002e8d7",
    "  #04 0x00007ffb6e4ec53c  ntdll.dll+0x0008c53c",
    "loaded modules:",
    "  0x00007ff604ca0000  size 0x0ae59000  D:\a\didi\godot-bin\Godot_v4.7.2-stable_win64.exe",
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
    "engine faulting in its own code" = @{ report = $engineCrash; expect = $true }

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
