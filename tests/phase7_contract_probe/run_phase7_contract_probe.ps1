param(
    [Parameter(Mandatory = $true)][string]$Godot451,
    [Parameter(Mandatory = $true)][string]$Godot472
)

$ErrorActionPreference = 'Stop'
$fixture = Split-Path -Parent $MyInvocation.MyCommand.Path
$expected = 'PHASE7_CONTRACT|signal_flag_combinations=1|key_identity_combinations=7'

foreach ($entry in @(@('4.5.1', $Godot451), @('4.7.2', $Godot472))) {
    $label = $entry[0]
    $executable = $entry[1]
    if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
        throw "Godot $label executable not found: $executable"
    }
    $temp = Join-Path ([IO.Path]::GetTempPath()) ("didi-phase7-contract-" + [guid]::NewGuid())
    New-Item -ItemType Directory -Path $temp | Out-Null
    try {
        Copy-Item -LiteralPath (Join-Path $fixture 'project.godot') -Destination $temp
        Copy-Item -LiteralPath (Join-Path $fixture 'probe.gd') -Destination $temp
        $output = & $executable --headless --path $temp --script res://probe.gd 2>&1
        if ($LASTEXITCODE -ne 0) {
            throw "Godot $label contract probe failed with exit $LASTEXITCODE`n$($output -join "`n")"
        }
        if (-not ($output -contains $expected)) {
            throw "Godot $label contract probe omitted expected evidence`n$($output -join "`n")"
        }
        Write-Output "ENGINE_CONTRACT|$label|signal_flag_combinations=1|key_identity_combinations=7"
    }
    finally {
        if (Test-Path -LiteralPath $temp) {
            Remove-Item -LiteralPath $temp -Recurse -Force
        }
    }
}
