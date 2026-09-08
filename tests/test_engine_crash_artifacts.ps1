# Run the production preservation function, then simulate the next attempt's
# cleanup. Losing the dump on retry must fail even when the text log survives.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'engine_crash_artifacts.ps1')

$testRoot = Join-Path (Split-Path -Parent $PSScriptRoot) ('build/crash-artifacts-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot -Force | Out-Null
try {
    $report = Join-Path $testRoot 'godot_crash_123.log'
    $dump = Join-Path $testRoot 'godot_crash_123.dmp'
    [IO.File]::WriteAllText($report, 'first attempt report')
    [byte[]]$dumpBytes = @(77, 68, 77, 80, 0, 255, 128, 1)
    [IO.File]::WriteAllBytes($dump, $dumpBytes)
    $saved = Save-EngineCrashArtifacts -BuildRoot $testRoot -EnginePid 123 -Attempt 1
    Remove-Item -Path (Join-Path $testRoot 'godot_crash_*.log'), (Join-Path $testRoot 'godot_crash_*.dmp') -Force
    if ([IO.File]::ReadAllText($saved.ReportPath) -ne 'first attempt report') { throw 'Report lost on retry' }
    if ([string]::IsNullOrEmpty($saved.DumpPath)) { throw 'Minidump was not preserved before retry cleanup' }
    if ([Convert]::ToBase64String([IO.File]::ReadAllBytes($saved.DumpPath)) -ne [Convert]::ToBase64String($dumpBytes)) {
        throw 'Minidump lost or modified on retry'
    }
    [IO.File]::WriteAllText($report, 'second attempt report')
    $second = Save-EngineCrashArtifacts -BuildRoot $testRoot -EnginePid 123 -Attempt 2
    if ($second.ReportPath -eq $saved.ReportPath) { throw 'Attempts share an artifact path' }
    if ($null -ne $second.DumpPath) { throw 'Missing minidump was reported as saved' }
    if ([IO.File]::ReadAllText($saved.ReportPath) -ne 'first attempt report') { throw 'Prior report overwritten' }
    if ([IO.File]::ReadAllText($second.ReportPath) -ne 'second attempt report') { throw 'Second report not preserved' }
    Write-Output 'Engine crash artifacts: report and binary dump survive retry cleanup; attempts remain separate.'
}
finally {
    $resolvedRoot = [IO.Path]::GetFullPath($testRoot)
    $expectedParent = [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $PSScriptRoot) 'build')) + [IO.Path]::DirectorySeparatorChar
    if (-not $resolvedRoot.StartsWith($expectedParent, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe test cleanup path' }
    Remove-Item -LiteralPath $resolvedRoot -Recurse -Force
}
