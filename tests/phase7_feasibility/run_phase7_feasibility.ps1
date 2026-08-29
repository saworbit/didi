[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Godot451,

    [Parameter(Mandatory = $true)]
    [string]$Godot472
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ExpectedTools = @(
    "signal_list_connections",
    "signal_connect",
    "signal_disconnect",
    "signal_emit",
    "viewport_set_camera_transform",
    "viewport_toggle_debug_draw",
    "tilemap_set_cells",
    "tilemap_get_used_rect",
    "gridmap_set_cells",
    "physics_raycast_query",
    "physics_simulate_step",
    "nav_bake_mesh",
    "nav_query_path",
    "anim_list_tracks",
    "anim_play_track",
    "runtime_inject_input",
    "runtime_get_call_stack",
    "runtime_read_profiler"
)
$ExpectedBlocked = @(
    "nav_bake_mesh",
    "physics_simulate_step",
    "runtime_get_call_stack"
)
$Utf8NoBom = [System.Text.UTF8Encoding]::new($false)
$RepositoryRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $PSScriptRoot "..\..")
)
$OutputRoot = Join-Path $RepositoryRoot "build\phase7-feasibility"
[void][System.IO.Directory]::CreateDirectory($OutputRoot)

function Assert-Executable {
    param(
        [string]$Label,
        [string]$Path
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Label executable does not exist: $Path"
    }
}

