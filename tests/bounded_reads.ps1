# Every bounded read says whether it is complete: Q5 in docs/BUILD_QUEUE.md,
# principle P4 in docs/DESIGN_PRINCIPLES.md.
#
# Every read-only tool the server implements has one entry in
# bounded_reads.json. Either it is bounded, saying what bounds it, and every
# successful answer carries a boolean `truncated` that is true when a bound was
# reached; or it is unbounded, saying why nothing can cut its answer short.
# run_godot_integration.ps1 dot-sources this file and checks two things, on
# every engine line:
#
# - Coverage. The registry names exactly the read-only tools the built server
#   implements. A new read with neither classification fails here, and in
#   tests/test_bounded_reads.py on every build.
# - Presence. Every exchange the harness makes goes through Invoke-Didi, which
#   records the bounded tools' answers here. Every successful one carries a
#   boolean `truncated`, wherever in the run it was made. The offline paths are
#   driven in tests/test_bounded_reads.py, and the flag's honesty for each
#   bound in the native and Python suites.

$boundedRegistry = Get-Content -LiteralPath (Join-Path $PSScriptRoot "bounded_reads.json") -Raw | ConvertFrom-Json
$boundedTools = @($boundedRegistry.tools.PSObject.Properties |
    Where-Object { $null -ne $_.Value.PSObject.Properties["bounded"] } |
    ForEach-Object { $_.Name })
$boundedExchanges = New-Object System.Collections.ArrayList

# Called by Invoke-Didi with each batch and what came back. Only the bounded
# tools' answers are kept and parsed.
function Add-BoundedExchanges([object[]]$Requests, [object[]]$Lines) {
    $calls = @{}
    foreach ($request in $Requests) {
        $text = [string]$request
        if ($text -notmatch '"tools/call"') { continue }
        $parsed = $text | ConvertFrom-Json
        if ($boundedTools -contains [string]$parsed.params.name) { $calls[[string]$parsed.id] = [string]$parsed.params.name }
    }
    if ($calls.Count -eq 0) { return }
    foreach ($line in $Lines) {
        # The server writes object keys in sorted order, so "id" leads.
        $match = [regex]::Match([string]$line, '^\{"id":(\d+),')
        if (-not $match.Success -or -not $calls.ContainsKey($match.Groups[1].Value)) { continue }
        [void]$boundedExchanges.Add([pscustomobject]@{ Tool = $calls[$match.Groups[1].Value]; Response = ([string]$line | ConvertFrom-Json) })
    }
}

function Assert-BoundedReadsCoverage([string]$ManifestJson) {
    $manifest = $ManifestJson | ConvertFrom-Json
    $reads = @($manifest.names.implemented | Where-Object { @($manifest.names.mutating) -notcontains $_ })
    $registered = @($boundedRegistry.tools.PSObject.Properties.Name)
    $unaccounted = @($reads | Where-Object { $registered -notcontains $_ })
    $stale = @($registered | Where-Object { $reads -notcontains $_ })
    Assert-True ($unaccounted.Count -eq 0) "Read-only tools classified neither bounded nor unbounded in tests/bounded_reads.json: $($unaccounted -join ', ')."
    Assert-True ($stale.Count -eq 0) "tests/bounded_reads.json names tools the server does not implement as reads: $($stale -join ', ')."
}

function Assert-BoundedAnswersSayWhetherComplete {
    $missing = @()
    $answered = @{}
    foreach ($exchange in $boundedExchanges) {
        $result = $exchange.Response.result
        if ($null -eq $result -or $result.isError) { continue }
        $payload = if ($null -ne $result.PSObject.Properties["structuredContent"]) { $result.structuredContent } else { @($result.content | Where-Object { $_.type -eq "text" })[0].text | ConvertFrom-Json }
        $answered[$exchange.Tool] = $true
        $flag = $payload.PSObject.Properties["truncated"]
        if ($null -eq $flag -or $flag.Value -isnot [bool]) {
            $missing += "$($exchange.Tool) answered request $($exchange.Response.id) without a boolean truncated"
        }
    }
    Assert-True ($missing.Count -eq 0) "Bounded reads that did not say whether they are complete:`n$($missing -join "`n")"
    # A floor, so a harness that stopped reaching these tools cannot pass by
    # having nothing to check.
    Assert-True ($answered.Count -ge 20) "Only $($answered.Count) bounded tools answered successfully in this run: $(@($answered.Keys | Sort-Object) -join ', ')."
    Write-Output "Bounded reads: $($boundedExchanges.Count) answers from $($answered.Count) of $($boundedTools.Count) bounded tools said whether they are complete."
}
