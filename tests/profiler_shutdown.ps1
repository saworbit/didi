# #1234: the game exits while a long profiler read owns the frame timer.
# The fixture witnesses the timer's real process_frame connection before quit,
# so neither a fixed delay nor a second client decides whether the read started.
# Engine output is the regression gate: shutdown must not unwatch a freed tree
# or turn off measurement on the root viewport after the engine has freed it.
function Invoke-ProfilerShutdownBlock {
    param([Parameter(Mandatory = $true)] [string]$FixtureRoot)

    $runName = "godot_profiler_shutdown_" + [guid]::NewGuid().ToString("N")
    $stdout = Join-Path $buildRoot ($runName + ".out")
    $stderr = Join-Path $buildRoot ($runName + ".err")
    $engineLog = Join-Path $buildRoot ($runName + ".log")
    $game = $null
    $server = $null
    $engine = $null
    # Official Windows console executables are launchers for a sibling engine.
    # Launch that engine directly so even failed discovery has an owned handle.
    $gameExecutable = $GodotExecutable
    if ($gameExecutable -match '_console\.exe$') {
        $sibling = $gameExecutable -replace '_console\.exe$', '.exe'
        if (Test-Path -LiteralPath $sibling) { $gameExecutable = $sibling }
    }
    try {
        $server = Start-DidiSession @("--project", $FixtureRoot)
        [void](Send-DidiSession $server (@{ jsonrpc = "2.0"; id = 8500; method = "initialize"; params = @{ protocolVersion = "2024-11-05" } } | ConvertTo-Json -Compress) 10)
        $before = Tool-Payload (Send-DidiSession $server (Tool-Request 8501 "runtime_list_sessions" @{ project_path = $FixtureRoot }) 10)
        $previousIds = @($before.sessions.session_id)
        $launchedAt = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
        $game = Start-Process -FilePath $gameExecutable `
            -ArgumentList @("--headless", "--path", ('"' + $FixtureRoot + '"'), "--log-file", ('"' + $engineLog + '"'), "res://profiler_shutdown.tscn") `
            -PassThru -WindowStyle Hidden `
            -RedirectStandardOutput $stdout -RedirectStandardError $stderr
        $gameStartedAt = Process-StartedAtMs $game
        $published = $null
        $deadline = [DateTime]::UtcNow.AddSeconds(10)
        while ([DateTime]::UtcNow -lt $deadline -and -not $game.HasExited) {
            $sessions = Tool-Payload (Send-DidiSession $server (Tool-Request 8501 "runtime_list_sessions" @{ project_path = $FixtureRoot }) 10)
            $published = @($sessions.sessions | Where-Object { $_.kind -eq "game" -and $_.alive -and $_.pid -eq $game.Id -and $previousIds -notcontains $_.session_id -and $_.started_at_ms -ge $launchedAt }) | Select-Object -First 1
            if ($null -ne $published) { break }
            Start-Sleep -Milliseconds 100
        }
        Assert-True ($null -ne $published) "Profiler shutdown fixture did not publish its own game session. Logs: $engineLog, $stderr"
        $engine = Get-Process -Id $published.pid
        [void]$engine.Handle
        $engineStartedAt = [int64]$published.started_at_ms
        Assert-True ([Math]::Abs((Process-StartedAtMs $engine) - $engineStartedAt) -le 1) "Profiler fixture process identity did not match its descriptor."
        [void](Tool-Payload (Send-DidiSession $server (Tool-Request 8502 "runtime_attach_session" @{ session_id = $published.session_id }) 10))
        $read = Send-DidiSession $server (Tool-Request 8503 "runtime_read_profiler" @{ duration_ms = 3000; sample_count = 10 }) 10
        Assert-True ($engine.WaitForExit(5000)) "The profiler shutdown fixture did not exit after witnessing the timer. Logs: $engineLog, $stderr"
        Assert-True ($game.WaitForExit(5000)) "The profiler fixture launcher did not finish after its engine exited."
        Assert-True ($game.ExitCode -eq 0) "The profiler shutdown fixture exited with $($game.ExitCode). Logs: $engineLog, $stderr"
        $output = @(
            Get-Content -LiteralPath $stdout -ErrorAction SilentlyContinue
            Get-Content -LiteralPath $stderr -ErrorAction SilentlyContinue
            Get-Content -LiteralPath $engineLog -ErrorAction SilentlyContinue
        )
        Assert-True (($output -join "`n") -match 'PROFILER_SHUTDOWN_TIMER_ACTIVE') "The game exited without witnessing an active profiler timer. Logs: $engineLog, $stderr"
        Assert-True ([bool]$read.result.isError) "The profiler read returned partial success after the fixture exited: $($read.result | ConvertTo-Json -Compress -Depth 8)"
        $errorText = [string]$read.result.content[0].text
        $errorPayload = ($errorText | ConvertFrom-Json).error
        # The bridge's 504 cancellation is normalized to public 502 while its
        # live_session_ended code and upstream_code remain available (#1234).
        Assert-True ($errorPayload.code -eq 502 -and $errorPayload.data.code -eq "live_session_ended" -and $errorPayload.data.upstream_code -eq 504 -and $errorPayload.data.retryable -eq $false) "Active profiler shutdown returned the wrong public cancellation: $errorText"
        $engineErrors = @($output | Where-Object { $_ -match '(^|\s)(SCRIPT )?ERROR:|\[ERROR\s*\]|Parameter "(viewport|t)" is null|Cannot get class|Can.t retrieve singleton' })
        Assert-True ($engineErrors.Count -eq 0) "Profiler shutdown reported engine errors:`n$($engineErrors -join "`n")`nLogs: $engineLog, $stderr"
    } finally {
        # Only this launch and its start-time-verified engine can be terminated.
        # Verify its creation time and hold its handle before killing it.
        try {
            if ($null -ne $engine -and -not $engine.HasExited) { Stop-VerifiedProcessObject $engine $engineStartedAt }
            if ($null -ne $game -and -not $game.HasExited) {
                Stop-VerifiedProcessObject $game $gameStartedAt
            }
        } finally {
            if ($null -ne $server) { Stop-DidiSession $server }
        }
    }
    Write-Output "Profiler shutdown: timer witnessed during a 3000 ms read, cancellation retained, game exited without engine errors."
}