function Invoke-Phase7EngineProbe {
    param(
        [string]$Version,
        [string]$Executable
    )

    $VersionRoot = Join-Path $OutputRoot $Version
    $ProjectRoot = Join-Path $VersionRoot "project"
    [void][System.IO.Directory]::CreateDirectory($ProjectRoot)
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot "project.godot") -Destination $ProjectRoot -Force
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot "probe.gd") -Destination $ProjectRoot -Force

    $StdoutPath = Join-Path $VersionRoot "probe.stdout.log"
    $StderrPath = Join-Path $VersionRoot "probe.stderr.log"
    $EngineLogPath = Join-Path $VersionRoot "godot.log"
    $ResultPath = Join-Path $VersionRoot "phase7-results.json"
    $ProbePath = Join-Path $ProjectRoot "probe.gd"

    $StartInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $StartInfo.FileName = [System.IO.Path]::GetFullPath($Executable)
    $StartInfo.WorkingDirectory = $VersionRoot
    $StartInfo.UseShellExecute = $false
    $StartInfo.RedirectStandardOutput = $true
    $StartInfo.RedirectStandardError = $true
    foreach ($Argument in @(
        "--headless",
        "--log-file",
        $EngineLogPath,
        "--path",
        $ProjectRoot,
        "--script",
        $ProbePath
    )) {
        [void]$StartInfo.ArgumentList.Add($Argument)
    }

    $Stdout = ""
    $Stderr = ""
    $ExitCode = -1
    try {
        $Process = [System.Diagnostics.Process]::new()
        $Process.StartInfo = $StartInfo
        [void]$Process.Start()
        $StdoutTask = $Process.StandardOutput.ReadToEndAsync()
        $StderrTask = $Process.StandardError.ReadToEndAsync()
        $Process.WaitForExit()
        $Stdout = $StdoutTask.GetAwaiter().GetResult()
        $Stderr = $StderrTask.GetAwaiter().GetResult()
        $ExitCode = $Process.ExitCode
        $Process.Dispose()
    }
    catch {
        $Stderr = $_.Exception.ToString()
    }

    [System.IO.File]::WriteAllText($StdoutPath, $Stdout, $Utf8NoBom)
    [System.IO.File]::WriteAllText($StderrPath, $Stderr, $Utf8NoBom)

    $Errors = [System.Collections.Generic.List[string]]::new()
    $Rows = [System.Collections.Generic.List[object]]::new()
    $EngineVersions = [System.Collections.Generic.List[string]]::new()
    foreach ($Line in ($Stdout -split "\r?\n")) {
        $EnginePrefixIndex = $Line.IndexOf(
            "PHASE7_ENGINE|",
            [System.StringComparison]::Ordinal
        )
        if ($EnginePrefixIndex -ge 0) {
            $EngineJson = $Line.Substring($EnginePrefixIndex + "PHASE7_ENGINE|".Length)
            try {
                $EnginePayload = $EngineJson | ConvertFrom-Json -Depth 20
                [void]$EngineVersions.Add(
                    "$($EnginePayload.major).$($EnginePayload.minor).$($EnginePayload.patch)"
                )
            }
            catch {
                [void]$Errors.Add("$Version malformed PHASE7_ENGINE row: $EngineJson")
            }
            continue
        }
        $PrefixIndex = $Line.IndexOf("PHASE7_RESULT|", [System.StringComparison]::Ordinal)
        if ($PrefixIndex -lt 0) {
            continue
        }
        $JsonText = $Line.Substring($PrefixIndex + "PHASE7_RESULT|".Length)
        try {
            $Payload = $JsonText | ConvertFrom-Json -Depth 100
            $Decision = if ([bool]$Payload.ok) { "GO" } else { "BLOCKED" }
            [void]$Rows.Add(
                [pscustomobject]@{
                    Tool = [string]$Payload.tool
                    Decision = $Decision
                    Evidence = $Payload.evidence
                }
            )
        }
        catch {
            [void]$Errors.Add("$Version malformed PHASE7_RESULT row: $JsonText")
        }
    }

    $DistinctTools = @($Rows | ForEach-Object { $_.Tool } | Sort-Object -Unique)
    $GoCount = @($Rows | Where-Object { $_.Decision -eq "GO" }).Count
    $BlockedTools = @(
        $Rows |
            Where-Object { $_.Decision -eq "BLOCKED" } |
            ForEach-Object { $_.Tool } |
            Sort-Object
    )
    $ExpectedSorted = @($ExpectedTools | Sort-Object)
    $ToolSetDifference = @(
        Compare-Object -ReferenceObject $ExpectedSorted -DifferenceObject $DistinctTools
    )
    $BlockedDifference = @(
        Compare-Object -ReferenceObject $ExpectedBlocked -DifferenceObject $BlockedTools
    )

    if ($ExitCode -ne 0) {
        [void]$Errors.Add("$Version exited $ExitCode")
    }
    if ($EngineVersions.Count -ne 1 -or $EngineVersions[0] -ne $Version) {
        [void]$Errors.Add(
            "$Version engine identity was $($EngineVersions -join ',') instead of $Version"
        )
    }
    if ($Rows.Count -ne 18) {
        [void]$Errors.Add("$Version emitted $($Rows.Count) rows instead of 18")
    }
    if ($DistinctTools.Count -ne 18) {
        [void]$Errors.Add("$Version emitted $($DistinctTools.Count) distinct tools instead of 18")
    }
    if ($ToolSetDifference.Count -ne 0) {
        [void]$Errors.Add("$Version tool set differs from the exact Phase 7 set")
    }
    if ($GoCount -ne 15 -or $BlockedTools.Count -ne 3) {
        [void]$Errors.Add(
            "$Version decisions were $GoCount GO and $($BlockedTools.Count) BLOCKED instead of 15/3"
        )
    }
    if ($BlockedDifference.Count -ne 0) {
        [void]$Errors.Add(
            "$Version blocker set was $($BlockedTools -join ',') instead of $($ExpectedBlocked -join ',')"
        )
    }

    $NormalizedRows = @(
        $Rows |
            Sort-Object Tool |
            ForEach-Object {
                [ordered]@{
                    tool = $_.Tool
                    decision = $_.Decision
                    evidence = $_.Evidence
                }
            }
    )
    $ResultJson = ConvertTo-Json -InputObject $NormalizedRows -Compress -Depth 100
    [System.IO.File]::WriteAllText($ResultPath, $ResultJson + [Environment]::NewLine, $Utf8NoBom)
    $ResultHash = (Get-FileHash -LiteralPath $ResultPath -Algorithm SHA256).Hash.ToLowerInvariant()

    $AuditLines = @(
        "ENGINE_EXIT|$Version|$ExitCode",
        "ENGINE_VERSION|$Version|$($EngineVersions -join ',')",
        "ROW_ASSERT|$Version|rows=$($Rows.Count)|distinct=$($DistinctTools.Count)|go=$GoCount|blocked=$($BlockedTools.Count)",
        "BLOCKED|$Version|$($BlockedTools -join ',')",
        "RESULT_SHA256|$Version|$ResultHash"
    )
    return [pscustomobject]@{
        AuditLines = $AuditLines
        Errors = @($Errors)
    }
}

Assert-Executable -Label "Godot 4.5.1" -Path $Godot451
Assert-Executable -Label "Godot 4.7.2" -Path $Godot472

$Results = @(
    Invoke-Phase7EngineProbe -Version "4.5.1" -Executable $Godot451
    Invoke-Phase7EngineProbe -Version "4.7.2" -Executable $Godot472
)
$AuditLines = @($Results | ForEach-Object { $_.AuditLines })
$Errors = @($Results | ForEach-Object { $_.Errors })
$AuditPath = Join-Path $OutputRoot "audit.txt"
[System.IO.File]::WriteAllLines($AuditPath, $AuditLines, $Utf8NoBom)
$AuditLines | Write-Output

if ($Errors.Count -ne 0) {
    throw (
        "Phase 7 feasibility audit failed:" +
        [Environment]::NewLine +
        ($Errors -join [Environment]::NewLine)
    )
}
