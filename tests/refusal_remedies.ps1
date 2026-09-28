# Every refusal names what fixes it: Q6 in docs/BUILD_QUEUE.md, principle P5
# in docs/DESIGN_PRINCIPLES.md.
#
# A refusal's error.data carries the argument or the call that fixes it:
# retry_with, field (or argument, missing, did_you_mean), next_call,
# restart_with or retry_after_ms. The error floor fills one from
# src/mcp/refusal_remedies.cpp when the site gave none. A 5xx fault other than
# 503 and 504 is the server's to fix, and a code the manifest lists under
# refusals.without_remedy has its reason there.
#
# run_godot_integration.ps1 dot-sources this file. Every exchange goes through
# Invoke-Didi, which records every failed tool answer here, and the end of the
# run checks each one. The census of codes against the source is
# tests/test_refusal_remedies.py, on every build.

$refusalExchanges = New-Object System.Collections.ArrayList
$refusalRemedyFields = @("retry_with", "field", "argument", "parameter", "missing", "did_you_mean", "next_call", "restart_with", "retry_after_ms", "no_remedy")

function Add-RefusalExchanges([object[]]$Requests, [object[]]$Lines) {
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
        # Only failures are parsed; a success cannot be a refusal.
        if ([string]$line -notmatch '"isError":true') { continue }
        [void]$refusalExchanges.Add([pscustomobject]@{ Tool = $calls[$match.Groups[1].Value]; Response = ([string]$line | ConvertFrom-Json) })
    }
}

function Assert-RefusalsNameTheirFix([string]$ManifestJson) {
    $withoutRemedy = @(($ManifestJson | ConvertFrom-Json).refusals.without_remedy.PSObject.Properties.Name)
    $missing = @()
    $checked = 0
    $repeats = 0
    $unstructured = @{}
    foreach ($exchange in $refusalExchanges) {
        $text = @($exchange.Response.result.content | Where-Object { $_.type -eq "text" })[0].text
        $envelope = $null
        try { $envelope = $text | ConvertFrom-Json } catch { $envelope = $null }
        if ($null -eq $envelope -or $null -eq $envelope.PSObject.Properties["error"] -or $envelope.error -isnot [pscustomobject]) {
            $unstructured[$exchange.Tool] = 1 + [int]$unstructured[$exchange.Tool]
            continue
        }
        $status = [int]$envelope.error.code
        if ($status -ge 500 -and $status -ne 503 -and $status -ne 504) { continue }
        $data = $envelope.error.data
        if ($null -ne $data -and $withoutRemedy -contains [string]$data.code) { continue }
        $checked++
        # The same call failing the same way again says so, and what it points
        # at is a fix the refusal really carries (Q6).
        if ($null -ne $data -and $null -ne $data.PSObject.Properties["repeated"]) {
            $repeats++
            $repeat = $data.repeated
            if ([int]$repeat.count -lt 2) { $missing += "$($exchange.Tool) request $($exchange.Response.id): repeated with a count under 2" }
            if ($null -ne $repeat.PSObject.Properties["follow"] -and $null -eq $data.PSObject.Properties[[string]$repeat.follow]) {
                $missing += "$($exchange.Tool) request $($exchange.Response.id): repeated points at $($repeat.follow), which it does not carry"
            }
        }
        $named = @($refusalRemedyFields | Where-Object { $null -ne $data -and $null -ne $data.PSObject.Properties[$_] })
        if ($named.Count -eq 0) {
            $said = [string]$envelope.error.message
            if ($said.Length -gt 110) { $said = $said.Substring(0, 110) + "..." }
            $missing += "$($exchange.Tool) request $($exchange.Response.id): $status $($data.code) names no fix: $said"
        }
    }
    Assert-True ($missing.Count -eq 0) "Refusals that do not say what fixes them:`n$($missing -join "`n")"
    Assert-True ($checked -ge 50) "Only $checked refusals were checked in this run; the harness has stopped reaching them."
    $plain = @($unstructured.Keys | Sort-Object | ForEach-Object { "$_ x$($unstructured[$_])" })
    Write-Output "Refusal remedies: $checked refusals named their fix, $repeats of them marked as a repeat. Plain-text failures still without an envelope: $(if ($plain.Count) { $plain -join ', ' } else { 'none' })."
}
