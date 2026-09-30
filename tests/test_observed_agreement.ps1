# Cases for the comparison the observed post-state block judges answers with.
#
# Run by run_godot_integration.ps1 before it starts an engine, so a rule that
# has drifted fails in a second rather than after a run it could have passed
# wrongly. An answer object is allowed to say less than the witness, and one
# that says nothing agreed with anything until #1097.

. (Join-Path $PSScriptRoot "observed_post_state.ps1")

function Json([string]$Text) { return $Text | ConvertFrom-Json }

$cell = Json '{"source_id": 0, "atlas_coords": {"x": 0, "y": 0}, "alternative_tile": 0}'

$cases = [ordered]@{
    "the same object" = @{ observed = $cell; witnessed = $cell; expect = $true }
    "an answer that says less than the witness" =
        @{ observed = (Json '{"source_id": 0}'); witnessed = $cell; expect = $true }
    "an answer that disagrees on a field" =
        @{ observed = (Json '{"source_id": 1}'); witnessed = $cell; expect = $false }
    "an empty answer against a witness with fields" =
        @{ observed = (Json '{}'); witnessed = $cell; expect = $false }
    "an empty answer against an empty witness" =
        @{ observed = (Json '{}'); witnessed = (Json '{}'); expect = $true }
    "an empty object inside an answer" =
        @{ observed = (Json '{"source_id": 0, "atlas_coords": {}}'); witnessed = $cell; expect = $false }
    "an empty object in a list" =
        @{ observed = (Json '[{}]'); witnessed = (Json '[{"item": 0}]'); expect = $false }
    "a missing answer" = @{ observed = $null; witnessed = $cell; expect = $false }
}

$failures = 0
foreach ($name in $cases.Keys) {
    $got = Test-ObservedAgreement $cases[$name].observed $cases[$name].witnessed
    if ($got -ne $cases[$name].expect) {
        $failures++
        Write-Output ("FAIL  {0}: agreed={1} expected={2}" -f $name, $got, $cases[$name].expect)
    }
}

if ($failures -ne 0) {
    throw "Observed agreement: $failures of $($cases.Count) cases wrong. An answer that read nothing back must not agree with the engine."
}
Write-Output "Observed agreement: $($cases.Count) cases correct."
