# Keep retry evidence outside the godot_crash_* glob cleared by a new attempt.
function Save-EngineCrashArtifacts {
    param(
        [Parameter(Mandatory = $true)][string]$BuildRoot,
        [Parameter(Mandatory = $true)][ValidateRange(1, 2147483647)][int]$EnginePid,
        [Parameter(Mandatory = $true)][ValidateRange(1, 2147483647)][int]$Attempt
    )
    $root = [IO.Path]::GetFullPath($BuildRoot)
    $saved = @{ log = $null; dmp = $null }
    foreach ($extension in @('log', 'dmp')) {
        $source = Join-Path $root ("godot_crash_" + $EnginePid + "." + $extension)
        $destination = Join-Path $root ("godot_engine_worker_crash_attempt" + $Attempt + "_" + $EnginePid + "." + $extension)
        if (Test-Path -LiteralPath $source -PathType Leaf) {
            Copy-Item -LiteralPath $source -Destination $destination -ErrorAction Stop
            $saved[$extension] = $destination
        }
    }
    return [PSCustomObject]@{ ReportPath = $saved.log; DumpPath = $saved.dmp }
}
