# Performance verdicts: Q16 in docs/BUILD_QUEUE.md, principle P7 in
# docs/DESIGN_PRINCIPLES.md, and #1228.
#
# runtime_read_profiler judges whether a frame is bound on the CPU, the GPU or
# physics. The native tests replay frames probed on all three engine lines;
# this block proves the live half on the attached line:
#
# - perf_cpu.tscn, perf_gpu.tscn and perf_physics.tscn, each launched in a
#   window, read cpu, gpu and physics. A window, because a headless game never
#   draws, and a game that draws nothing has no GPU time to weigh.
# - The GPU-bound game's self-check stalls its process step by a known amount,
#   and the verdict it reads then is cpu, with the stall in its process time.
# - A second read of each game still measures the GPU. Switching render-time
#   measurement off keeps the last times it took, and a second read that took
#   those for the game's own would leave measurement on and its times frozen.
#
# The fixtures grow their load until a frame takes three 60 Hz frames, so they
# are bound on a machine with a GPU and on a software-rendered runner alike.
# The block waits for each to say it is ready and asserts the verdict, never a
# timing. The headless game the main runtime block drives is read there, where
# its verdict must leave the GPU unmeasured.
#
# run_godot_integration.ps1 dot-sources this file after reimport_job.ps1, whose
# kept-server helpers it uses, and calls the block with the fixture project.

function Invoke-PerformanceVerdictsBlock {
    param([Parameter(Mandatory = $true)] [string]$FixtureRoot)

    $session = Start-DidiSession @("--project", $FixtureRoot)
    $id = 8200
    $seen = @()
    try {
        [void](Send-DidiSession $session (@{ jsonrpc = "2.0"; id = $id; method = "initialize"; params = @{ protocolVersion = "2024-11-05" } } | ConvertTo-Json -Compress))
        foreach ($kind in @("cpu", "gpu", "physics")) {
            $id++
            $launch = Tool-Payload (Send-DidiSession $session (Tool-Request $id "runtime_launch" @{ scene_path = "res://perf_$kind.tscn"; timeout_seconds = 60; headless = $false; detach = $true }) 90)
            Assert-True ($launch.detached -eq $true -and $launch.session_published -eq $true) "The $kind-bound fixture did not start in a window: $($launch | ConvertTo-Json -Compress -Depth 6)"
            $gameId = [string]$launch.game_session.session_id
            try {
                $id++
                [void](Tool-Payload (Send-DidiSession $session (Tool-Request $id "runtime_attach_session" @{ session_id = $gameId })))

                # The load grows until it holds the frame; ready says it has.
                $state = $null
                $waited = [Diagnostics.Stopwatch]::StartNew()
                while ($waited.Elapsed.TotalSeconds -lt 90) {
                    $id++
                    $state = (Tool-Payload (Send-DidiSession $session (Tool-Request $id "eval_gdscript" @{ expression = "node.get('editor_description')"; context_node = "/root/PerfLoad" }))).value
                    if ($state -eq "ready") { break }
                    Start-Sleep -Milliseconds 500
                }
                Assert-True ($state -eq "ready") "The $kind-bound fixture did not hold its load within 90 seconds: $state"

                $id++
                $readId = $id
                $read = Send-DidiSession $session (Tool-Request $readId "runtime_read_profiler" @{ duration_ms = 2000; sample_count = 5; self_check = ($kind -eq "gpu") }) 60
                $profile = Tool-Payload $read
                $verdict = $profile.verdict
                $verdictText = $verdict | ConvertTo-Json -Compress -Depth 6
                Assert-True ($verdict.bound -eq $kind) "The $kind-bound fixture read as $($verdict.bound): $verdictText"
                Assert-True ($verdict.confidence -ne "low") "The $kind-bound fixture's verdict was not confident: $verdictText"
                Assert-True ($verdict.gpu_measured -eq $true -and $null -ne $verdict.median_ms.gpu) "A game drawn in a window did not have its GPU measured: $verdictText"
                Assert-True ($verdict.frames -ge 3 -and $verdict.median_ms.frame -gt $verdict.frame_budget_ms) "The $kind-bound fixture's frames were not over their budget: $verdictText"
                Assert-True ($verdict.next.Length -gt 0) "The $kind-bound fixture's verdict says nothing to look at: $verdictText"
                $seen += "$kind ($($verdict.confidence), $($verdict.median_ms.frame) ms frames)"
                if ($kind -eq "gpu") {
                    $check = $profile.self_check
                    $checkText = $check | ConvertTo-Json -Compress
                    Assert-True ($null -ne $check -and $check.passed -eq $true -and $check.bound -eq "cpu") "The self-check did not see its stall move the verdict to cpu: $checkText"
                    Assert-True ($check.process_ms -ge 0.9 * $check.stall_ms) "The self-check's process time does not hold its stall: $checkText"
                    $seen += "self-check saw a $($check.stall_ms) ms stall as cpu"
                } else {
                    Assert-True ($null -eq $profile.self_check) "A read that asked for no self-check reported one: $($profile.self_check | ConvertTo-Json -Compress)"
                }

                $id++
                $again = (Tool-Payload (Send-DidiSession $session (Tool-Request $id "runtime_read_profiler" @{ duration_ms = 600; sample_count = 2 }) 30)).verdict
                Assert-True ($again.gpu_measured -eq $true) "A second read of the $kind-bound fixture lost its GPU times: $($again | ConvertTo-Json -Compress -Depth 6)"
            } finally {
                $id++
                [void](Send-DidiSession $session (Tool-Request $id "runtime_stop" @{ exit_code = 0 }) 30)
            }
        }
    } finally {
        Stop-DidiSession $session
    }
    Write-Host "Performance verdicts: $($seen -join '; ')."
}
