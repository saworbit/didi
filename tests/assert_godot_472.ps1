function Assert-Godot472Executable {
    param([Parameter(Mandatory = $true)][string]$Executable)

    if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) {
        throw "Godot 4.7.2 executable not found: $Executable"
    }
    $resolved = [IO.Path]::GetFullPath($Executable)
    $versionOutput = @(& $resolved --version 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "Godot version check failed with exit $LASTEXITCODE`: $($versionOutput -join ' ')"
    }
    $versionText = ($versionOutput -join "`n").Trim()
    if ($versionText -notmatch '(?m)^4\.7\.2(?:[.-]|$)') {
        throw "Current verification requires Godot 4.7.2; reported: $versionText"
    }
    return $resolved
}
