# The project's own tests: Q9 part 2 in docs/BUILD_QUEUE.md, principle P7 in
# docs/DESIGN_PRINCIPLES.md, and #1206.
#
# project_run_tests runs GUT or GdUnit4 headless and reads the JUnit report
# each writes. CI has neither, so tests/test_runner_fixture carries a stand-in
# at addons/gut that follows GUT's command-line contract, and this block proves
# Didi's side of it on the attached engine line:
#
# - A passing suite answers verdict: pass with a result per test, and records
#   the files it ran against, the code under test included.
# - A failing test is named, with the file it is in and its message.
# - A test file that does not parse fails the run, though the stand-in, like
#   GUT, leaves it out of its report and exits on what the others did.
# - A directory with no tests proves nothing and says so.
# - Changing the code under test marks the earlier pass stale.
# - A framework the project does not have is refused before any Godot starts.
#
# The fixture is copied fresh into the build tree, because a run writes .godot
# and .didi into the project it runs in.

function Invoke-TestRunnerBlock {
    param([string]$BuildRoot)

    $source = Join-Path $PSScriptRoot "test_runner_fixture"
    $project = Join-Path $BuildRoot "test_runner_fixture"
    if (Test-Path -LiteralPath $project) { Remove-TestDirectory -Path $project }
    Copy-Item -Recurse -LiteralPath $source -Destination $project

    $requests = @(
        (@{ jsonrpc = "2.0"; id = 8100; method = "initialize"; params = @{ protocolVersion = "2024-11-05" } } | ConvertTo-Json -Compress),
        (Tool-Request 8101 "project_run_tests" @{ paths = @("res://test/pass"); name = "passing" }),
        (Tool-Request 8102 "project_run_tests" @{ paths = @("res://test/fail"); name = "failing" }),
        (Tool-Request 8103 "project_run_tests" @{ paths = @("res://test/broken"); name = "broken" }),
        (Tool-Request 8104 "project_run_tests" @{ paths = @("res://test/empty"); name = "empty" }),
        (Tool-Request 8105 "project_run_tests" @{ framework = "gdunit4" })
    )
    $byId = @{}
    foreach ($response in @((Invoke-Didi -Requests $requests -Arguments @("--project", $project)) | Where-Object { $_ -like "{*" } | ForEach-Object { $_ | ConvertFrom-Json } | Where-Object { $_.PSObject.Properties.Name -contains "id" })) {
        $byId[[int]$response.id] = $response
    }
    Assert-True ($LASTEXITCODE -eq 0) "Didi test-runner process exited with $LASTEXITCODE."
    Assert-True ($byId.Count -eq $requests.Count) "The test-runner block received $($byId.Count) answers for $($requests.Count) requests."

    $passText = $byId[8101].result.content[0].text
    $pass = $passText | ConvertFrom-Json
    Assert-True (-not $byId[8101].result.isError -and $pass.verdict -eq "pass") "A passing suite did not pass: $passText"
    Assert-True ($pass.counts.total -eq 2 -and $pass.counts.passed -eq 2) "A passing suite did not report each of its two tests: $passText"
    Assert-True ($pass.framework.name -eq "gut" -and $pass.framework.version -eq "0.0.0-standin") "The run did not name the framework it found: $($pass.framework | ConvertTo-Json -Compress)"
    $recorded = @($pass.files | ForEach-Object { $_.path })
    Assert-True ($recorded -contains "res://src/counter.gd" -and $recorded -contains "res://test/pass/test_counter.gd") "The pass does not name the code it tested: $($recorded -join ', ')"
    Assert-True ($pass.record -eq ".didi/scenarios/passing.json") "The pass was not recorded: $($pass.record)"

    $failText = $byId[8102].result.content[0].text
    $fail = $failText | ConvertFrom-Json
    Assert-True ($byId[8102].result.isError -and $fail.verdict -eq "fail" -and $fail.error.data.code -eq "tests_failed") "A failing test did not fail the run: $failText"
    $failed = @($fail.tests | Where-Object { $_.outcome -eq "failed" })
    Assert-True ($failed.Count -eq 1 -and $failed[0].name -eq "test_is_wrong_on_purpose" -and $failed[0].file -eq "res://test/fail/test_counter_fails.gd" -and $failed[0].message -match "one bump is not three") "The failing test was not named with its file and message: $failText"

    $brokenText = $byId[8103].result.content[0].text
    $broken = $brokenText | ConvertFrom-Json
    Assert-True ($byId[8103].result.isError -and $broken.verdict -ne "pass" -and $broken.reason -eq "scripts_did_not_load") "A test file that does not parse did not fail the run: $brokenText"
    Assert-True (@($broken.scripts_did_not_load | Where-Object { $_.file -eq "res://test/broken/test_broken.gd" }).Count -eq 1) "The test file that did not parse was not named: $brokenText"
    Assert-True ($broken.counts.passed -eq 1) "The tests beside the broken file were not still reported: $brokenText"

    $emptyText = $byId[8104].result.content[0].text
    $empty = $emptyText | ConvertFrom-Json
    Assert-True ($byId[8104].result.isError -and $empty.reason -eq "no_tests" -and $empty.error.data.field -eq "paths") "A run that found no tests was not refused as proving nothing: $emptyText"

    $missing = $byId[8105].result.content[0].text | ConvertFrom-Json
    Assert-True ($byId[8105].result.isError -and $missing.error.code -eq 404 -and $missing.error.data.field -eq "framework") "A framework the project does not have was not refused: $($byId[8105].result.content[0].text)"

    # The code under test changes: the earlier pass is stale.
    $counter = Join-Path $project (Join-Path "src" "counter.gd")
    [System.IO.File]::WriteAllText($counter, [System.IO.File]::ReadAllText($counter).Replace("const STEP := 1", "const STEP := 2"))
    $readRequests = @(
        (@{ jsonrpc = "2.0"; id = 8110; method = "initialize"; params = @{ protocolVersion = "2024-11-05" } } | ConvertTo-Json -Compress),
        (@{ jsonrpc = "2.0"; id = 8111; method = "resources/read"; params = @{ uri = "godot://project/scenarios" } } | ConvertTo-Json -Compress)
    )
    $readById = @{}
    foreach ($response in @((Invoke-Didi -Requests $readRequests -Arguments @("--project", $project)) | Where-Object { $_ -like "{*" } | ForEach-Object { $_ | ConvertFrom-Json } | Where-Object { $_.PSObject.Properties.Name -contains "id" })) {
        $readById[[int]$response.id] = $response
    }
    $records = ($readById[8111].result.contents[0].text | ConvertFrom-Json).scenarios
    $passing = @($records | Where-Object { $_.name -eq "passing" })
    Assert-True ($passing.Count -eq 1 -and $passing[0].kind -eq "tests" -and $passing[0].verdict -eq "pass" -and $passing[0].stale -eq $true -and @($passing[0].changed_files) -contains "res://src/counter.gd") "Changing the code under test did not mark the pass stale: $($passing | ConvertTo-Json -Compress -Depth 5)"
}
