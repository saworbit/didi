# Proof in one call: Q9 in docs/BUILD_QUEUE.md, principle P7 in
# docs/DESIGN_PRINCIPLES.md, and #1206.
#
# The standing exercise in docs/SURFACE_AMENDMENTS.md, "make the player
# double-jump and prove it works", as one runtime_run_scenario call against
# res://scenario_runner/double_jump.tscn. This proves on the attached engine
# line what the native tests cannot:
#
# - The pass is the engine's: a real CharacterBody2D, pressed through the
#   game's own InputMap and stepped at a fixed rate, rises past what one jump
#   reaches and prints what its controller prints.
# - The verdict can fail. The same scenario with one press fails on the
#   assertion it names, and so does the two-press scenario once the controller
#   allows only one jump.
# - A pass records what it was true for: editing the controller marks it stale
#   in godot://project/scenarios.
# - Nothing is left behind and nothing is moved: the game the scenario started
#   is gone, the harness's own game is still there, and the editor this server
#   had selected is still the one selected.
# - A scenario with no assertion, and a capture in a headless game, are refused
#   before any game starts.
#
# run_godot_integration.ps1 dot-sources this file and calls the block while the
# editor and the harness's game are attached. Everything under
# res://scenario_runner is its own, and it puts the controller back as it found
# it.

function Get-ScenarioRunnerSteps([bool]$TwoPresses) {
    $player = "/root/DoubleJump/Player"
    # Standing first, whatever the game did before the scenario paused it.
    $steps = @(
        @{ kind = "wait_until"; label = "standing"; expression = 'node.get("velocity").y == 0'; context_node = $player; frames = 120 },
        @{ kind = "press"; action = "ui_accept" },
        @{ kind = "wait"; frames = 12 }
    )
    if ($TwoPresses) { $steps += @{ kind = "press"; action = "ui_accept" } }
    # One jump from the floor peaks about 82 pixels up; the player starts at
    # y = -8, so -120 is out of its reach and inside a second jump's.
    $steps += @(
        @{ kind = "wait"; frames = 20 },
        @{ kind = "assert"; label = "higher than one jump reaches"; expression = 'node.get("position").y'; context_node = $player; maximum = -120 },
        @{ kind = "assert_output"; text = "double jump" },
        @{ kind = "assert_output"; level = "error"; absent = $true }
    )
    return ,$steps
}

function Get-ScenarioRunnerAnswers([object[]]$Lines) {
    $byId = @{}
    foreach ($response in @($Lines | Where-Object { $_ -like "{*" } | ForEach-Object { $_ | ConvertFrom-Json } | Where-Object { $_.PSObject.Properties.Name -contains "id" })) {
        $byId[[int]$response.id] = $response
    }
    return $byId
}

function Get-ScenarioRunnerSessionIds($Response) {
    return ,@((Tool-Payload $Response).sessions | ForEach-Object { $_.session_id } | Sort-Object)
}

