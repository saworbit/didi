# Independent observations used by existing lifecycle blocks (#1020).
function Assert-ObservedExternalFields([string]$Tool, $Answer, $Witness) {
    $gaps = @(Get-ObservedFieldGaps $Tool $Answer)
    Assert-True ($gaps.Count -eq 0) "$Tool has no observed field $($gaps -join ', ')"
    foreach ($field in $observedFields[$Tool]) {
        $actual = $Answer; $wanted = $Witness
        foreach ($part in $field.Split('.')) { $actual = $actual.$part; $wanted = $wanted.$part }
        Assert-True (Test-ObservedFieldPresent $Witness $field) "$Tool has no independent witness for $field"
        Assert-True (Test-ObservedAgreement $actual $wanted) "$Tool.$field disagrees with the engine: answer $($actual | ConvertTo-Json -Compress -Depth 20), witness $($wanted | ConvertTo-Json -Compress -Depth 20)"
    }
    $observedExternalExecuted[$Tool] = $true
}

function Assert-ProjectApplyObservedState($Answer, $HeldCopy, [string]$Path, [string]$ExpectedText, [string]$FileText) {
    Assert-True ($FileText -ceq $ExpectedText) "project_apply_changes did not write the proposal's bytes."
    $current = @($HeldCopy.methods) -contains 'delta' -and @($HeldCopy.methods) -notcontains 'beta'
    Assert-True $current "project_apply_changes left the engine holding the old script."
    $witness = [pscustomobject]@{ editor_copies_reloaded = @($Path) }
    Assert-ObservedExternalFields 'project_apply_changes' $Answer $witness
}

function Assert-ScenarioObservedTeardown($Answer, [bool]$GameRunning, [bool]$EditorRunning) {
    Assert-True ($Answer.game.pid -gt 0) "runtime_run_scenario did not name a real game process."
    Assert-True $EditorRunning "The process witness did not recognize the live editor control."
    $witness = [pscustomobject]@{ teardown = [pscustomobject]@{ session_gone = -not $GameRunning } }
    Assert-ObservedExternalFields 'runtime_run_scenario' $Answer $witness
}
