# Verifies a Godot executable meets Didi's documented minimum engine version.
#
# This replaces an earlier assertion that demanded exactly 4.7.2. That
# requirement came from the Godot 4.7 migration, which PR #86 ruled is not
# authorized by the Phase 7 decision: the feasibility gate records the same
# 15 GO / 3 BLOCKED result on Godot 4.5.1 and 4.7.2, so Phase 7 evidence must
# be obtainable on the supported floor rather than only on the newest engine.

Set-StrictMode -Version Latest

$script:DidiMinimumGodotMajor = 4
$script:DidiMinimumGodotMinor = 5

function Assert-SupportedGodotExecutable {
    param([Parameter(Mandatory = $true)][string]$Executable)

    if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) {
        throw "Godot executable not found: $Executable"
    }
    $resolved = [IO.Path]::GetFullPath($Executable)
    $versionOutput = @(& $resolved --version 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "Godot version check failed with exit $LASTEXITCODE`: $($versionOutput -join ' ')"
    }
    $versionText = ($versionOutput -join "`n").Trim()
    if ($versionText -notmatch '(?m)^(?<major>\d+)\.(?<minor>\d+)(?:\.(?<patch>\d+))?') {
        throw "Could not read a Godot version from: $versionText"
    }
    $major = [int]$Matches['major']
    $minor = [int]$Matches['minor']
    $supported = ($major -gt $script:DidiMinimumGodotMajor) -or
                 (($major -eq $script:DidiMinimumGodotMajor) -and
                  ($minor -ge $script:DidiMinimumGodotMinor))
    if (-not $supported) {
        throw ("Didi requires Godot " +
               "$script:DidiMinimumGodotMajor.$script:DidiMinimumGodotMinor or newer; reported: $versionText")
    }
    # Publish the engine that actually ran so evidence records the real version
    # rather than a hard-coded one.
    $patch = if ($Matches.ContainsKey('patch') -and $Matches['patch']) { $Matches['patch'] } else { '0' }
    $script:DidiDetectedGodotVersion = "$major.$minor.$patch"
    return $resolved
}
