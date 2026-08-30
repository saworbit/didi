[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$GodotExecutable,
    [Parameter(Mandatory = $true)][string]$ExtensionLibrary,
    [Parameter(Mandatory = $true)][string]$ProbeExecutable,
    # Trial the shipping binary. The seam scenarios need a method that is
    # compiled out of the production extension, so they are skipped.
    [switch]$Production,
    [int]$Repeat = 2
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot 'assert_godot_supported.ps1')
$GodotExecutable = Assert-SupportedGodotExecutable -Executable $GodotExecutable

$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$source = Join-Path $PSScriptRoot 'phase7_signal_bridge'
$fixture = Join-Path $root 'build\phase7_signal_bridge'
$sessionRoot = Join-Path $root 'build\phase7_signal_sessions'
if (-not $fixture.StartsWith((Join-Path $root 'build') + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe fixture path' }
Remove-Item -LiteralPath $fixture -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $sessionRoot -Recurse -Force -ErrorAction SilentlyContinue
Copy-Item -LiteralPath $source -Destination $fixture -Recurse
Copy-Item -LiteralPath (Join-Path $root 'addons\didi') -Destination (Join-Path $fixture 'addons\didi') -Recurse
New-Item -ItemType Directory -Path (Join-Path $fixture 'addons\didi\bin') -Force | Out-Null
Copy-Item -LiteralPath $ExtensionLibrary -Destination (Join-Path $fixture 'addons\didi\bin\didi_extension.dll') -Force

for ($run = 1; $run -le $Repeat; $run++) {
    $sessions = Join-Path $sessionRoot "run-$run"
    New-Item -ItemType Directory -Path $sessions -Force | Out-Null
    $stdout = Join-Path $sessions 'godot.stdout.log'
    $stderr = Join-Path $sessions 'godot.stderr.log'
    $engineLog = Join-Path $sessions 'godot.log'
    $previous = $env:DIDI_SESSION_DIR
    $env:DIDI_SESSION_DIR = $sessions
    $process = $null
    try {
        $process = Start-Process -FilePath $GodotExecutable -WindowStyle Hidden -PassThru `
            -ArgumentList @('--headless', '--editor', '--path', $fixture, '--log-file', $engineLog,
                            'res://main.tscn') `
            -RedirectStandardOutput $stdout -RedirectStandardError $stderr
        $deadline = [DateTime]::UtcNow.AddSeconds(30)
        $descriptor = $null
        while ([DateTime]::UtcNow -lt $deadline -and -not $process.HasExited) {
            $descriptor = Get-ChildItem -LiteralPath $sessions -Filter '*.json' -File -ErrorAction SilentlyContinue | Select-Object -First 1
            $ready = (Test-Path -LiteralPath $stdout) -and ((Get-Content -LiteralPath $stdout -Raw) -match 'PHASE7_SIGNAL_FIXTURE_READY')
            if ($null -ne $descriptor -and $ready) { break }
            Start-Sleep -Milliseconds 100
        }
        if ($null -eq $descriptor) { throw "Run $run did not publish an editor descriptor" }
        if (-not ((Get-Content -LiteralPath $stdout -Raw) -match 'PHASE7_SIGNAL_FIXTURE_READY')) {
            throw "Run $run did not initialize the edited-scene fixture"
        }
        if ($Production) { & $ProbeExecutable --production $descriptor.FullName }
        else { & $ProbeExecutable $descriptor.FullName }
        if ($LASTEXITCODE -ne 0) { throw "Raw signal bridge probe failed on run $run" }
        Write-Output "PHASE7_SIGNAL_ENGINE_RUN|$run|ok"
    }
    finally {
        if ($null -ne $process -and -not $process.HasExited) {
            Stop-Process -Id $process.Id -Force
            $process.WaitForExit()
        }
        if ($null -eq $previous) { Remove-Item Env:DIDI_SESSION_DIR -ErrorAction SilentlyContinue }
        else { $env:DIDI_SESSION_DIR = $previous }
    }
}
Write-Output "PHASE7_SIGNAL_BRIDGE_COMPLETE|$script:DidiDetectedGodotVersion|runs=$Repeat"