function Invoke-ScenarioRunnerBlock {
    param($EditorSession, [string]$FixtureRoot)

    $scene = "res://scenario_runner/double_jump.tscn"
    $playerPath = Join-Path $FixtureRoot (Join-Path "scenario_runner" "double_jump_player.gd")
    $original = [System.IO.File]::ReadAllText($playerPath)
    Assert-True ($original.Contains("const MAX_JUMPS := 2")) "The double-jump fixture no longer declares MAX_JUMPS := 2, which this block edits."

    $requests = @(
        (@{ jsonrpc = "2.0"; id = 8000; method = "initialize"; params = @{ protocolVersion = "2024-11-05" } } | ConvertTo-Json -Compress),
        (Tool-Request 8001 "runtime_attach_session" @{ session_id = $EditorSession.session_id }),
        (Tool-Request 8002 "runtime_list_sessions" @{ project_path = $FixtureRoot }),
        (Tool-Request 8003 "runtime_run_scenario" @{ name = "double_jump"; scene_path = $scene; steps = (Get-ScenarioRunnerSteps $true) }),
        (Tool-Request 8004 "runtime_get_session" @{}),
        (Tool-Request 8005 "runtime_list_sessions" @{ project_path = $FixtureRoot }),
        (Tool-Request 8006 "runtime_run_scenario" @{ name = "one_press"; scene_path = $scene; steps = (Get-ScenarioRunnerSteps $false) }),
        (Tool-Request 8007 "runtime_run_scenario" @{ name = "smoke"; scene_path = $scene; steps = @(@{ kind = "wait"; frames = 30 }, @{ kind = "press"; action = "ui_accept" }) }),
        (Tool-Request 8008 "runtime_run_scenario" @{ name = "headless_capture"; scene_path = $scene; steps = @(@{ kind = "capture" }, @{ kind = "assert"; expression = 'node.get("position").y'; context_node = "/root/DoubleJump/Player"; maximum = 0 }) }),
        (Tool-Request 8009 "runtime_list_sessions" @{ project_path = $FixtureRoot }),
        # The game starts paused, so nothing has run before the first step and
        # a wait of N frames is N physics frames from the scene's own start
        # (#1208).
        (Tool-Request 8014 "runtime_run_scenario" @{ name = "frame_counter"; scene_path = "res://scenario_runner/frame_counter.tscn"; steps = @(
            @{ kind = "assert"; label = "no physics frame yet"; expression = 'node.get("position").x'; context_node = "/root/FrameCounter"; minimum = 0; maximum = 0 },
            @{ kind = "wait"; frames = 30 },
            @{ kind = "assert"; label = "exactly thirty"; expression = 'node.get("position").x'; context_node = "/root/FrameCounter"; minimum = 30; maximum = 30 }) })
    )
    $byId = Get-ScenarioRunnerAnswers (Invoke-Didi -Requests $requests -Arguments @("--project", $FixtureRoot))
    Assert-True ($LASTEXITCODE -eq 0) "Didi scenario process exited with $LASTEXITCODE."
    Assert-True ($byId.Count -eq $requests.Count) "The scenario block received $($byId.Count) answers for $($requests.Count) requests."
    $before = Get-ScenarioRunnerSessionIds $byId[8002]

    # The exercise, in one call.
    $passText = $byId[8003].result.content[0].text
    $pass = $passText | ConvertFrom-Json
    Assert-True (-not $byId[8003].result.isError -and $pass.verdict -eq "pass") "The double-jump scenario did not pass: $passText"
    Assert-True ($pass.assertions.total -eq 3 -and $pass.assertions.held -eq 3) "The double-jump pass did not hold all three assertions: $passText"
    Assert-True ([double]$pass.steps[5].value -le -120) "The double-jump pass reports a height one jump reaches: $($pass.steps[5].value)"
    Assert-True ($pass.steps[6].matched.message -eq "double jump") "The double-jump pass did not cite the controller's own output: $passText"
    Assert-True ($pass.game.fixed_fps -eq 60) "The scenario's game did not run at the project's physics rate: $($pass.game.fixed_fps)"
    Assert-True ($pass.teardown.ok -eq $true -and $pass.teardown.session_gone -eq $true) "The double-jump scenario did not stop its game: $($pass.teardown | ConvertTo-Json -Compress)"
    $recorded = @($pass.files | ForEach-Object { $_.path })
    Assert-True ($recorded -contains "project.godot" -and $recorded -contains $scene -and $recorded -contains "res://scenario_runner/double_jump_player.gd") "The pass does not name the files it ran against: $($recorded -join ', ')"
    Assert-True ($pass.stale -eq $false) "A pass nothing changed under reported itself stale."
    Assert-True ($pass.record -eq ".didi/scenarios/double_jump.json" -and (Test-Path -LiteralPath (Join-Path $FixtureRoot ".didi/scenarios/double_jump.json"))) "The pass was not recorded: $($pass.record)"

    # Its own game, gone; the harness's game, still there; the selection, unmoved.
    Assert-True ($before -notcontains $pass.game.session_id) "The scenario drove a session that was already running instead of a game of its own."
    $after = Get-ScenarioRunnerSessionIds $byId[8005]
    Assert-True (($before -join ",") -eq ($after -join ",")) "The sessions after the scenario ($($after -join ', ')) are not the ones before it ($($before -join ', '))."
    Assert-True ((Tool-Payload $byId[8004]).session.session_id -eq $EditorSession.session_id) "The scenario moved this server's selection off the editor."

    # The same scenario with one press fails, on the assertion it names.
    $oneText = $byId[8006].result.content[0].text
    $one = $oneText | ConvertFrom-Json
    Assert-True ($byId[8006].result.isError -and $one.verdict -eq "fail") "One press passed the double-jump scenario: $oneText"
    Assert-True ($one.failure.step -eq 4 -and $one.error.data.code -eq "scenario_failed" -and $one.error.data.field -eq "steps") "The one-press failure did not name its assertion: $oneText"
    Assert-True ([double]$one.steps[4].value -gt -120) "The one-press run reports a height only a second jump reaches: $($one.steps[4].value)"
    Assert-True ($one.teardown.session_gone -eq $true) "The failed scenario did not stop its game: $oneText"

    # Refused before a game starts.
    $smokeText = $byId[8007].result.content[0].text
    $smoke = $smokeText | ConvertFrom-Json
    Assert-True ($byId[8007].result.isError -and $smoke.error.data.reason -eq "no_assertion" -and $smoke.error.data.field -eq "steps") "A scenario with no assertion was not refused: $smokeText"
    $captureText = $byId[8008].result.content[0].text
    $capture = $captureText | ConvertFrom-Json
    Assert-True ($byId[8008].result.isError -and $capture.error.data.field -eq "headless" -and $capture.error.data.retry_with.headless -eq $false) "A capture in a headless game was not refused with the argument that fixes it: $captureText"
    $counterText = $byId[8014].result.content[0].text
    $counter = $counterText | ConvertFrom-Json
    Assert-True (-not $byId[8014].result.isError -and $counter.verdict -eq "pass") "A scenario's game ran physics frames before its first step, or a wait of 30 frames was not 30: $counterText"
    Assert-True ($counter.game.started_paused -eq $true -and $counter.game.frames_before_pause -eq 0) "The scenario's game did not report starting paused before any physics frame: $($counter.game | ConvertTo-Json -Compress)"
    $final = Get-ScenarioRunnerSessionIds $byId[8009]
    Assert-True (($before -join ",") -eq ($final -join ",")) "A scenario left a session behind: $($final -join ', ')"

    # Break the controller: one jump only. The earlier pass is stale, and the
    # same scenario now fails.
    try {
        [System.IO.File]::WriteAllText($playerPath, $original.Replace("const MAX_JUMPS := 2", "const MAX_JUMPS := 1"))
        $brokenRequests = @(
            (@{ jsonrpc = "2.0"; id = 8010; method = "initialize"; params = @{ protocolVersion = "2024-11-05" } } | ConvertTo-Json -Compress),
            (@{ jsonrpc = "2.0"; id = 8011; method = "resources/read"; params = @{ uri = "godot://project/scenarios" } } | ConvertTo-Json -Compress),
            (Tool-Request 8012 "runtime_run_scenario" @{ name = "double_jump"; scene_path = $scene; steps = (Get-ScenarioRunnerSteps $true) }),
            (Tool-Request 8013 "runtime_list_sessions" @{ project_path = $FixtureRoot })
        )
        $brokenById = Get-ScenarioRunnerAnswers (Invoke-Didi -Requests $brokenRequests -Arguments @("--project", $FixtureRoot))
        Assert-True ($LASTEXITCODE -eq 0) "Didi scenario process exited with $LASTEXITCODE after the controller changed."
        Assert-True ($brokenById.Count -eq $brokenRequests.Count) "The scenario block received $($brokenById.Count) answers for $($brokenRequests.Count) requests after the controller changed."
        $records = ($brokenById[8011].result.contents[0].text | ConvertFrom-Json).scenarios
        $earlier = @($records | Where-Object { $_.name -eq "double_jump" })
        Assert-True ($earlier.Count -eq 1 -and $earlier[0].verdict -eq "pass" -and $earlier[0].stale -eq $true -and @($earlier[0].changed_files) -contains "res://scenario_runner/double_jump_player.gd") "Changing the controller did not mark the earlier pass stale: $($earlier | ConvertTo-Json -Compress -Depth 5)"
        $brokenText = $brokenById[8012].result.content[0].text
        $broken = $brokenText | ConvertFrom-Json
        Assert-True ($brokenById[8012].result.isError -and $broken.verdict -eq "fail" -and $broken.failure.step -eq 5) "The double-jump scenario passed a controller that allows one jump: $brokenText"
        $afterBroken = Get-ScenarioRunnerSessionIds $brokenById[8013]
        Assert-True (($before -join ",") -eq ($afterBroken -join ",")) "The failed scenario left a session behind: $($afterBroken -join ', ')"
    } finally {
        [System.IO.File]::WriteAllText($playerPath, $original)
    }
}
