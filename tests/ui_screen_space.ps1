# ui_hit_test in window pixels (#1223).
#
# Since #1219 ui_list_controls reports each Control's screen_rect, the window
# pixels runtime_inject_input takes, beside its global_rect in the viewport's
# own coordinates. ui_hit_test now takes a point in the same window pixels with
# space: "screen" and gives each hit in a game its screen_rect, so list, hit and
# click use one space. This block proves it in a stretched game on the attached
# engine line:
#
# - ui_stretch.tscn draws at 1600x900 into a 1280x720 window, so the two spaces
#   differ and a point given in the wrong one misses.
# - The centre of the Start button's screen_rect, given as space: "screen",
#   hits the button, the hit carries the same screen_rect, and the answer names
#   the viewport point it compared.
# - The same numbers as a viewport point miss it.
# - An editor session refuses space: "screen" and says to use a viewport point.
#
# run_godot_integration.ps1 dot-sources this after reimport_job.ps1, whose
# kept-server helpers it uses.

function Invoke-UiScreenSpaceBlock {
    param(
        [Parameter(Mandatory = $true)] [string]$FixtureRoot,
        [Parameter(Mandatory = $true)] [string]$EditorSessionId
    )

    $session = Start-DidiSession @("--project", $FixtureRoot)
    $id = 8300
    try {
        [void](Send-DidiSession $session (@{ jsonrpc = "2.0"; id = $id; method = "initialize"; params = @{ protocolVersion = "2024-11-05" } } | ConvertTo-Json -Compress))
        $id++
        $launch = Tool-Payload (Send-DidiSession $session (Tool-Request $id "runtime_launch" @{ scene_path = "res://ui_stretch.tscn"; timeout_seconds = 60; headless = $false; detach = $true; extra_args = @("--resolution", "1280x720") }) 90)
        Assert-True ($launch.session_published -eq $true) "The stretched UI fixture did not start in a window: $($launch | ConvertTo-Json -Compress -Depth 6)"
        try {
            $id++
            [void](Tool-Payload (Send-DidiSession $session (Tool-Request $id "runtime_attach_session" @{ session_id = [string]$launch.game_session.session_id })))
            $id++
            $controls = @((Tool-Payload (Send-DidiSession $session (Tool-Request $id "ui_list_controls" @{ max_results = 32 }))).controls | Where-Object { $_.node_path -like "*/Start" })
            Assert-True ($controls.Count -eq 1) "ui_list_controls did not list the stretched fixture's Start button."
            $global = $controls[0].global_rect
            $screen = $controls[0].screen_rect
            $controlText = $controls[0] | ConvertTo-Json -Compress -Depth 4
            Assert-True ([math]::Abs($screen.size.x - $global.size.x) -gt 1) "The fixture is not stretched, so it proves nothing about window pixels: $controlText"
            $centreX = $screen.position.x + $screen.size.x / 2
            $centreY = $screen.position.y + $screen.size.y / 2

            $id++
            $hit = Tool-Payload (Send-DidiSession $session (Tool-Request $id "ui_hit_test" @{ point = @{ x = $centreX; y = $centreY }; space = "screen"; max_results = 8 }))
            $hitText = $hit | ConvertTo-Json -Compress -Depth 6
            Assert-True ($null -ne $hit.topmost -and $hit.topmost.node_path -eq $controls[0].node_path) "ui_hit_test given the Start button's screen_rect centre in window pixels did not name the button: $hitText"
            Assert-True ($hit.space -eq "screen" -and [math]::Abs($hit.viewport_point.x - ($global.position.x + $global.size.x / 2)) -lt 1 -and [math]::Abs($hit.viewport_point.y - ($global.position.y + $global.size.y / 2)) -lt 1) "ui_hit_test did not map the window point to the button's viewport centre: $hitText"
            $hitScreen = $hit.topmost.screen_rect
            Assert-True ($null -ne $hitScreen -and [math]::Abs($hitScreen.position.x - $screen.position.x) -lt 0.01 -and [math]::Abs($hitScreen.position.y - $screen.position.y) -lt 0.01 -and [math]::Abs($hitScreen.size.x - $screen.size.x) -lt 0.01 -and [math]::Abs($hitScreen.size.y - $screen.size.y) -lt 0.01) "The hit's screen_rect is not the one ui_list_controls reported: $hitText against $controlText"

            $id++
            $wrongSpace = Tool-Payload (Send-DidiSession $session (Tool-Request $id "ui_hit_test" @{ point = @{ x = $centreX; y = $centreY }; max_results = 8 }))
            Assert-True ($null -eq $wrongSpace.topmost -or $wrongSpace.topmost.node_path -ne $controls[0].node_path) "The window-pixel centre given as a viewport point still hit the button, so the fixture does not tell the spaces apart: $($wrongSpace | ConvertTo-Json -Compress -Depth 6)"
            Assert-True ($null -eq $wrongSpace.PSObject.Properties["space"]) "A viewport point's answer changed shape: $($wrongSpace | ConvertTo-Json -Compress -Depth 6)"
        } finally {
            $id++
            [void](Send-DidiSession $session (Tool-Request $id "runtime_stop" @{ exit_code = 0 }) 30)
        }

        $id++
        [void](Tool-Payload (Send-DidiSession $session (Tool-Request $id "runtime_attach_session" @{ session_id = $EditorSessionId })))
        $id++
        $editorRefusal = Send-DidiSession $session (Tool-Request $id "ui_hit_test" @{ point = @{ x = 10; y = 10 }; space = "screen" })
        $refusalText = $editorRefusal.result.content[0].text
        $refusal = ($refusalText | ConvertFrom-Json).error
        Assert-True ($editorRefusal.result.isError -and $refusal.code -eq 400 -and $refusal.data.field -eq "space" -and $refusal.data.retry_with.space -eq "viewport") "An editor session did not refuse space: screen with what fixes it: $refusalText"
    } finally {
        Stop-DidiSession $session
    }
    Write-Host "UI screen space: a window-pixel point hit the stretched Start button at viewport ($($hit.viewport_point.x), $($hit.viewport_point.y))."
}
