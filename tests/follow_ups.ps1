# Every mutation that leaves work undone names it: Q6 in docs/BUILD_QUEUE.md,
# principle P5 in docs/DESIGN_PRINCIPLES.md.
#
# tests/follow_ups.json says, for every mutating tool, the work a successful
# call can leave, and src/mcp/follow_ups.cpp derives each follow_up step from a
# fact the answer carries. run_godot_integration.ps1 dot-sources this file.
# Every exchange goes through Invoke-Didi, which records the successful answers
# that carry a step or a fact here, and the end of the run checks, on every
# engine line:
#
# - Every step is one its tool declares, says why, and names a tool the server
#   implements when it names one.
# - Every fact arrived with its step: scene_saved: false with a save, and
#   requires_editor_restart: true with a restart.
# - Enough of each was seen that the check cannot pass on nothing.
#
# Whether a save step is true is checked against the engine in the DirtyProbe
# block, where Godot 4.7 names the scene it holds unsaved.

$followUpRegistry = Get-Content -LiteralPath (Join-Path $PSScriptRoot "follow_ups.json") -Raw | ConvertFrom-Json
$followUpExchanges = New-Object System.Collections.ArrayList

# Called by Invoke-Didi with each batch and what came back. Only successful
# answers that mention a step or a fact are parsed; the rest cannot fail here.
function Add-FollowUpExchanges([object[]]$Requests, [object[]]$Lines) {
    $calls = @{}
    foreach ($request in $Requests) {
        $text = [string]$request
        if ($text -notmatch '"tools/call"') { continue }
        $parsed = $text | ConvertFrom-Json
        $calls[[string]$parsed.id] = [string]$parsed.params.name
    }
    if ($calls.Count -eq 0) { return }
    foreach ($line in $Lines) {
        $match = [regex]::Match([string]$line, '^\{"id":(\d+),')
        if (-not $match.Success -or -not $calls.ContainsKey($match.Groups[1].Value)) { continue }
        if ([string]$line -match '"isError":true') { continue }
        if ([string]$line -notmatch 'follow_up|scene_saved|requires_editor_restart') { continue }
        [void]$followUpExchanges.Add([pscustomobject]@{ Tool = $calls[$match.Groups[1].Value]; Response = ([string]$line | ConvertFrom-Json) })
    }
}

function Assert-MutationsNameTheirFollowUps([string]$ManifestJson) {
    $implemented = @(($ManifestJson | ConvertFrom-Json).names.implemented)
    $problems = @()
    $seen = @{}
    foreach ($exchange in $followUpExchanges) {
        $result = $exchange.Response.result
        $payload = if ($null -ne $result.PSObject.Properties["structuredContent"]) { $result.structuredContent } else { @($result.content | Where-Object { $_.type -eq "text" })[0].text | ConvertFrom-Json }
        if ($payload -isnot [pscustomobject]) { continue }
        $where = "$($exchange.Tool) request $($exchange.Response.id)"
        $entry = $followUpRegistry.tools.PSObject.Properties[$exchange.Tool]
        $declared = if ($null -ne $entry -and $null -ne $entry.Value.PSObject.Properties["leaves"]) { @($entry.Value.leaves) } else { @() }
        $steps = if ($null -ne $payload.PSObject.Properties["follow_up"]) { @($payload.follow_up) } else { @() }
        # A job still running answers with a poll step of its own (Q8). It is
        # the job's, not the tool's, so no tool declares it; it has to name
        # the tool the job runs, which is the call that reads it.
        if ($payload.status -eq "working" -and $null -ne $payload.PSObject.Properties["job"]) {
            foreach ($step in $steps) {
                $seen["poll"] = 1 + [int]$seen["poll"]
                if ([string]$step.work -ne "poll" -or [string]$step.tool -ne $exchange.Tool -or [string]::IsNullOrWhiteSpace([string]$step.reason)) {
                    $problems += "$where is a working job whose step is not a poll of $($exchange.Tool) with a reason: $($step | ConvertTo-Json -Compress)"
                }
            }
            continue
        }
        foreach ($step in $steps) {
            $work = [string]$step.work
            $seen[$work] = 1 + [int]$seen[$work]
            if ($declared -notcontains $work) { $problems += "$where named $work, which tests/follow_ups.json does not declare for it" }
            if ([string]::IsNullOrWhiteSpace([string]$step.reason)) { $problems += "$where named $work without a reason" }
            if ($null -ne $step.PSObject.Properties["tool"] -and $implemented -notcontains [string]$step.tool) {
                $problems += "$where named $($step.tool), which the server does not implement"
            }
        }
        $works = @($steps | ForEach-Object { [string]$_.work })
        if ($null -ne $payload.PSObject.Properties["scene_saved"] -and $payload.scene_saved -eq $false -and $works -notcontains "save") {
            $problems += "$where said scene_saved: false and named no save"
        }
        if ($null -ne $payload.PSObject.Properties["requires_editor_restart"] -and $payload.requires_editor_restart -eq $true -and $works -notcontains "restart") {
            $problems += "$where said requires_editor_restart: true and named no restart"
        }
    }
    Assert-True ($problems.Count -eq 0) "Follow-ups that are undeclared, unexplained or missing:`n$($problems -join "`n")"
    # Floors, so a harness that stopped reaching these answers cannot pass by
    # having nothing to check.
    Assert-True ([int]$seen["save"] -ge 10 -and [int]$seen["restart"] -ge 1) "Only $([int]$seen['save']) save and $([int]$seen['restart']) restart steps were seen in this run."
    Write-Output "Follow-ups: $([int]$seen['save']) save and $([int]$seen['restart']) restart steps, each declared by its tool."
}
