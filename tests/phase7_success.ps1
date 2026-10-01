# Every Phase 7 answer meets its success contract (#861).
#
# Each schema in schemas/phase7 declares what a successful answer looks like
# in $defs.success. Nothing compared that with an answer, so it drifted from
# what the tools say. run_godot_integration.ps1 dot-sources this file. Every
# exchange goes through Invoke-Didi, which keeps each successful Phase 7
# answer here; at the end of the run tools/phase7_success.py checks every one
# against its tool's contract, setting aside the fields the server adds to all
# answers. Dry runs are left out: a preview is not the answer the contract
# describes. The answers the contract snapshots recorded are checked the same
# way, with no engine, in tests/test_phase7_schema_contract.py.

$phase7SuccessTools = @(Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot "..\schemas\phase7") -Filter "*.schema.json" |
    ForEach-Object { $_.Name -replace '\.schema\.json$', '' })
$phase7SuccessLines = New-Object System.Collections.ArrayList

# Called by Invoke-Didi with each batch and what came back. The response is
# kept as the server wrote it, so the check reads the same JSON a client does.
function Add-Phase7Exchanges([object[]]$Requests, [object[]]$Lines) {
    $calls = @{}
    foreach ($request in $Requests) {
        $text = [string]$request
        if ($text -notmatch '"tools/call"') { continue }
        $parsed = $text | ConvertFrom-Json
        $name = [string]$parsed.params.name
        if ($phase7SuccessTools -notcontains $name) { continue }
        $arguments = $parsed.params.arguments
        if ($null -ne $arguments -and $null -ne $arguments.PSObject.Properties["dry_run"] -and $arguments.dry_run) { continue }
        $calls[[string]$parsed.id] = $name
    }
    if ($calls.Count -eq 0) { return }
    foreach ($line in $Lines) {
        # The server writes object keys in sorted order, so "id" leads.
        $match = [regex]::Match([string]$line, '^\{"id":(\d+),')
        if (-not $match.Success -or -not $calls.ContainsKey($match.Groups[1].Value)) { continue }
        [void]$phase7SuccessLines.Add('{"tool":"' + $calls[$match.Groups[1].Value] + '","response":' + [string]$line + '}')
    }
}

function Assert-Phase7AnswersMeetTheirContracts([string]$OutputPath) {
    # Python's own messages arrive on stderr, which Windows PowerShell turns
    # into error records; they are the report, not a reason to stop.
    $ErrorActionPreference = "Continue"
    [IO.File]::WriteAllLines($OutputPath, [string[]]@($phase7SuccessLines), (New-Object Text.UTF8Encoding($false)))
    $checker = Join-Path $PSScriptRoot "..\tools\phase7_success.py"
    $report = @(& python $checker $OutputPath 2>&1 | ForEach-Object { [string]$_ })
    $exit = $LASTEXITCODE
    Assert-True ($exit -eq 0) "Phase 7 answers that break their success contract in schemas/phase7 (exit $exit):`n$($report -join "`n")"
    $summary = [string]($report | Select-Object -Last 1)
    # Every implemented Phase 7 tool, so a harness that stopped reaching one
    # cannot pass by having nothing to check. The three blocked names never
    # answer, which leaves fifteen.
    $answered = [regex]::Match($summary, 'from (\d+) tools')
    Assert-True ($answered.Success -and [int]$answered.Groups[1].Value -ge 15) "Too few Phase 7 tools answered successfully to check their contracts: $summary"
    Write-Output $summary
}
