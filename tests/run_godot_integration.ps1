param(
    [Parameter(Mandatory = $true)]
    [string]$GodotExecutable,
    [string]$Configuration = "Release",
    [string]$McpExecutable = "",
    [int]$StartupTimeoutSeconds = 30
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$sourceFixtureRoot = Join-Path $PSScriptRoot "godot_smoke"
$buildRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot "build"))
$fixtureRoot = [IO.Path]::GetFullPath((Join-Path $buildRoot "godot_phase2_smoke"))
$sessionDirectory = [IO.Path]::GetFullPath((Join-Path $fixtureRoot ".didi-sessions"))
$didiExecutable = Join-Path $repoRoot "build\$Configuration\didi.exe"
if ($McpExecutable) {
    $didiExecutable = [IO.Path]::GetFullPath($McpExecutable)
}
$stdoutPath = Join-Path $repoRoot "build\godot_integration.out"
$stderrPath = Join-Path $repoRoot "build\godot_integration.err"
$gameStdoutPath = Join-Path $repoRoot "build\godot_game_integration.out"
$gameStderrPath = Join-Path $repoRoot "build\godot_game_integration.err"
$editorEngineLogPath = Join-Path $repoRoot "build\godot_editor_engine.log"
$gameEngineLogPath = Join-Path $repoRoot "build\godot_game_engine.log"
$shutdownGameStdoutPath = Join-Path $repoRoot "build\godot_shutdown_game_integration.out"
$shutdownGameStderrPath = Join-Path $repoRoot "build\godot_shutdown_game_integration.err"
$shutdownGameEngineLogPath = Join-Path $repoRoot "build\godot_shutdown_game_engine.log"
$previousGodotBin = $env:GODOT_BIN

if (-not (Test-Path -LiteralPath $GodotExecutable)) {
    throw "Godot executable not found: $GodotExecutable"
}
$env:GODOT_BIN = [IO.Path]::GetFullPath($GodotExecutable)
if (-not (Test-Path -LiteralPath $didiExecutable)) {
    throw "Didi executable not found: $didiExecutable"
}
if (-not $fixtureRoot.StartsWith($buildRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to recreate an integration fixture outside the build directory: $fixtureRoot"
}

Remove-Item -LiteralPath $fixtureRoot -Recurse -Force -ErrorAction SilentlyContinue
Copy-Item -LiteralPath $sourceFixtureRoot -Destination $fixtureRoot -Recurse
New-Item -ItemType Directory -Path $sessionDirectory | Out-Null
$env:DIDI_SESSION_DIR = $sessionDirectory

Remove-Item -LiteralPath $stdoutPath, $stderrPath, $gameStdoutPath, $gameStderrPath, $editorEngineLogPath, $gameEngineLogPath, $shutdownGameStdoutPath, $shutdownGameStderrPath, $shutdownGameEngineLogPath -Force -ErrorAction SilentlyContinue
$godot = $null
$game = $null
$shutdownGame = $null
$editorEnginePid = 0
$gameEnginePid = 0
$shutdownGameEnginePid = 0
$editorEngineStartedAtMs = 0
$gameEngineStartedAtMs = 0
$shutdownGameEngineStartedAtMs = 0
$editorSessionToken = ""
$gameSessionToken = ""
$shutdownGameSessionToken = ""
$editorSessionId = ""
$gameSessionId = ""
$shutdownGameSessionId = ""
$integrationSucceeded = $false
$primaryFailureMessage = ""
$rawPhase5Responses = @()

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Tool-Request([int]$Id, [string]$Name, [hashtable]$Arguments) {
    return @{
        jsonrpc = "2.0"
        id = $Id
        method = "tools/call"
        params = @{ name = $Name; arguments = $Arguments }
    } | ConvertTo-Json -Compress -Depth 100
}

function Tool-Payload($Response) {
    Assert-True (-not $Response.result.isError) "Tool request $($Response.id) failed: $($Response.result.content[0].text)"
    return $Response.result.content[0].text | ConvertFrom-Json
}

function Runtime-FrameCounter($TreePayload) {
    $counterNode = @($TreePayload.scene_tree.children | Where-Object { $_.name -match '^FrameCounter_(\d+)$' })[0]
    Assert-True ($null -ne $counterNode) "Runtime tree did not expose the frame counter node."
    return [int]([regex]::Match($counterNode.name, '^FrameCounter_(\d+)$').Groups[1].Value)
}

function Process-StartedAtMs($Process) {
    try {
        return ([DateTimeOffset]::new($Process.StartTime.ToUniversalTime())).ToUnixTimeMilliseconds()
    }
    catch {
        throw "Could not verify start identity for process $($Process.Id): $($_.Exception.Message)"
    }
}

function Exact-ProcessAlive([uint64]$EnginePid, [int64]$EngineStartedAtMs) {
    if ($EnginePid -eq 0 -or $EngineStartedAtMs -le 0) { return $false }
    $process = Get-Process -Id $EnginePid -ErrorAction SilentlyContinue
    if ($null -eq $process) { return $false }
    return [Math]::Abs((Process-StartedAtMs $process) - $EngineStartedAtMs) -le 1
}

function Invoke-IdentityBoundProcessAction($VerifiedProcess, [int64]$ExpectedStartedAtMs,
                                           [scriptblock]$Action,
                                           [scriptblock]$AfterIdentityVerified = {}) {
    $safeHandle = $VerifiedProcess.SafeHandle
    $handlePinned = $false
    $safeHandle.DangerousAddRef([ref]$handlePinned)
    try {
        $actualStartedAtMs = Process-StartedAtMs $VerifiedProcess
        Assert-True ([Math]::Abs($actualStartedAtMs - $ExpectedStartedAtMs) -le 1) "Refusing to act on reused process PID $($VerifiedProcess.Id) (expected start $ExpectedStartedAtMs, found $actualStartedAtMs)."
        if ($AfterIdentityVerified) {
            & $AfterIdentityVerified $VerifiedProcess $safeHandle | Out-Null
        }
        return & $Action $VerifiedProcess
    }
    finally {
        if ($handlePinned) { $safeHandle.DangerousRelease() }
    }
}

function Stop-VerifiedProcessObject($VerifiedProcess, [int64]$ExpectedStartedAtMs,
                                    [scriptblock]$AfterIdentityVerified = {}) {
    Invoke-IdentityBoundProcessAction $VerifiedProcess $ExpectedStartedAtMs {
        param($heldProcess)
        if (-not $heldProcess.HasExited) {
            $heldProcess.Kill()
            $heldProcess.WaitForExit(5000) | Out-Null
        }
    } $AfterIdentityVerified | Out-Null
}

function Request-ExactProcessClose([uint64]$EnginePid, [int64]$EngineStartedAtMs, [int]$TimeoutSeconds) {
    $process = Get-Process -Id $EnginePid -ErrorAction SilentlyContinue
    if ($null -eq $process) { return $true }
    $accepted = Invoke-IdentityBoundProcessAction $process $EngineStartedAtMs {
        param($heldProcess)
        $heldProcess.CloseMainWindow()
    }
    Assert-True $accepted "Exact editor process $EnginePid did not accept a graceful close request."
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline -and
           (Exact-ProcessAlive $EnginePid $EngineStartedAtMs)) {
        Start-Sleep -Milliseconds 100
    }
    return -not (Exact-ProcessAlive $EnginePid $EngineStartedAtMs)
}

function Stop-RuntimeProcess($Launcher, [uint64]$EnginePid, [int64]$EngineStartedAtMs) {
    if ($EnginePid -gt 0) {
        Assert-True ($EngineStartedAtMs -gt 0) "Refusing to stop engine PID $EnginePid without its process-start identity."
        $engine = Get-Process -Id $EnginePid -ErrorAction SilentlyContinue
        if ($null -ne $engine) {
            Stop-VerifiedProcessObject $engine $EngineStartedAtMs
        }
    }
    if ($null -ne $Launcher -and -not $Launcher.HasExited -and $Launcher.Id -ne $EnginePid) {
        Stop-VerifiedProcessObject $Launcher (Process-StartedAtMs $Launcher)
    }
}

function Assert-IdentityBoundTerminationMutation() {
    $pwshExecutable = (Get-Process -Id $PID).Path
    $verified = Start-Process -FilePath $pwshExecutable `
        -ArgumentList @("-NoLogo", "-NoProfile", "-Command", "Start-Sleep -Seconds 30") `
        -PassThru -WindowStyle Hidden
    $replacement = Start-Process -FilePath $pwshExecutable `
        -ArgumentList @("-NoLogo", "-NoProfile", "-Command", "Start-Sleep -Seconds 30") `
        -PassThru -WindowStyle Hidden
    $observation = [PSCustomObject]@{ Held = $null; Lookup = $verified }
    try {
        $verifiedStart = Process-StartedAtMs $verified
        Stop-VerifiedProcessObject $verified $verifiedStart {
            param($heldProcess, $heldHandle)
            $observation.Held = $heldProcess
            $observation.Lookup = $replacement
        }
        Assert-True ([Object]::ReferenceEquals($observation.Held, $verified)) "Identity-bound terminator discarded the verified process object."
        Assert-True $verified.HasExited "Identity-bound terminator did not stop the verified process."
        Assert-True (-not $replacement.HasExited) "A replacement process was stopped after the verified target changed."
    }
    finally {
        if ($null -ne $replacement -and -not $replacement.HasExited) {
            Stop-VerifiedProcessObject $replacement (Process-StartedAtMs $replacement)
        }
        if ($null -ne $verified) { $verified.Dispose() }
        if ($null -ne $replacement) { $replacement.Dispose() }
    }
}

Assert-IdentityBoundTerminationMutation

$malformedDescriptorPath = Join-Path $sessionDirectory "malformed.json"
$oversizedDescriptorPath = Join-Path $sessionDirectory "oversized.json"
$nonFileDescriptorPath = Join-Path $sessionDirectory "not-a-file.json"
[IO.File]::WriteAllText($malformedDescriptorPath, "{")
[IO.File]::WriteAllText($oversizedDescriptorPath, "x" * 65537)
New-Item -ItemType Directory -Path $nonFileDescriptorPath | Out-Null

$prelaunchRequests = @(
    (@{ jsonrpc = "2.0"; id = 880; method = "initialize"; params = @{} } | ConvertTo-Json -Compress),
    (Tool-Request 881 "runtime_list_sessions" @{ project_path = $fixtureRoot })
)
$rawPrelaunchResponses = $prelaunchRequests | & $didiExecutable --project $fixtureRoot
$prelaunchResponses = @($rawPrelaunchResponses | Where-Object { $_ -like "{*" } | ForEach-Object { $_ | ConvertFrom-Json })
Assert-True ($LASTEXITCODE -eq 0) "Didi prelaunch discovery process exited with $LASTEXITCODE."
$prelaunchById = @{}
foreach ($response in $prelaunchResponses) { $prelaunchById[[int]$response.id] = $response }
$prelaunchDiscovery = Tool-Payload $prelaunchById[881]
$preexistingSessionIds = @($prelaunchDiscovery.sessions.session_id)
Assert-True (@($prelaunchDiscovery.diagnostics).Count -eq 3) "Malformed descriptor discovery did not report every bounded diagnostic."
Assert-True (($prelaunchDiscovery.diagnostics.error -join "`n") -match "Malformed descriptor") "Malformed JSON descriptor was not rejected."
Assert-True (($prelaunchDiscovery.diagnostics.error -join "`n") -match "64 KiB") "Oversized descriptor was not rejected before parsing."
Assert-True (($prelaunchDiscovery.diagnostics.error -join "`n") -match "regular file") "Non-file descriptor entry was not rejected."
Remove-Item -LiteralPath $malformedDescriptorPath, $oversizedDescriptorPath -Force
Remove-Item -LiteralPath $nonFileDescriptorPath -Recurse -Force

try {
    $godot = Start-Process -FilePath $GodotExecutable `
        -ArgumentList @("--editor", "--path", $fixtureRoot, "--log-file", $editorEngineLogPath) `
        -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath

    $deadline = [DateTime]::UtcNow.AddSeconds($StartupTimeoutSeconds)
    $ready = $false
    while ([DateTime]::UtcNow -lt $deadline -and -not $godot.HasExited) {
        $logs = ((Get-Content $stdoutPath, $stderrPath -ErrorAction SilentlyContinue) -join "`n")
        if ($logs -match "Named pipe server started" -and $logs -match "\[DidiSmoke\] scene opened") {
            $ready = $true
            break
        }
        Start-Sleep -Milliseconds 250
    }
    Assert-True $ready "Godot editor did not start Didi IPC and open the smoke scene within $StartupTimeoutSeconds seconds."

    $discoveryRequests = @(
        (@{ jsonrpc = "2.0"; id = 901; method = "initialize"; params = @{} } | ConvertTo-Json -Compress),
        (Tool-Request 902 "runtime_list_sessions" @{ project_path = $fixtureRoot })
    )
    $rawDiscoveryResponses = $discoveryRequests | & $didiExecutable --project $fixtureRoot
    $discoveryResponses = @($rawDiscoveryResponses | Where-Object { $_ -like "{*" } | ForEach-Object { $_ | ConvertFrom-Json })
    Assert-True ($LASTEXITCODE -eq 0) "Didi discovery process exited with $LASTEXITCODE."
    $discoveryById = @{}
    foreach ($response in $discoveryResponses) { $discoveryById[[int]$response.id] = $response }
    $discoveredSessions = @(Tool-Payload $discoveryById[902]).sessions
    $editorSession = @($discoveredSessions | Where-Object {
        $_.kind -eq "editor" -and $_.alive -and $_.project_path -ieq $fixtureRoot -and
        $preexistingSessionIds -notcontains $_.session_id
    })[0]
    Assert-True ($null -ne $editorSession) "Runtime discovery did not return a live editor session."
    Assert-True ($editorSession.endpoint -match "godot_didi_") "Discovered editor endpoint is not process-unique."
    Assert-True ($null -eq $editorSession.PSObject.Properties["token"]) "Runtime discovery leaked the editor session token."
    $editorEnginePid = [uint64]$editorSession.pid
    $editorDescriptor = Get-Content -LiteralPath (Join-Path $sessionDirectory ($editorSession.session_id + ".json")) -Raw | ConvertFrom-Json
    $editorEngineStartedAtMs = [int64]$editorDescriptor.started_at_ms
    $editorSessionId = [string]$editorSession.session_id
    $editorSessionToken = [string]$editorDescriptor.token
    Assert-True ($editorSessionToken -match '^[0-9a-f]{64}$') "Editor descriptor token did not meet the private protocol shape."
    Assert-True ($editorEngineStartedAtMs -eq [int64]$editorSession.started_at_ms) "Editor discovery and private descriptor disagreed on process-start identity."

    $game = Start-Process -FilePath $GodotExecutable `
        -ArgumentList @("--headless", "--path", $fixtureRoot, "--log-file", $gameEngineLogPath, "res://runtime_main.tscn") `
        -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput $gameStdoutPath -RedirectStandardError $gameStderrPath

    $gameSession = $null
    $gameDeadline = [DateTime]::UtcNow.AddSeconds($StartupTimeoutSeconds)
    while ([DateTime]::UtcNow -lt $gameDeadline -and -not $game.HasExited) {
        $gameDiscoveryRequests = @(
            (@{ jsonrpc = "2.0"; id = 291; method = "initialize"; params = @{} } | ConvertTo-Json -Compress),
            (Tool-Request 292 "runtime_list_sessions" @{ project_path = $fixtureRoot })
        )
        $rawGameDiscovery = $gameDiscoveryRequests | & $didiExecutable --project $fixtureRoot
        $gameDiscoveryResponses = @($rawGameDiscovery | Where-Object { $_ -like "{*" } | ForEach-Object { $_ | ConvertFrom-Json })
        if ($LASTEXITCODE -eq 0) {
            $gameDiscoveryById = @{}
            foreach ($response in $gameDiscoveryResponses) { $gameDiscoveryById[[int]$response.id] = $response }
            if ($gameDiscoveryById.ContainsKey(292) -and -not $gameDiscoveryById[292].result.isError) {
                $sessions = @(Tool-Payload $gameDiscoveryById[292]).sessions
                $gameSession = @($sessions | Where-Object {
                    $_.kind -eq "game" -and $_.alive -and $_.project_path -ieq $fixtureRoot -and
                    $preexistingSessionIds -notcontains $_.session_id -and
                    $_.session_id -ne $editorSession.session_id
                })[0]
                if ($null -ne $gameSession) { break }
            }
        }
        Start-Sleep -Milliseconds 250
    }
    Assert-True ($null -ne $gameSession) "Runtime discovery did not return a live game session."
    Assert-True ($gameSession.session_id -ne $editorSession.session_id) "Editor and game reused a session ID."
    Assert-True ($gameSession.endpoint -ne $editorSession.endpoint) "Editor and game reused an IPC endpoint."
    Assert-True ($null -eq $gameSession.PSObject.Properties["token"]) "Runtime discovery leaked the game session token."
    $gameEnginePid = [uint64]$gameSession.pid
    $gameDescriptor = Get-Content -LiteralPath (Join-Path $sessionDirectory ($gameSession.session_id + ".json")) -Raw | ConvertFrom-Json
    $gameEngineStartedAtMs = [int64]$gameDescriptor.started_at_ms
    $gameSessionId = [string]$gameSession.session_id
    $gameSessionToken = [string]$gameDescriptor.token
    Assert-True ($gameSessionToken -match '^[0-9a-f]{64}$') "Game descriptor token did not meet the private protocol shape."
    Assert-True ($gameEngineStartedAtMs -eq [int64]$gameSession.started_at_ms) "Game discovery and private descriptor disagreed on process-start identity."
    Assert-True ($gameEnginePid -gt 0 -and $editorEnginePid -gt 0) "Runtime descriptors did not publish usable engine PIDs."

    $deepEval = "0"
    foreach ($level in 1..18) { $deepEval = "[$deepEval]" }
    $oversizedEval = "'x'.repeat(300000)"
    $oversizedSource = "'" + ("x" * 2047) + "'"
    $nulExpression = "node" + [char]0 + ".get_child_count()"

    $runtimeRequests = @(
        (@{ jsonrpc = "2.0"; id = 300; method = "initialize"; params = @{} } | ConvertTo-Json -Compress),
        (Tool-Request 301 "runtime_attach_session" @{ session_id = $gameSession.session_id }),
        (Tool-Request 376 "runtime_get_session" @{}),
        (Tool-Request 377 "runtime_detach_session" @{}),
        (Tool-Request 378 "runtime_attach_session" @{ session_id = $gameSession.session_id }),
        (Tool-Request 302 "runtime_get_tree" @{ root_path = "/root/RuntimeRoot"; max_depth = 2 }),
        (Tool-Request 303 "runtime_set_paused" @{ paused = $true }),
        (Tool-Request 304 "runtime_get_tree" @{ root_path = "/root/RuntimeRoot"; max_depth = 2 }),
        (Tool-Request 322 "eval_gdscript" @{ expression = "node.get('process_priority')" }),
        (Tool-Request 305 "runtime_step" @{ frames = 1 }),
        (Tool-Request 363 "eval_gdscript" @{ expression = "node.get('process_priority')" }),
        (Tool-Request 306 "runtime_get_tree" @{ root_path = "/root/RuntimeRoot"; max_depth = 2 }),
        (Tool-Request 364 "runtime_step" @{ frames = 3 }),
        (Tool-Request 365 "eval_gdscript" @{ expression = "node.get('process_priority')" }),
        (Tool-Request 366 "runtime_get_tree" @{ root_path = "/root/RuntimeRoot"; max_depth = 2 }),
        (Tool-Request 367 "runtime_get_tree" @{ root_path = "/root/RuntimeRoot/RuntimeChild/Nested"; max_depth = 1 }),
        (Tool-Request 379 "runtime_get_tree" @{ root_path = "/root/RuntimeRoot/RuntimeChild"; max_depth = 1 }),
        (Tool-Request 307 "runtime_get_tree" @{ root_path = "/root/RuntimeRoot"; max_depth = 17 }),
        (Tool-Request 308 "runtime_get_tree" @{ root_path = ".."; max_depth = 1 }),
        (Tool-Request 309 "runtime_set_paused" @{ paused = $false }),
        (Tool-Request 310 "runtime_step" @{ frames = 1 }),
        (Tool-Request 311 "runtime_set_paused" @{ paused = $true }),
        (Tool-Request 312 "runtime_step" @{ frames = 0 }),
        (Tool-Request 313 "runtime_step" @{ frames = 61 }),
        (Tool-Request 314 "runtime_stop" @{ exit_code = 256 }),
        (Tool-Request 315 "scene_set_property" @{ target_node = "/root/RuntimeRoot"; property_name = "process_priority"; value = 12 }),
        (Tool-Request 316 "runtime_set_paused" @{ paused = $false }),
        (Tool-Request 323 "eval_gdscript" @{ expression = "node.get_child_count()"; context_node = "/root/RuntimeRoot" }),
        (Tool-Request 324 "eval_gdscript" @{ expression = "[1, 2, 3]"; context_node = "/root/RuntimeRoot" }),
        (Tool-Request 325 "eval_gdscript" @{ expression = "{'answer': 42, 'ok': true}"; context_node = "/root/RuntimeRoot" }),
        (Tool-Request 326 "eval_gdscript" @{ expression = "Vector2(3, 4)"; context_node = "/root/RuntimeRoot" }),
        (Tool-Request 327 "eval_gdscript" @{ expression = "node.set('process_priority', 99)"; context_node = "/root/RuntimeRoot" }),
        (Tool-Request 328 "eval_gdscript" @{ expression = "OS.execute('cmd', [])"; context_node = "/root/RuntimeRoot" }),
        (Tool-Request 329 "eval_gdscript" @{ expression = "node.get_child_count()"; context_node = "/root/Missing" }),
        (Tool-Request 330 "eval_gdscript" @{ expression = "node.get_child_count()"; context_node = "/root/RuntimeRoot/.." }),
        (Tool-Request 331 "eval_gdscript" @{ expression = $deepEval; context_node = "/root/RuntimeRoot" }),
        (Tool-Request 332 "eval_gdscript" @{ expression = $oversizedEval; context_node = "/root/RuntimeRoot" }),
        (Tool-Request 333 "eval_gdscript" @{ expression = "1 *"; context_node = "/root/RuntimeRoot" }),
        (Tool-Request 334 "eval_gdscript" @{ expression = "tree"; context_node = "/root/RuntimeRoot" }),
        (Tool-Request 335 "eval_gdscript" @{ expression = "1"; timeout_ms = 0 }),
        (Tool-Request 336 "eval_gdscript" @{ expression = "1"; timeout_ms = 5001 }),
        (Tool-Request 337 "eval_gdscript" @{ expression = $oversizedSource }),
        (Tool-Request 338 "eval_gdscript" @{ expression = $nulExpression }),
        (Tool-Request 339 "eval_gdscript" @{ expression = "'didi_secret_expression_42'"; context_node = "/root/RuntimeRoot" }),
        (Tool-Request 340 "eval_gdscript" @{ expression = "Vector2(INF, 0)"; context_node = "/root/RuntimeRoot" }),
        (Tool-Request 347 "eval_gdscript" @{ expression = "node.get('process_physics_priority')"; context_node = "/root/RuntimeRoot" }),
        (Tool-Request 341 "eval_gdscript" @{ expression = "str(node)"; context_node = "/root/RuntimeRoot" }),
        (Tool-Request 342 "eval_gdscript" @{ expression = "'%s' % node"; context_node = "/root/RuntimeRoot" }),
        (Tool-Request 343 "eval_gdscript" @{ expression = "node.get('dangerous_property')"; context_node = "/root/RuntimeRoot" }),
        (Tool-Request 344 "eval_gdscript" @{ expression = "node.get('dynamic_property')"; context_node = "/root/RuntimeRoot" }),
        (Tool-Request 345 "eval_gdscript" @{ expression = "node.get_child(0).get('name')"; context_node = "/root/RuntimeRoot" }),
        (Tool-Request 348 "eval_gdscript" @{ expression = "node.dynamic_property"; context_node = "/root/RuntimeRoot" }),
        (Tool-Request 349 "eval_gdscript" @{ expression = "node['dynamic_property']"; context_node = "/root/RuntimeRoot" }),
        (Tool-Request 350 "eval_gdscript" @{ expression = "'dynamic_property' in node"; context_node = "/root/RuntimeRoot" }),
        (Tool-Request 351 "eval_gdscript" @{ expression = "node.get_node('/root')"; context_node = "/root/RuntimeRoot" }),
        (Tool-Request 352 "eval_gdscript" @{ expression = "node.get_node('..')"; context_node = "/root/RuntimeRoot" }),
        (Tool-Request 353 "eval_gdscript" @{ expression = "node.get_node_or_null('/root')"; context_node = "/root/RuntimeRoot" }),
        (Tool-Request 354 "eval_gdscript" @{ expression = "node.has_node('/root')"; context_node = "/root/RuntimeRoot" }),
        (Tool-Request 355 "eval_gdscript" @{ expression = "node.get_meta('detached_node')"; context_node = "/root/RuntimeRoot" }),
        (Tool-Request 356 "eval_gdscript" @{ expression = "node.get_meta('huge_metadata')"; context_node = "/root/RuntimeRoot" }),
        (Tool-Request 357 "eval_gdscript" @{ expression = "node.get_children()"; context_node = "/root/RuntimeRoot/RuntimeChild/Nested" }),
        (Tool-Request 358 "eval_gdscript" @{ expression = "node.get_path()"; context_node = "/root/RuntimeRoot/RuntimeChild/Nested" }),
        (Tool-Request 359 "eval_gdscript" @{ expression = "node.get_child_count()"; context_node = "/root/RuntimeRoot/RuntimeChild/Nested" }),
        (Tool-Request 360 "eval_gdscript" @{ expression = "node"; context_node = "/root/RuntimeRoot/RuntimeChild/Nested" }),
        (Tool-Request 361 "eval_gdscript" @{ expression = "node.get_node('/root').get_child_count()"; context_node = "/root/RuntimeRoot" }),
        (Tool-Request 362 "eval_gdscript" @{ expression = "node.get_node('..').get_path()"; context_node = "/root/RuntimeRoot" }),
        (Tool-Request 368 "eval_gdscript" @{ expression = '"escaped \"quote\""'; context_node = "/root/RuntimeRoot" }),
        (Tool-Request 369 "eval_gdscript" @{ expression = "π"; context_node = "/root/RuntimeRoot" }),
        (Tool-Request 370 "eval_gdscript" @{ expression = "1; 2"; context_node = "/root/RuntimeRoot" }),
        (Tool-Request 371 "eval_gdscript" @{ expression = "1`n2"; context_node = "/root/RuntimeRoot" }),
        (Tool-Request 372 "eval_gdscript" @{ expression = "1 # injected comment"; context_node = "/root/RuntimeRoot" }),
        (Tool-Request 373 "eval_gdscript" @{ expression = "node . set ('process_priority', 99)"; context_node = "/root/RuntimeRoot" }),
        (Tool-Request 374 "eval_gdscript" @{ expression = "node.call('set', 'process_priority', 99)"; context_node = "/root/RuntimeRoot" }),
        (Tool-Request 375 "eval_gdscript" @{ expression = "Callable(node, 'set').call('process_priority', 99)"; context_node = "/root/RuntimeRoot" }),
        (Tool-Request 346 "eval_gdscript" @{ expression = "node.get('process_physics_priority')"; context_node = "/root/RuntimeRoot" }),
        (Tool-Request 317 "runtime_attach_session" @{ session_id = $editorSession.session_id }),
        (Tool-Request 318 "runtime_step" @{ frames = 1 }),
        (Tool-Request 319 "runtime_list_sessions" @{ project_path = $fixtureRoot }),
        (Tool-Request 320 "runtime_get_tree" @{ root_path = "/root/RuntimeRoot:frame_counter"; max_depth = 1 }),
        (Tool-Request 321 "runtime_get_tree" @{ root_path = "/root/%RuntimeRoot"; max_depth = 1 })
    )
    $rawRuntimeResponses = $runtimeRequests | & $didiExecutable --project $fixtureRoot
    $runtimeResponses = @($rawRuntimeResponses | Where-Object { $_ -like "{*" } | ForEach-Object { $_ | ConvertFrom-Json })
    Assert-True ($LASTEXITCODE -eq 0) "Didi runtime-control process exited with $LASTEXITCODE."
    Assert-True ($runtimeResponses.Count -eq $runtimeRequests.Count) "Runtime-control response count mismatch."
    $runtimeById = @{}
    foreach ($response in $runtimeResponses) { $runtimeById[[int]$response.id] = $response }

    $selectedGame = Tool-Payload $runtimeById[376]
    Assert-True ($selectedGame.session.session_id -eq $gameSession.session_id -and $selectedGame.session.kind -eq "game") "Runtime get-session did not return the selected game route."
    Assert-True ($null -eq $selectedGame.session.PSObject.Properties["token"]) "Runtime get-session leaked the game token."
    Assert-True ((Tool-Payload $runtimeById[377]).session.session_id -eq $gameSession.session_id) "Runtime detach did not report the route it released."
    Assert-True ((Tool-Payload $runtimeById[378]).handshake.status -eq "ok") "Runtime reattach did not restore the authenticated game route."

    $runtimeTree = Tool-Payload $runtimeById[302]
    Assert-True ($runtimeTree.scene_tree.path -eq "/root/RuntimeRoot") "Runtime tree root was not canonical."
    Assert-True ($runtimeTree.scene_tree.child_count -eq 2) "Runtime tree did not report child_count."
    Assert-True ($runtimeTree.node_count -eq 5 -and $runtimeTree.truncated) "Runtime tree bounds metadata did not report the deliberately large truncated subtree."
    Assert-True ((Tool-Payload $runtimeById[303]).paused -eq $true) "Game pause was not verified."
    $beforeStep = Runtime-FrameCounter (Tool-Payload $runtimeById[304])
    $step = Tool-Payload $runtimeById[305]
    $afterStepTree = Tool-Payload $runtimeById[306]
    $afterStep = Runtime-FrameCounter $afterStepTree
    $beforeStepEval = Tool-Payload $runtimeById[322]
    $afterStepEval = Tool-Payload $runtimeById[363]
    Assert-True ($afterStep -eq ($beforeStep + 1)) "One-frame step advanced from $beforeStep to $afterStep."
    Assert-True ($beforeStepEval.value -eq $beforeStep) "Pre-step expression observed $($beforeStepEval.value), not live frame $beforeStep."
    Assert-True ($afterStepEval.value -eq ($beforeStepEval.value + 1) -and $afterStepEval.value -eq $afterStep) "Native scalar expression did not advance exactly once with the processed frame."
    Assert-True ($step.frames -eq 1 -and $step.paused -eq $true -and $afterStepTree.paused -eq $true) "Step did not finish re-paused."
    Assert-True ($beforeStepEval.context_node -eq "/root/RuntimeRoot" -and $afterStepEval.context_node -eq "/root/RuntimeRoot" -and $beforeStepEval.session_kind -eq "game" -and $afterStepEval.session_kind -eq "game") "Live frame expressions used incorrect game context or provenance."
    $multiStep = Tool-Payload $runtimeById[364]
    $multiStepEval = Tool-Payload $runtimeById[365]
    $multiStepTree = Tool-Payload $runtimeById[366]
    $multiStepFrame = Runtime-FrameCounter $multiStepTree
    Assert-True ($multiStep.frames -eq 3 -and $multiStep.paused -eq $true) "Three-frame step did not report exact, re-paused completion."
    Assert-True ($multiStepFrame -eq ($afterStep + 3) -and $multiStepEval.value -eq $multiStepFrame) "Three-frame step did not advance live state by exactly three frames."
    Assert-True ($multiStepTree.paused -eq $true) "Three-frame step left the game running."
    $cappedTree = Tool-Payload $runtimeById[367]
    $cappedTreeBytes = [Text.Encoding]::UTF8.GetByteCount([string]$runtimeById[367].result.content[0].text)
    Assert-True ($cappedTree.node_count -lt 10000 -and $cappedTree.max_nodes -eq 10000 -and $cappedTree.truncated) "Runtime tree did not stop at the serialized response budget before the node cap."
    Assert-True ($cappedTree.max_response_bytes -eq 262144 -and $cappedTreeBytes -le $cappedTree.max_response_bytes) "Runtime tree public payload was $cappedTreeBytes bytes and exceeded its advertised 256 KiB serialized response budget after session provenance."
    Assert-True ($cappedTree.scene_tree.children_truncated -eq $true) "Runtime tree response-budget metadata did not identify the truncated child list."
    $fieldBoundTree = Tool-Payload $runtimeById[379]
    $oversizedNameNode = @($fieldBoundTree.scene_tree.children | Where-Object { $_.name -like "TreeName_*" })[0]
    Assert-True ($null -ne $oversizedNameNode) "Runtime tree did not expose the oversized-name probe."
    Assert-True ($oversizedNameNode.name_truncated -eq $true -and [Text.Encoding]::UTF8.GetByteCount([string]$oversizedNameNode.name) -le 1024) "Runtime tree did not UTF-8 truncate an oversized node name explicitly."
    Assert-True ($oversizedNameNode.path_truncated -eq $true -and [Text.Encoding]::UTF8.GetByteCount([string]$oversizedNameNode.path) -le 4096) "Runtime tree did not UTF-8 truncate an oversized node path explicitly."
    foreach ($rejectedId in 307, 308, 310, 312, 313, 314, 315, 318, 320, 321) {
        Assert-True ([bool]$runtimeById[$rejectedId].result.isError) "Runtime rejection $rejectedId returned fake success."
    }
    Assert-True ((Tool-Payload $runtimeById[316]).paused -eq $false) "Game resume was not verified."
    Assert-True ((Tool-Payload $runtimeById[323]).value -eq 2) "Game expression child count was incorrect."
    Assert-True (@((Tool-Payload $runtimeById[324]).value).Count -eq 3) "Game expression array was not preserved."
    Assert-True ((Tool-Payload $runtimeById[325]).value.answer -eq 42) "Game expression dictionary was not preserved."
    $gameVector = Tool-Payload $runtimeById[326]
    Assert-True ($gameVector.value.type -eq "Vector2" -and $gameVector.value.x -eq 3 -and $gameVector.value.y -eq 4) "Game Vector2 conversion was incorrect."
    Assert-True ($gameVector.read_only -eq $true -and $gameVector.sandbox_profile -eq "expression_const_v1" -and $gameVector.execution_mode -eq "live") "Game expression provenance was incomplete."
    foreach ($rejectedId in 327, 328, 329, 330, 331, 332, 333, 334, 335, 336, 337, 338, 340, 341, 342, 343, 344, 345, 348, 349, 350, 351, 352, 353, 354, 355, 356, 357, 361, 362) {
        Assert-True ([bool]$runtimeById[$rejectedId].result.isError) "Game expression rejection $rejectedId returned fake success."
    }
    foreach ($policyRejectedId in 350, 351, 352, 353, 354, 355, 356, 357, 361, 362) {
        Assert-True ($runtimeById[$policyRejectedId].result.content[0].text -match "Invalid expression request") "Game policy rejection $policyRejectedId reached the engine."
    }
    Assert-True ($runtimeById[331].result.content[0].text -match "nesting depth") "Deep game result failed for the wrong reason."
    Assert-True ($runtimeById[332].result.content[0].text -match "256 KiB") "Oversized game result failed for the wrong reason."
    Assert-True ($runtimeById[333].result.content[0].text -match "parse failed") "Game parse error was not structured."
    Assert-True ($runtimeById[334].result.content[0].text -match "unsupported non-Node Object") "Unsupported game Object did not fail as a typed result."
    Assert-True ($runtimeById[340].result.content[0].text -match "non-finite") "Non-finite vector components did not fail as a typed result."
    Assert-True ((Tool-Payload $runtimeById[346]).value -eq (Tool-Payload $runtimeById[347]).value) "Rejected game expressions executed a scripted getter or string callback."
    Assert-True ((Tool-Payload $runtimeById[368]).value -eq 'escaped "quote"') "Escaped quote expression did not round-trip safely."
    foreach ($injectionRejectedId in 369, 370, 371, 372, 373, 374, 375) {
        Assert-True ([bool]$runtimeById[$injectionRejectedId].result.isError) "Expression injection $injectionRejectedId returned fake success."
        Assert-True ($runtimeById[$injectionRejectedId].result.content[0].text -match "Invalid expression request") "Expression injection $injectionRejectedId reached the engine."
    }
    $gameSecret = Tool-Payload $runtimeById[339]
    Assert-True ($gameSecret.value -eq "didi_secret_expression_42") "Harmless string expression was not evaluated."
    Assert-True ($null -eq $gameSecret.PSObject.Properties["expression"]) "Game expression source escaped in response provenance."
    Assert-True ((Tool-Payload $runtimeById[358]).value -eq "/root/RuntimeRoot/RuntimeChild/Nested") "Safe game path scalar was rejected or incorrect."
    Assert-True ((Tool-Payload $runtimeById[359]).value -eq 10001) "Safe game child-count scalar did not inspect the large scene in constant space."
    $gameNodeSummary = Tool-Payload $runtimeById[360]
    Assert-True ($gameNodeSummary.value.type -eq "Node" -and $gameNodeSummary.value.path -eq "/root/RuntimeRoot/RuntimeChild/Nested") "In-scope game Node summary failed ancestry confinement."
    Assert-True ((Tool-Payload $runtimeById[317]).session.session_id -eq $editorSession.session_id) "Could not reattach to the editor."
    $stillLiveGame = @((Tool-Payload $runtimeById[319]).sessions | Where-Object { $_.session_id -eq $gameSession.session_id -and $_.alive })[0]
    Assert-True ($null -ne $stillLiveGame) "Reattaching to the editor terminated the game session."

    $tooDeep = @{ leaf = "value" }
    foreach ($level in 1..18) { $tooDeep = @{ nested = $tooDeep } }

    $requests = @(
        (@{ jsonrpc = "2.0"; id = 1; method = "initialize"; params = @{} } | ConvertTo-Json -Compress),
        (Tool-Request 900 "runtime_attach_session" @{ session_id = $editorSession.session_id }),
        (Tool-Request 130 "eval_gdscript" @{ expression = "node.get('process_priority')"; context_node = "/root/SmokeRoot/Subject" }),
        (Tool-Request 131 "eval_gdscript" @{ expression = "node.get_child_count()" }),
        (Tool-Request 132 "eval_gdscript" @{ expression = "[1, 2, 3]" }),
        (Tool-Request 133 "eval_gdscript" @{ expression = "{'answer': 42, 'ok': true}" }),
        (Tool-Request 134 "eval_gdscript" @{ expression = "Vector2(3, 4)" }),
        (Tool-Request 135 "eval_gdscript" @{ expression = "node.get('name')" }),
        (Tool-Request 136 "eval_gdscript" @{ expression = "node"; context_node = "/root/SmokeRoot/Subject" }),
        (Tool-Request 137 "eval_gdscript" @{ expression = "tree" }),
        (Tool-Request 138 "eval_gdscript" @{ expression = "node.set('process_priority', 99)"; context_node = "/root/SmokeRoot/Subject" }),
        (Tool-Request 139 "eval_gdscript" @{ expression = "node.call('queue_free')"; context_node = "/root/SmokeRoot/Subject" }),
        (Tool-Request 140 "eval_gdscript" @{ expression = "node.get_child_count()"; context_node = "/root/SmokeRoot/Missing" }),
        (Tool-Request 141 "eval_gdscript" @{ expression = "node.get_child_count()"; context_node = "/root/SmokeRoot/Subject/.." }),
        (Tool-Request 142 "eval_gdscript" @{ expression = "1 *" }),
        (Tool-Request 143 "eval_gdscript" @{ expression = $deepEval }),
        (Tool-Request 144 "eval_gdscript" @{ expression = $oversizedEval }),
        (Tool-Request 145 "eval_gdscript" @{ expression = "1"; timeout_ms = 0 }),
        (Tool-Request 146 "eval_gdscript" @{ expression = "1"; timeout_ms = 5001 }),
        (Tool-Request 147 "eval_gdscript" @{ expression = $oversizedSource }),
        (Tool-Request 148 "eval_gdscript" @{ expression = $nulExpression }),
        (Tool-Request 149 "eval_gdscript" @{ expression = "O\u0053.execute('cmd', [])" }),
        (Tool-Request 150 "eval_gdscript" @{ expression = "'didi_secret_expression_42'" }),
        (Tool-Request 156 "eval_gdscript" @{ expression = "node.get('process_physics_priority')"; context_node = "/root/SmokeRoot/Container/MaliciousSubject" }),
        (Tool-Request 151 "eval_gdscript" @{ expression = "str(node)"; context_node = "/root/SmokeRoot/Container/MaliciousSubject" }),
        (Tool-Request 152 "eval_gdscript" @{ expression = "'%s' % node"; context_node = "/root/SmokeRoot/Container/MaliciousSubject" }),
        (Tool-Request 153 "eval_gdscript" @{ expression = "node.get('dangerous_property')"; context_node = "/root/SmokeRoot/Container/MaliciousSubject" }),
        (Tool-Request 154 "eval_gdscript" @{ expression = "node.get('dynamic_property')"; context_node = "/root/SmokeRoot/Container/MaliciousSubject" }),
        (Tool-Request 157 "eval_gdscript" @{ expression = "node.dangerous_property"; context_node = "/root/SmokeRoot/Container/MaliciousSubject" }),
        (Tool-Request 158 "eval_gdscript" @{ expression = "node['dynamic_property']"; context_node = "/root/SmokeRoot/Container/MaliciousSubject" }),
        (Tool-Request 159 "eval_gdscript" @{ expression = "'dynamic_property' in node"; context_node = "/root/SmokeRoot/Container/MaliciousSubject" }),
        (Tool-Request 160 "eval_gdscript" @{ expression = "node.get_node('/root')"; context_node = "/root/SmokeRoot/Container/MaliciousSubject" }),
        (Tool-Request 161 "eval_gdscript" @{ expression = "node.get_node('..')"; context_node = "/root/SmokeRoot/Container/MaliciousSubject" }),
        (Tool-Request 162 "eval_gdscript" @{ expression = "node.get_node_or_null('/root')"; context_node = "/root/SmokeRoot/Container/MaliciousSubject" }),
        (Tool-Request 163 "eval_gdscript" @{ expression = "node.has_node('/root')"; context_node = "/root/SmokeRoot/Container/MaliciousSubject" }),
        (Tool-Request 164 "eval_gdscript" @{ expression = "node.get_meta('detached_node')"; context_node = "/root/SmokeRoot/Container/MaliciousSubject" }),
        (Tool-Request 165 "eval_gdscript" @{ expression = "node.get_meta('huge_metadata')"; context_node = "/root/SmokeRoot/Container/MaliciousSubject" }),
        (Tool-Request 166 "eval_gdscript" @{ expression = "node.get_children()"; context_node = "/root/SmokeRoot/Container/MaliciousSubject" }),
        (Tool-Request 167 "eval_gdscript" @{ expression = "node.get_path()"; context_node = "/root/SmokeRoot/Container/MaliciousSubject" }),
        (Tool-Request 168 "eval_gdscript" @{ expression = "node.get_child_count()"; context_node = "/root/SmokeRoot/Container/MaliciousSubject" }),
        (Tool-Request 169 "eval_gdscript" @{ expression = "node.get_node('/root').get_child_count()"; context_node = "/root/SmokeRoot/Container/MaliciousSubject" }),
        (Tool-Request 170 "eval_gdscript" @{ expression = "node.get_node('..').get_path()"; context_node = "/root/SmokeRoot/Container/MaliciousSubject" }),
        (Tool-Request 155 "eval_gdscript" @{ expression = "node.get('process_physics_priority')"; context_node = "/root/SmokeRoot/Container/MaliciousSubject" }),
        (Tool-Request 2 "scene_get_hierarchy" @{ root_path = "/root"; max_depth = 2 }),
        (Tool-Request 3 "scene_get_property" @{ target_node = "/root/SmokeRoot/Subject"; property_name = "process_priority" }),
        (Tool-Request 4 "scene_set_property" @{ target_node = "/root/SmokeRoot/Subject"; property_name = "process_priority"; value = 12 }),
        (Tool-Request 5 "scene_get_property" @{ target_node = "/root/SmokeRoot/Subject"; property_name = "process_priority" }),
        (Tool-Request 6 "editor_undo" @{}),
        (Tool-Request 7 "scene_get_property" @{ target_node = "/root/SmokeRoot/Subject"; property_name = "process_priority" }),
        (Tool-Request 8 "editor_redo" @{}),
        (Tool-Request 9 "scene_get_property" @{ target_node = "/root/SmokeRoot/Subject"; property_name = "process_priority" }),
        (Tool-Request 10 "scene_instantiate_node" @{ node_type = "Node"; parent_path = "/root/SmokeRoot"; name = "Spawned"; properties = @{ process_priority = 21 } }),
        (Tool-Request 11 "scene_get_property" @{ target_node = "/root/SmokeRoot/Spawned"; property_name = "process_priority" }),
        (Tool-Request 12 "scene_remove_node" @{ target_node = "/root/SmokeRoot/Spawned" }),
        (Tool-Request 13 "editor_undo" @{}),
        (Tool-Request 14 "scene_duplicate_node" @{ target_node = "/root/SmokeRoot/Spawned" }),
        (Tool-Request 15 "scene_reparent_node" @{ target_node = "/root/SmokeRoot/SpawnedCopy"; new_parent_path = "/root/SmokeRoot/Container"; keep_global_transform = $true }),
        (Tool-Request 16 "scene_get_hierarchy" @{ root_path = "/root"; max_depth = 3 }),
        (Tool-Request 17 "editor_undo" @{}),
        (Tool-Request 18 "scene_get_hierarchy" @{ root_path = "/root"; max_depth = 3 }),
        (Tool-Request 19 "viewport_capture_frame" @{ camera_identifier = "active_editor_view" }),
        (Tool-Request 20 "signal_connect" @{ emitter_node = "/root/SmokeRoot"; signal_name = "tree_entered"; target_node = "/root/SmokeRoot/Subject"; target_method = "queue_free" }),
        (Tool-Request 21 "scene_get_property" @{ target_node = "/root/SmokeRoot/Missing"; property_name = "name" }),
        (Tool-Request 22 "scene_get_property" @{ target_node = "/root/SmokeRoot/Subject"; property_name = "phase_one_typo" }),
        (Tool-Request 23 "scene_set_property" @{ target_node = "/root/SmokeRoot/Subject"; property_name = "process_priority"; value = "wrong-type" }),
        (Tool-Request 24 "scene_get_property" @{ target_node = "/root/SmokeRoot/Subject"; property_name = "process_priority" }),
        (Tool-Request 25 "scene_instantiate_node" @{ node_type = "Node"; parent_path = "/root/SmokeRoot"; name = "InvalidSpawn"; properties = @{ phase_one_typo = 1 } }),
        (Tool-Request 26 "scene_remove_node" @{ target_node = "/root/SmokeRoot/Subject" }),
        (Tool-Request 27 "editor_undo" @{}),
        (Tool-Request 28 "scene_get_hierarchy" @{ root_path = "/root"; max_depth = 2 }),
        (Tool-Request 29 "scene_get_hierarchy" @{ root_path = "/root/SmokeRoot/Container"; max_depth = 1 }),
        (Tool-Request 30 "scene_reparent_node" @{ target_node = "/root/SmokeRoot/Subject"; new_parent_path = "/root/SmokeRoot/Container"; keep_global_transform = $true }),
        (Tool-Request 31 "editor_undo" @{}),
        (Tool-Request 32 "scene_get_hierarchy" @{ root_path = "/root"; max_depth = 2 }),
        (Tool-Request 33 "scene_get_hierarchy" @{ root_path = "Container"; max_depth = 1 }),
        (Tool-Request 34 "scene_get_hierarchy" @{ root_path = ".."; max_depth = 0 }),
        (Tool-Request 35 "scene_get_hierarchy" @{ root_path = "../.."; max_depth = 0 }),
        (Tool-Request 36 "scene_instantiate_node" @{ node_type = "Resource"; parent_path = "/root"; name = "InvalidNonNode" }),
        (Tool-Request 37 "scene_remove_node" @{ target_node = "/root" }),
        (Tool-Request 38 "scene_duplicate_node" @{ target_node = "/root" }),
        (Tool-Request 39 "scene_reparent_node" @{ target_node = "/root"; new_parent_path = "/root/SmokeRoot/Container" }),
        (Tool-Request 40 "scene_reparent_node" @{ target_node = "/root/SmokeRoot/SpawnedCopy"; new_parent_path = "/root/SmokeRoot/Container" }),
        (Tool-Request 41 "scene_reparent_node" @{ target_node = "/root/SmokeRoot/Container"; new_parent_path = "/root/SmokeRoot/Container/SpawnedCopy" }),
        (Tool-Request 42 "scene_reparent_node" @{ target_node = "/root/SmokeRoot/Subject"; new_parent_path = "/root/SmokeRoot/Subject" }),
        (Tool-Request 43 "scene_get_hierarchy" @{ root_path = "/root"; max_depth = 3 }),
        (Tool-Request 44 "project_set_setting" @{ setting = "didi_phase2/nested"; value = @{ enabled = $true; count = 3; names = @("alpha", "beta"); nested = @{ ratio = 0.5 } } }),
        (Tool-Request 45 "project_get_setting" @{ setting = "didi_phase2/nested" }),
        (Tool-Request 46 "project_set_setting" @{ setting = "didi_phase2/nested"; remove = $true }),
        (Tool-Request 47 "project_get_setting" @{ setting = "didi_phase2/nested" }),
        (Tool-Request 48 "project_set_setting" @{ setting = "autoload/Blocked"; value = "res://blocked.gd" }),
        (Tool-Request 49 "project_set_setting" @{ setting = "input/blocked"; value = @{ deadzone = 0.2; events = @() } }),
        (Tool-Request 50 "project_set_setting" @{ setting = "didi_phase2/too_deep"; value = $tooDeep }),
        (Tool-Request 51 "script_attach_to_node" @{ target_node = "/root/SmokeRoot/Subject"; script_path = "res://subject.gd" }),
        (Tool-Request 52 "script_detach_from_node" @{ target_node = "/root/SmokeRoot/Subject" }),
        (Tool-Request 53 "editor_undo" @{}),
        (Tool-Request 54 "editor_redo" @{}),
        (Tool-Request 55 "script_detach_from_node" @{ target_node = "/root/SmokeRoot/Subject" }),
        (Tool-Request 56 "scene_add_to_group" @{ target_node = "/root/SmokeRoot/Subject"; group = "phase_two"; persistent = $true }),
        (Tool-Request 57 "scene_list_groups" @{ target_node = "/root/SmokeRoot/Subject" }),
        (Tool-Request 58 "scene_get_group_members" @{ group = "phase_two" }),
        (Tool-Request 59 "scene_remove_from_group" @{ target_node = "/root/SmokeRoot/Subject"; group = "phase_two" }),
        (Tool-Request 60 "scene_get_group_members" @{ group = "phase_two" }),
        (Tool-Request 61 "editor_undo" @{}),
        (Tool-Request 62 "scene_get_group_members" @{ group = "phase_two" }),
        (Tool-Request 63 "scene_add_to_group" @{ target_node = "/root/SmokeRoot/Subject"; group = "phase_two"; persistent = $true }),
        (Tool-Request 64 "project_list_autoloads" @{}),
        (Tool-Request 65 "project_set_autoload" @{ name = "PhaseTwo"; path = "res://subject.gd"; singleton = $true }),
        (Tool-Request 66 "project_list_autoloads" @{}),
        (Tool-Request 67 "project_set_autoload" @{ name = "PhaseTwo"; path = "res://subject.gd"; singleton = $true }),
        (Tool-Request 68 "project_set_autoload" @{ name = "PhaseTwo"; path = "res://subject.gd"; singleton = $false; replace = $true }),
        (Tool-Request 69 "project_list_autoloads" @{}),
        (Tool-Request 70 "project_remove_autoload" @{ name = "PhaseTwo" }),
        (Tool-Request 71 "project_list_autoloads" @{}),
        (Tool-Request 72 "project_remove_autoload" @{ name = "PhaseTwo" }),
        (Tool-Request 73 "project_set_autoload" @{ name = "not-valid"; path = "res://subject.gd" }),
        (Tool-Request 74 "project_set_autoload" @{ name = "Missing"; path = "res://missing.gd" }),
        (Tool-Request 75 "project_list_input_actions" @{}),
        (Tool-Request 76 "project_set_input_action" @{ action = "phase_two_jump"; deadzone = 0.35; events = @(
            @{ type = "key"; keycode = 32; shift = $true },
            @{ type = "mouse_button"; button_index = 1; device = 2 },
            @{ type = "joypad_button"; button_index = 0; device = 1 },
            @{ type = "joypad_motion"; axis = 0; axis_value = -0.75; device = 3 }
        ) }),
        (Tool-Request 77 "project_list_input_actions" @{}),
        (Tool-Request 78 "project_set_input_action" @{ action = "phase_two_jump"; events = @() }),
        (Tool-Request 79 "project_set_input_action" @{ action = "phase_two_jump"; deadzone = 0.1; events = @(); replace = $true }),
        (Tool-Request 80 "project_list_input_actions" @{}),
        (Tool-Request 81 "project_remove_input_action" @{ action = "phase_two_jump" }),
        (Tool-Request 82 "project_list_input_actions" @{}),
        (Tool-Request 83 "project_set_input_action" @{ action = "bad_event"; events = @(@{ type = "touch" }) }),
        (Tool-Request 84 "project_set_input_action" @{ action = "bad_deadzone"; deadzone = 1.5; events = @() }),
        (Tool-Request 85 "project_set_input_action" @{ action = "empty_key"; events = @(@{ type = "key" }) }),
        (Tool-Request 86 "scene_close" @{}),
        (Tool-Request 87 "scene_pack_branch" @{ target_node = "/root/SmokeRoot/Container"; scene_path = "res://packed_branch.tscn" }),
        (Tool-Request 88 "scene_pack_branch" @{ target_node = "/root/SmokeRoot/Container"; scene_path = "res://packed_branch.tscn" }),
        (Tool-Request 89 "scene_open" @{ scene_path = "res://packed_branch.tscn" }),
        (Tool-Request 90 "scene_get_hierarchy" @{ root_path = "/root"; max_depth = 2 }),
        (Tool-Request 91 "scene_close" @{ discard_unsaved = $true }),
        (Tool-Request 92 "scene_create" @{ scene_path = "res://created_phase2.tscn"; root_type = "Node2D"; root_name = "Created" }),
        (Tool-Request 93 "scene_get_hierarchy" @{ root_path = "/root"; max_depth = 1 }),
        (Tool-Request 94 "scene_create" @{ scene_path = "res://created_phase2.tscn"; root_type = "Control"; root_name = "Blocked" }),
        (Tool-Request 95 "scene_open" @{ scene_path = "res://packed_branch.tscn" }),
        (Tool-Request 96 "scene_close" @{ discard_unsaved = $true }),
        (Tool-Request 97 "scene_open" @{ scene_path = "res://missing_scene.tscn" }),
        (Tool-Request 98 "scene_create" @{ scene_path = "C:\\unsafe.tscn" }),
        (Tool-Request 99 "scene_create" @{ scene_path = "res://created_phase2.tscn"; root_type = "Control"; root_name = "Replaced"; overwrite = $true }),
        (Tool-Request 100 "scene_get_hierarchy" @{ root_path = "/root"; max_depth = 1 }),
        (Tool-Request 101 "scene_close" @{ discard_unsaved = $true }),
        (Tool-Request 102 "scene_open" @{ scene_path = "res://main.tscn" }),
        (Tool-Request 103 "script_attach_to_node" @{ target_node = "/root/SmokeRoot/Subject"; script_path = "res://missing.gd" }),
        (Tool-Request 104 "script_attach_to_node" @{ target_node = "/root/SmokeRoot/Subject"; script_path = "res://main.tscn" }),
        (Tool-Request 105 "script_attach_to_node" @{ target_node = "/root/SmokeRoot/Subject"; script_path = "res://incompatible.gd" }),
        (Tool-Request 106 "scene_add_to_group" @{ target_node = "/root/SmokeRoot/Subject"; group = "phase_two_transient"; persistent = $false }),
        (Tool-Request 107 "scene_remove_from_group" @{ target_node = "/root/SmokeRoot/Subject"; group = "phase_two_transient" }),
        (Tool-Request 108 "editor_undo" @{}),
        (Tool-Request 109 "scene_pack_branch" @{ target_node = "/root/SmokeRoot/Subject"; scene_path = "res://transient_probe.tscn" }),
        (Tool-Request 110 "scene_remove_from_group" @{ target_node = "/root/SmokeRoot/Subject"; group = "phase_two_transient" }),
        (Tool-Request 111 "scene_close" @{ discard_unsaved = $true }),
        (Tool-Request 112 "scene_create" @{ scene_path = "res:////escape.tscn" }),
        (Tool-Request 113 "runtime_read_logs" @{ cursor = 0; limit = 500; minimum_level = "debug" })
    )

    $rawResponses = $requests | & $didiExecutable --project $fixtureRoot
    $responses = @($rawResponses | Where-Object { $_ -like "{*" } | ForEach-Object { $_ | ConvertFrom-Json })
    Assert-True ($LASTEXITCODE -eq 0) "Didi MCP process exited with $LASTEXITCODE."
    Assert-True ($responses.Count -eq $requests.Count) "Expected $($requests.Count) JSON-RPC responses, received $($responses.Count)."

    $byId = @{}
    foreach ($response in $responses) { $byId[[int]$response.id] = $response }
    $attached = Tool-Payload $byId[900]
    Assert-True ($attached.handshake.status -eq "ok") "Runtime attach did not complete the authenticated handshake."
    Assert-True ($attached.session.session_id -eq $editorSession.session_id) "Runtime attach selected the wrong editor session."

    $scalarEval = Tool-Payload $byId[130]
    Assert-True ($scalarEval.value -eq 7 -and $scalarEval.value_type -eq "int") "Editor scalar expression was incorrect."
    Assert-True ($scalarEval.context_node -eq "/root/SmokeRoot/Subject" -and $scalarEval.session_kind -eq "editor") "Editor expression context or provenance was incorrect."
    Assert-True ((Tool-Payload $byId[131]).value -eq 2) "Editor default-context child count was incorrect."
    Assert-True (@((Tool-Payload $byId[132]).value).Count -eq 3) "Editor expression array was not preserved."
    Assert-True ((Tool-Payload $byId[133]).value.answer -eq 42) "Editor expression dictionary was not preserved."
    $editorVector = Tool-Payload $byId[134]
    Assert-True ($editorVector.value.type -eq "Vector2" -and $editorVector.value.x -eq 3 -and $editorVector.value.y -eq 4) "Editor Vector2 conversion was incorrect."
    Assert-True ((Tool-Payload $byId[135]).value -eq "SmokeRoot") "Editor default expression context was not the edited scene root."
    $nodeSummary = Tool-Payload $byId[136]
    Assert-True ($nodeSummary.value.type -eq "Node" -and $nodeSummary.value.path -eq "/root/SmokeRoot/Subject") "Node result did not use a bounded typed summary."
    foreach ($rejectedId in 137, 138, 139, 140, 141, 142, 143, 144, 145, 146, 147, 148, 149) {
        Assert-True ([bool]$byId[$rejectedId].result.isError) "Editor expression rejection $rejectedId returned fake success."
    }
    Assert-True ($byId[137].result.content[0].text -match "unsupported non-Node Object") "Unsupported editor Object did not fail as a typed result."
    Assert-True ($byId[142].result.content[0].text -match "parse failed") "Editor parse error was not structured."
    Assert-True ($byId[143].result.content[0].text -match "nesting depth") "Deep editor result failed for the wrong reason."
    Assert-True ($byId[144].result.content[0].text -match "256 KiB") "Oversized editor result failed for the wrong reason."
    $secretEval = Tool-Payload $byId[150]
    Assert-True ($secretEval.value -eq "didi_secret_expression_42" -and $secretEval.read_only -eq $true -and $secretEval.sandbox_profile -eq "expression_const_v1") "Editor string expression or provenance was incorrect."
    Assert-True ($null -eq $secretEval.PSObject.Properties["expression"]) "Editor expression source escaped in response provenance."
    foreach ($rejectedId in 151, 152, 153, 154, 157, 158, 159, 160, 161, 162, 163, 164, 165, 166, 169, 170) {
        Assert-True ([bool]$byId[$rejectedId].result.isError) "Editor callback rejection $rejectedId returned fake success."
    }
    foreach ($policyRejectedId in 159, 160, 161, 162, 163, 164, 165, 166, 169, 170) {
        Assert-True ($byId[$policyRejectedId].result.content[0].text -match "Invalid expression request") "Editor policy rejection $policyRejectedId reached the engine."
    }
    Assert-True ((Tool-Payload $byId[167]).value -match "SmokeRoot/Container/MaliciousSubject$") "Safe editor path scalar was rejected or incorrect."
    Assert-True ((Tool-Payload $byId[168]).value -eq 1024) "Safe editor child-count scalar did not inspect the large scene in constant space."
    Assert-True ((Tool-Payload $byId[155]).value -eq (Tool-Payload $byId[156]).value) "Rejected editor expressions executed a scripted getter or string callback."

    $hierarchy = Tool-Payload $byId[2]
    Assert-True ($hierarchy.execution_mode -eq "live") "Hierarchy was not attributed to live execution."
    Assert-True ($hierarchy.scene_tree.path -eq "/root/SmokeRoot") "Hierarchy root path is not editor-independent: $($hierarchy.scene_tree.path)"
    Assert-True ($hierarchy.scene_tree.children[0].path -eq "/root/SmokeRoot/Subject") "Hierarchy child path is not editor-independent: $($hierarchy.scene_tree.children[0].path)"

    Assert-True ((Tool-Payload $byId[3]).value -eq 7) "Fixture property did not start at 7; actual=$((Tool-Payload $byId[3]).value)."
    Assert-True ((Tool-Payload $byId[4]).undo_redo_registered) "Property mutation did not report a real UndoRedo transaction."
    Assert-True ((Tool-Payload $byId[5]).value -eq 12) "Property mutation was not observable."
    Assert-True ((Tool-Payload $byId[7]).value -eq 7) "Undo did not restore the property."
    Assert-True ((Tool-Payload $byId[9]).value -eq 12) "Redo did not restore the changed property."

    $instantiated = Tool-Payload $byId[10]
    Assert-True ($instantiated.node_path -eq "/root/SmokeRoot/Spawned") "Instantiate returned a non-portable node path."
    Assert-True ((Tool-Payload $byId[11]).value -eq 21) "Instantiate ignored initial scalar properties."
    $duplicated = Tool-Payload $byId[14]
    Assert-True ($duplicated.duplicated_node -eq "/root/SmokeRoot/SpawnedCopy") "Duplicate did not return its deterministic logical path: $($duplicated.duplicated_node)"
    $reparentedHierarchy = Tool-Payload $byId[16]
    $container = @($reparentedHierarchy.scene_tree.children | Where-Object name -eq "Container")[0]
    Assert-True (@($container.children.name) -contains "SpawnedCopy") "Reparent did not move the duplicate under Container."
    $restoredHierarchy = Tool-Payload $byId[18]
    Assert-True (@($restoredHierarchy.scene_tree.children.name) -contains "SpawnedCopy") "Undo did not restore the duplicate's original parent."

    $capture = $byId[19].result
    Assert-True (-not $capture.isError) "Live viewport capture failed: $($capture.content[0].text)"
    $image = @($capture.content | Where-Object type -eq "image")
    $captureMetadataContent = @($capture.content | Where-Object type -eq "text")
    Assert-True ($captureMetadataContent.Count -eq 1) "Viewport capture did not return exactly one metadata payload."
    $captureMetadata = $captureMetadataContent[0].text | ConvertFrom-Json
    Assert-True ($captureMetadata.execution_mode -eq "live") "Viewport capture metadata did not identify live execution."
    Assert-True ($captureMetadata.is_live_frame -eq $true) "Viewport capture metadata did not identify a live frame."
    Assert-True ($captureMetadata.source -eq "godot_editor_viewport_texture") "Viewport capture source was not the editor viewport texture."
    Assert-True ($image.Count -eq 1) "Viewport capture did not return exactly one image."
    Assert-True ($image[0].mimeType -eq "image/png") "Viewport capture MIME type is not PNG."
    Assert-True ($image[0].data.StartsWith("iVBORw0K")) "Viewport capture does not contain a PNG signature."
    $pngBytes = [Convert]::FromBase64String($image[0].data)
    $pngWidth = [System.Net.IPAddress]::NetworkToHostOrder([BitConverter]::ToInt32($pngBytes, 16))
    $pngHeight = [System.Net.IPAddress]::NetworkToHostOrder([BitConverter]::ToInt32($pngBytes, 20))
    Assert-True ($captureMetadata.resolution.width -eq $pngWidth) "Viewport metadata width did not match PNG IHDR width."
    Assert-True ($captureMetadata.resolution.height -eq $pngHeight) "Viewport metadata height did not match PNG IHDR height."

    # Re-open the canonical fixture and obtain a baseline whose process-local cache ID is
    # consumed by a subsequent MCP process while the same editor extension remains alive.
    $phase4BaselineRequests = @(
        (@{ jsonrpc = "2.0"; id = 400; method = "initialize"; params = @{} } | ConvertTo-Json -Compress),
        (Tool-Request 401 "runtime_attach_session" @{ session_id = $editorSession.session_id }),
        (Tool-Request 402 "scene_open" @{ scene_path = "res://main.tscn" }),
        (Tool-Request 403 "viewport_capture_frame" @{ camera_identifier = "active_editor_view" }),
        (Tool-Request 404 "project_search_text" @{ query = "DIDI_PHASE4_SEARCH_PROBE"; search_path = "res://"; extensions = @(".gd"); max_results = 10 }),
        (Tool-Request 405 "project_search_symbols" @{ query = "phase_four_probe"; search_path = "res://"; extensions = @(".gd"); match = "exact"; kinds = @("function"); max_results = 10 })
    )
    $rawPhase4BaselineResponses = $phase4BaselineRequests | & $didiExecutable --project $fixtureRoot
    $phase4BaselineResponses = @($rawPhase4BaselineResponses | Where-Object { $_ -like "{*" } | ForEach-Object { $_ | ConvertFrom-Json })
    Assert-True ($LASTEXITCODE -eq 0) "Phase 4 baseline MCP process exited with $LASTEXITCODE."
    $phase4BaselineById = @{}
    foreach ($response in $phase4BaselineResponses) { $phase4BaselineById[[int]$response.id] = $response }
    Assert-True ((Tool-Payload $phase4BaselineById[402]).opened -eq $true) "Phase 4 fixture scene could not be reopened."
    $phase4Baseline = Tool-Payload $phase4BaselineById[403]
    Assert-True ($phase4Baseline.capture_id -match '^[0-9a-f]{32}$') "Live baseline capture did not return a bounded process-local ID."
    $textSearch = Tool-Payload $phase4BaselineById[404]
    Assert-True ($textSearch.execution_mode -eq "offline_fallback" -and @($textSearch.matches).Count -eq 2) "Bounded project text search did not find the fixture probe."
    Assert-True ($textSearch.matches[0].path -eq "res://subject.gd") "Project text search leaked a non-resource result path."
    $symbolSearch = Tool-Payload $phase4BaselineById[405]
    Assert-True ($symbolSearch.lexical -eq $true -and @($symbolSearch.matches).Count -eq 1) "Lexical symbol search did not find exactly one fixture function."
    Assert-True ($symbolSearch.matches[0].name -eq "phase_four_probe" -and $symbolSearch.matches[0].kind -eq "function") "Symbol search returned the wrong declaration."

    $phase4Requests = @(
        (@{ jsonrpc = "2.0"; id = 410; method = "initialize"; params = @{} } | ConvertTo-Json -Compress),
        (Tool-Request 411 "runtime_attach_session" @{ session_id = $editorSession.session_id }),
        (Tool-Request 412 "viewport_capture_frame" @{ camera_identifier = "active_editor_view"; node_isolation_path = "/root/SmokeRoot/Subject"; isolation_background = "original" }),
        (Tool-Request 413 "scene_get_property" @{ target_node = "/root/SmokeRoot/Container"; property_name = "visible" }),
        (Tool-Request 414 "asset_reimport" @{ paths = @("res://reimport_probe.svg"); timeout_ms = 10000 }),
        (Tool-Request 415 "scene_set_property" @{ target_node = "/root/SmokeRoot/Container"; property_name = "visible"; value = $false }),
        (Tool-Request 416 "viewport_diff_capture" @{ baseline_capture_id = $phase4Baseline.capture_id; camera_identifier = "active_editor_view"; threshold = 0 }),
        (Tool-Request 417 "editor_undo" @{}),
        (Tool-Request 418 "viewport_diff_capture" @{ baseline_capture_id = $phase4Baseline.capture_id; camera_identifier = "active_editor_view"; threshold = 0 }),
        (Tool-Request 419 "scene_get_property" @{ target_node = "/root/SmokeRoot/Container"; property_name = "visible" })
    )
    $rawPhase4Responses = $phase4Requests | & $didiExecutable --project $fixtureRoot
    $phase4Responses = @($rawPhase4Responses | Where-Object { $_ -like "{*" } | ForEach-Object { $_ | ConvertFrom-Json })
    Assert-True ($LASTEXITCODE -eq 0) "Phase 4 verification MCP process exited with $LASTEXITCODE."
    $phase4ById = @{}
    foreach ($response in $phase4Responses) { $phase4ById[[int]$response.id] = $response }
    $isolated = Tool-Payload $phase4ById[412]
    Assert-True ($isolated.isolated -eq $true -and $isolated.state_restored -eq $true) "Isolated capture did not confirm reversible state restoration."
    Assert-True ($isolated.node_isolation_path -eq "/root/SmokeRoot/Subject" -and $isolated.temporarily_hidden_count -ge 1) "Isolated capture did not canonicalize the target or hide unrelated visual branches."
    Assert-True ((Tool-Payload $phase4ById[413]).value -eq $true) "Isolation left an unrelated visual branch hidden."
    $reimport = Tool-Payload $phase4ById[414]
    Assert-True ($reimport.accepted_count -eq 1 -and $reimport.idle -eq $true) "Editor-backed asset reimport did not reach a stable idle state."
    $changedDiff = Tool-Payload $phase4ById[416]
    Assert-True ($changedDiff.changed_pixels -gt 0 -and $null -ne $changedDiff.bounding_box) "Visual mutation did not produce a bounded non-empty pixel diff."
    Assert-True ($changedDiff.comparison_capture_id -match '^[0-9a-f]{32}$') "Viewport diff did not retain the fresh comparison capture."
    Assert-True (@($phase4ById[416].result.content | Where-Object type -eq "image").Count -eq 1) "Viewport diff did not return exactly one PNG content item."
    $restoredDiff = Tool-Payload $phase4ById[418]
    Assert-True ($restoredDiff.identical -eq $true -and $restoredDiff.changed_pixels -eq 0) "Undo did not restore an exact baseline viewport at threshold zero."
    Assert-True ((Tool-Payload $phase4ById[419]).value -eq $true) "Visual mutation undo did not restore scene visibility."

    $phase5Requests = @(
        (@{ jsonrpc = "2.0"; id = 500; method = "initialize"; params = @{} } | ConvertTo-Json -Compress),
        (Tool-Request 501 "runtime_attach_session" @{ session_id = $editorSession.session_id }),
        (Tool-Request 502 "project_list_export_presets" @{}),
        (Tool-Request 503 "shader_check_compile" @{ shader_path = "res://phase5_valid.gdshader"; timeout_seconds = 30 }),
        (Tool-Request 504 "shader_check_compile" @{ shader_path = "res://phase5_invalid.gdshader"; timeout_seconds = 30 }),
        (Tool-Request 505 "project_export" @{ preset = "Phase5 Pack"; output_path = "res://phase5-output.pck"; mode = "pack"; timeout_seconds = 120 }),
        (Tool-Request 506 "gridmap_export_mesh_library" @{ source_scene = "res://phase5_mesh_source.tscn"; output_path = "res://phase5.meshlib"; generate_collisions = $true; timeout_seconds = 60 }),
        (Tool-Request 511 "runtime_attach_session" @{ session_id = $editorSession.session_id }),
        (Tool-Request 507 "scene_open" @{ scene_path = "res://phase5_ui.tscn" }),
        (Tool-Request 508 "ui_hit_test" @{ point = @{ x = 32; y = 32 }; root_path = "/root/Phase5Ui"; max_results = 16 }),
        (Tool-Request 509 "ui_hit_test" @{ point = @{ x = 32; y = 32 }; root_path = "/root/Phase5Ui"; include_mouse_filter_ignore = $true; max_results = 16 })
    )
    $dotnetAvailable = $null -ne (Get-Command dotnet -ErrorAction SilentlyContinue)
    if ($dotnetAvailable) {
        $phase5Requests += Tool-Request 510 "csharp_check_build" @{ project_file = "res://Phase5.csproj"; configuration = "Debug"; timeout_seconds = 120 }
    }
    $previousGodotBin = $env:GODOT_BIN
    try {
        $env:GODOT_BIN = $GodotExecutable
        Push-Location $fixtureRoot
        try {
            $rawPhase5Responses = $phase5Requests | & $didiExecutable --project $fixtureRoot
        }
        finally {
            Pop-Location
        }
    }
    finally {
        if ($null -eq $previousGodotBin) { Remove-Item Env:GODOT_BIN -ErrorAction SilentlyContinue }
        else { $env:GODOT_BIN = $previousGodotBin }
    }
    $phase5Responses = @($rawPhase5Responses | Where-Object { $_ -like "{*" } | ForEach-Object { $_ | ConvertFrom-Json })
    Assert-True ($LASTEXITCODE -eq 0) "Phase 5 MCP process exited with $LASTEXITCODE."
    $phase5ById = @{}
    foreach ($response in $phase5Responses) { $phase5ById[[int]$response.id] = $response }
    $phase5Presets = Tool-Payload $phase5ById[502]
    Assert-True (@($phase5Presets.presets).Count -eq 1 -and $phase5Presets.presets[0].name -eq "Phase5 Pack") "Phase 5 export preset was not listed."
    Assert-True ($phase5ById[502].result.content[0].text -notmatch "phase5-secret") "Export preset options leaked a secret value."
    $validShader = Tool-Payload $phase5ById[503]
    Assert-True ($validShader.success -eq $true -and $validShader.has_errors -eq $false) "Valid shader did not compile cleanly."
    $invalidShader = Tool-Payload $phase5ById[504]
    Assert-True ($invalidShader.success -eq $false -and $invalidShader.has_errors -eq $true -and @($invalidShader.diagnostics).Count -ge 1) "Invalid shader did not return structured diagnostics."
    $phase5Export = Tool-Payload $phase5ById[505]
    Assert-True ($phase5Export.success -eq $true -and $phase5Export.size_bytes -gt 0) "Phase 5 pack export did not create a non-empty artifact."
    $meshLibrary = Tool-Payload $phase5ById[506]
    Assert-True ($meshLibrary.success -eq $true -and $meshLibrary.item_count -eq 2) "MeshLibrary export did not preserve two deterministic items."
    Assert-True ((Tool-Payload $phase5ById[511]).handshake.status -eq "ok") "Phase 5 did not restore the editor route after offline subprocess work."
    Assert-True ((Tool-Payload $phase5ById[507]).opened -eq $true) "Phase 5 UI fixture scene could not be opened."
    $uiDefault = Tool-Payload $phase5ById[508]
    Assert-True ($uiDefault.topmost.node_path -match "/TopControl$") "UI hit-test did not return the top non-ignored Control first."
    Assert-True (-not (@($uiDefault.hits.node_path) -match "/IgnoredControl$")) "UI hit-test included MOUSE_FILTER_IGNORE by default."
    $uiIncludingIgnored = Tool-Payload $phase5ById[509]
    Assert-True ($uiIncludingIgnored.topmost.node_path -match "/IgnoredControl$") "UI hit-test did not honor include_mouse_filter_ignore ordering."
    if ($dotnetAvailable) {
        $csharpBuild = Tool-Payload $phase5ById[510]
        Assert-True ($csharpBuild.success -eq $true -and $csharpBuild.has_errors -eq $false) "C# diagnostic build did not compile the fixture."
    }

    Assert-True $byId[20].result.isError "Unimplemented signal tool returned fake success."
    Assert-True ($byId[20].result.content[0].text -match "no trustworthy execution path") "Unimplemented tool error is not actionable."
    Assert-True $byId[21].result.isError "Missing node lookup returned fake success."
    Assert-True $byId[22].result.isError "Unknown property lookup returned fake null success."
    Assert-True $byId[23].result.isError "Incompatible scalar property write returned fake success."
    Assert-True ((Tool-Payload $byId[24]).value -eq 12) "Rejected property write changed the live node."
    Assert-True $byId[25].result.isError "Unknown initial property returned fake instantiate success."
    $restoredOrder = @((Tool-Payload $byId[28]).scene_tree.children.name)
    Assert-True ($restoredOrder[0] -eq "Subject") "Remove undo did not restore the node's original sibling index: $($restoredOrder -join ', ')"
    Assert-True ($restoredOrder -notcontains "InvalidSpawn") "Rejected instantiation leaked a node into the edited scene."
    $nestedHierarchy = Tool-Payload $byId[29]
    Assert-True ($nestedHierarchy.scene_tree.path -eq "/root/SmokeRoot/Container") "Nested hierarchy root lost its absolute logical path: $($nestedHierarchy.scene_tree.path)"
    $reparentUndoOrder = @((Tool-Payload $byId[32]).scene_tree.children.name)
    Assert-True ($reparentUndoOrder[0] -eq "Subject") "Reparent undo did not restore the original sibling index: $($reparentUndoOrder -join ', ')"
    $relativeHierarchy = Tool-Payload $byId[33]
    Assert-True ($relativeHierarchy.scene_tree.path -eq "/root/SmokeRoot/Container") "Relative hierarchy root was not canonicalized: $($relativeHierarchy.scene_tree.path)"
    Assert-True $byId[34].result.isError "A parent-relative hierarchy path escaped the edited scene."
    Assert-True $byId[35].result.isError "A multi-level parent-relative hierarchy path escaped the edited scene."
    Assert-True $byId[36].result.isError "A non-Node ClassDB object was accepted as a scene node."
    Assert-True $byId[37].result.isError "The edited scene root was accepted for removal."
    Assert-True $byId[38].result.isError "The edited scene root was accepted for duplication."
    Assert-True $byId[39].result.isError "The edited scene root was accepted for reparenting."
    Assert-True (-not $byId[40].result.isError) "Cycle-test setup could not place SpawnedCopy under Container."
    Assert-True $byId[41].result.isError "An ancestor node was accepted for reparenting beneath its descendant."
    Assert-True $byId[42].result.isError "A node was accepted as its own new parent."
    $cycleHierarchy = Tool-Payload $byId[43]
    $cycleContainer = @($cycleHierarchy.scene_tree.children | Where-Object name -eq "Container")[0]
    Assert-True (@($cycleContainer.children.name) -contains "SpawnedCopy") "Rejected cyclic reparenting changed the hierarchy."
    Assert-True (@($cycleHierarchy.scene_tree.children.name) -contains "Subject") "Rejected self-reparenting changed the hierarchy."

    $settingWrite = Tool-Payload $byId[44]
    Assert-True ($settingWrite.persisted -eq $true) "Project setting write did not confirm persistence."
    $settingRead = Tool-Payload $byId[45]
    Assert-True ($settingRead.value.enabled -eq $true) "Nested project setting lost its boolean value."
    Assert-True ($settingRead.value.names[1] -eq "beta") "Nested project setting lost its array value."
    Assert-True ($settingRead.value.nested.ratio -eq 0.5) "Nested project setting lost its dictionary value."
    Assert-True ((Tool-Payload $byId[46]).removed -eq $true) "Project setting removal did not report success."
    Assert-True $byId[47].result.isError "Missing project setting returned fake success."
    Assert-True $byId[48].result.isError "Generic project settings tool wrote into the autoload namespace."
    Assert-True $byId[49].result.isError "Generic project settings tool wrote into the InputMap namespace."
    Assert-True $byId[50].result.isError "Excessively nested project setting was accepted."

    Assert-True ((Tool-Payload $byId[51]).undo_redo_registered) "Script attachment bypassed UndoRedo."
    Assert-True ((Tool-Payload $byId[52]).detached -eq $true) "Script detachment was not observed."
    Assert-True (-not $byId[53].result.isError) "Script detach could not be undone."
    Assert-True (-not $byId[54].result.isError) "Script detach could not be redone."
    Assert-True $byId[55].result.isError "Detaching a node without a script returned fake success."

    Assert-True ((Tool-Payload $byId[56]).undo_redo_registered) "Group addition bypassed UndoRedo."
    Assert-True (@((Tool-Payload $byId[57]).groups) -contains "phase_two") "Added group was not listed."
    Assert-True (@((Tool-Payload $byId[58]).members) -contains "/root/SmokeRoot/Subject") "Group member query missed the edited-scene node."
    Assert-True (@((Tool-Payload $byId[60]).members).Count -eq 0) "Removed group membership remained visible."
    Assert-True (@((Tool-Payload $byId[62]).members) -contains "/root/SmokeRoot/Subject") "Group removal undo did not restore membership."
    Assert-True $byId[63].result.isError "Duplicate group membership returned fake success."

    Assert-True (@((Tool-Payload $byId[64]).autoloads).Count -eq 0) "Disposable fixture unexpectedly began with autoloads."
    $autoload = @((Tool-Payload $byId[66]).autoloads)[0]
    Assert-True ($autoload.name -eq "PhaseTwo" -and $autoload.path -eq "res://subject.gd" -and $autoload.singleton -eq $true) "Autoload did not persist in canonical form."
    Assert-True $byId[67].result.isError "Existing autoload was overwritten without replace: true."
    Assert-True (@((Tool-Payload $byId[69]).autoloads)[0].singleton -eq $false) "Explicit autoload replacement did not persist singleton=false."
    Assert-True ((Tool-Payload $byId[70]).removed -eq $true) "Autoload removal did not report success."
    Assert-True (@((Tool-Payload $byId[71]).autoloads).Count -eq 0) "Removed autoload remained persisted."
    Assert-True $byId[72].result.isError "Removing a missing autoload returned fake success."
    Assert-True $byId[73].result.isError "Invalid autoload identifier was accepted."
    Assert-True $byId[74].result.isError "Missing autoload resource was accepted."

    Assert-True (-not (@((Tool-Payload $byId[75]).actions.action) -contains "phase_two_jump")) "Disposable fixture unexpectedly began with the Phase 2 input action."
    $inputAction = @((Tool-Payload $byId[77]).actions | Where-Object action -eq "phase_two_jump")[0]
    Assert-True ($inputAction.action -eq "phase_two_jump" -and $inputAction.deadzone -eq 0.35) "Input action metadata did not persist."
    Assert-True (@($inputAction.events).Count -eq 4) "Input action did not persist every supported event shape."
    Assert-True ($inputAction.events[0].type -eq "key" -and $inputAction.events[0].shift -eq $true) "Key event was not normalized correctly."
    Assert-True ($inputAction.events[3].type -eq "joypad_motion" -and $inputAction.events[3].axis_value -eq -0.75) "Joypad motion event was not normalized correctly."
    Assert-True $byId[78].result.isError "Existing input action was overwritten without replace: true."
    $replacedInputAction = @((Tool-Payload $byId[80]).actions | Where-Object action -eq "phase_two_jump")[0]
    Assert-True (@($replacedInputAction.events).Count -eq 0) "Explicit input action replacement did not persist."
    Assert-True ((Tool-Payload $byId[81]).removed -eq $true) "Input action removal did not report success."
    Assert-True (-not (@((Tool-Payload $byId[82]).actions.action) -contains "phase_two_jump")) "Removed input action remained persisted."
    Assert-True $byId[83].result.isError "Unknown input event type was accepted."
    Assert-True $byId[84].result.isError "Out-of-range InputMap deadzone was accepted."
    Assert-True $byId[85].result.isError "Empty key event was accepted."

    Assert-True $byId[86].result.isError "Scene close discarded unsaved edits without explicit permission."
    Assert-True ((Tool-Payload $byId[87]).saved -eq $true) "Packed branch did not report a saved PackedScene."
    Assert-True $byId[88].result.isError "Packed branch overwrote an existing scene without overwrite: true."
    Assert-True ((Tool-Payload $byId[89]).opened -eq $true) "Packed branch could not be opened."
    $packedHierarchy = Tool-Payload $byId[90]
    Assert-True ($packedHierarchy.scene_tree.name -eq "Container") "Packed branch root was not preserved."
    Assert-True (@($packedHierarchy.scene_tree.children.name) -contains "SpawnedCopy") "Packed branch lost its owned child."
    Assert-True ((Tool-Payload $byId[91]).closed -eq $true) "Clean packed scene could not be closed."
    Assert-True ((Tool-Payload $byId[92]).opened -eq $true) "New scene was not created and opened."
    Assert-True ((Tool-Payload $byId[93]).scene_tree.name -eq "Created") "Created scene root was not observable."
    Assert-True $byId[94].result.isError "Scene create overwrote an existing target without overwrite: true."
    Assert-True ((Tool-Payload $byId[95]).opened -eq $true) "Existing PackedScene could not be reopened."
    Assert-True ((Tool-Payload $byId[96]).closed -eq $true) "Reopened clean scene could not be closed."
    Assert-True $byId[97].result.isError "Missing PackedScene returned fake open success."
    Assert-True $byId[98].result.isError "Absolute filesystem scene path was accepted."
    Assert-True ((Tool-Payload $byId[99]).opened -eq $true) "Explicit scene overwrite failed."
    Assert-True ((Tool-Payload $byId[100]).scene_tree.name -eq "Replaced") "Explicit scene overwrite did not replace the root."
    Assert-True ((Tool-Payload $byId[101]).closed -eq $true) "Clean replaced scene could not be closed."
    Assert-True ((Tool-Payload $byId[102]).opened -eq $true) "Smoke scene could not be reopened for script rejection checks."
    Assert-True $byId[103].result.isError "Missing script resource was accepted."
    Assert-True $byId[104].result.isError "Non-script scene resource was accepted for script attachment."
    Assert-True $byId[105].result.isError "Native-base-incompatible script returned fake attachment success."
    Assert-True ((Tool-Payload $byId[106]).added -eq $true) "Transient group setup failed."
    Assert-True ((Tool-Payload $byId[107]).removed -eq $true) "Transient group removal failed."
    Assert-True (-not $byId[108].result.isError) "Transient group removal could not be undone."
    Assert-True ((Tool-Payload $byId[109]).saved -eq $true) "Transient group probe scene could not be packed."
    $transientProbe = Get-Content -LiteralPath (Join-Path $fixtureRoot "transient_probe.tscn") -Raw
    Assert-True ($transientProbe -notmatch "phase_two_transient") "Remove undo changed a transient group into persistent membership."
    Assert-True ((Tool-Payload $byId[110]).removed -eq $true) "Transient group cleanup failed."
    Assert-True ((Tool-Payload $byId[111]).closed -eq $true) "Smoke scene cleanup failed."
    Assert-True $byId[112].result.isError "Non-normalized res:// scene path was accepted."
    Assert-True ($byId[112].result.content[0].text -match "normalized") "Non-normalized path error was not actionable."

    $firstLogPage = Tool-Payload $byId[113]
    Assert-True ($firstLogPage.execution_mode -eq "live") "First runtime log read was not live."
    Assert-True (@($firstLogPage.records).Count -gt 0) "First runtime log read returned no extension records."
    Assert-True ($firstLogPage.next_cursor -gt $firstLogPage.records[-1].sequence) "First runtime log cursor did not advance."
    Assert-True (($firstLogPage.records | ConvertTo-Json -Compress -Depth 20) -notmatch "didi_secret_expression_42") "Runtime logs exposed full expression text."
    foreach ($record in @($firstLogPage.records)) {
        Assert-True ($null -ne $record.PSObject.Properties["details"]) "Live runtime log record omitted the uniform details field."
    }
    $publicTranscript = (@($rawRuntimeResponses) + @($rawResponses) +
        @(Get-Content $stdoutPath, $stderrPath, $gameStdoutPath, $gameStderrPath -ErrorAction SilentlyContinue)) -join "`n"
    Assert-True ($publicTranscript -notmatch [regex]::Escape($editorSessionToken)) "Editor session token leaked into public responses or process logs."
    Assert-True ($publicTranscript -notmatch [regex]::Escape($gameSessionToken)) "Game session token leaked into public responses or process logs."

    $nextLogRequests = @(
        (@{ jsonrpc = "2.0"; id = 120; method = "initialize"; params = @{} } | ConvertTo-Json -Compress),
        (Tool-Request 121 "runtime_attach_session" @{ session_id = $editorSession.session_id }),
        (Tool-Request 122 "runtime_read_logs" @{ cursor = [uint64]$firstLogPage.next_cursor; limit = 5; minimum_level = "debug" })
    )
    $rawNextLogResponses = $nextLogRequests | & $didiExecutable --project $fixtureRoot
    $nextLogResponses = @($rawNextLogResponses | Where-Object { $_ -like "{*" } | ForEach-Object { $_ | ConvertFrom-Json })
    Assert-True ($LASTEXITCODE -eq 0) "Sequential log MCP process exited with $LASTEXITCODE."
    $nextLogById = @{}
    foreach ($response in $nextLogResponses) { $nextLogById[[int]$response.id] = $response }
    $secondLogPage = Tool-Payload $nextLogById[122]
    Assert-True ($secondLogPage.next_cursor -ge $firstLogPage.next_cursor) "Second runtime log cursor moved backwards."
    foreach ($record in @($secondLogPage.records)) {
        Assert-True ($record.sequence -ge $firstLogPage.next_cursor) "Sequential runtime log reads repeated sequence $($record.sequence)."
    }

    $stopRequests = @(
        (@{ jsonrpc = "2.0"; id = 330; method = "initialize"; params = @{} } | ConvertTo-Json -Compress),
        (Tool-Request 331 "runtime_attach_session" @{ session_id = $gameSession.session_id }),
        (Tool-Request 332 "runtime_stop" @{ exit_code = 0 })
    )
    $rawStopResponses = $stopRequests | & $didiExecutable --project $fixtureRoot
    $stopResponses = @($rawStopResponses | Where-Object { $_ -like "{*" } | ForEach-Object { $_ | ConvertFrom-Json })
    Assert-True ($LASTEXITCODE -eq 0) "Didi runtime-stop process exited with $LASTEXITCODE."
    $stopById = @{}
    foreach ($response in $stopResponses) { $stopById[[int]$response.id] = $response }
    Assert-True ((Tool-Payload $stopById[332]).shutdown_requested -eq $true) "Runtime stop did not report a shutdown request."

    $stopDeadline = [DateTime]::UtcNow.AddSeconds($StartupTimeoutSeconds)
    while ([DateTime]::UtcNow -lt $stopDeadline -and -not $game.HasExited) { Start-Sleep -Milliseconds 100 }
    Assert-True $game.HasExited "Runtime stop did not terminate the game process."

    $cleanupRequests = @(
        (@{ jsonrpc = "2.0"; id = 340; method = "initialize"; params = @{} } | ConvertTo-Json -Compress),
        (Tool-Request 341 "runtime_list_sessions" @{ project_path = $fixtureRoot })
    )
    $rawCleanupResponses = $cleanupRequests | & $didiExecutable --project $fixtureRoot
    $cleanupResponses = @($rawCleanupResponses | Where-Object { $_ -like "{*" } | ForEach-Object { $_ | ConvertFrom-Json })
    $cleanupById = @{}
    foreach ($response in $cleanupResponses) { $cleanupById[[int]$response.id] = $response }
    $remainingSessions = @(Tool-Payload $cleanupById[341]).sessions
    Assert-True (-not (@($remainingSessions.session_id) -contains $gameSession.session_id)) "Stopped game descriptor was not cleaned up."
    Assert-True (@($remainingSessions | Where-Object { $_.session_id -eq $editorSession.session_id -and $_.alive }).Count -eq 1) "Editor did not remain live after stopping the game."

    $env:DIDI_TEST_STOP_DURING_STEP = "1"
    try {
        $shutdownGame = Start-Process -FilePath $GodotExecutable `
            -ArgumentList @("--headless", "--path", $fixtureRoot, "--log-file", $shutdownGameEngineLogPath, "res://runtime_main.tscn") `
            -PassThru -WindowStyle Hidden `
            -RedirectStandardOutput $shutdownGameStdoutPath -RedirectStandardError $shutdownGameStderrPath
    }
    finally {
        Remove-Item Env:DIDI_TEST_STOP_DURING_STEP -ErrorAction SilentlyContinue
    }

    $shutdownSession = $null
    $shutdownDeadline = [DateTime]::UtcNow.AddSeconds($StartupTimeoutSeconds)
    while ([DateTime]::UtcNow -lt $shutdownDeadline -and -not $shutdownGame.HasExited) {
        $shutdownDiscoveryRequests = @(
            (@{ jsonrpc = "2.0"; id = 420; method = "initialize"; params = @{} } | ConvertTo-Json -Compress),
            (Tool-Request 421 "runtime_list_sessions" @{ project_path = $fixtureRoot })
        )
        $shutdownDiscoveryResponses = @($shutdownDiscoveryRequests | & $didiExecutable --project $fixtureRoot |
            Where-Object { $_ -like "{*" } | ForEach-Object { $_ | ConvertFrom-Json })
        if ($LASTEXITCODE -eq 0) {
            $shutdownDiscoveryById = @{}
            foreach ($response in $shutdownDiscoveryResponses) { $shutdownDiscoveryById[[int]$response.id] = $response }
            if ($shutdownDiscoveryById.ContainsKey(421) -and -not $shutdownDiscoveryById[421].result.isError) {
                $shutdownSession = @((Tool-Payload $shutdownDiscoveryById[421]).sessions | Where-Object {
                    $_.kind -eq "game" -and $_.alive -and $_.session_id -ne $gameSession.session_id -and
                    $_.session_id -ne $editorSession.session_id
                })[0]
                if ($null -ne $shutdownSession) { break }
            }
        }
        Start-Sleep -Milliseconds 100
    }
    Assert-True ($null -ne $shutdownSession) "Shutdown-cancellation fixture did not publish a game session."
    $shutdownGameEnginePid = [uint64]$shutdownSession.pid
    $shutdownGameEngineStartedAtMs = [int64]$shutdownSession.started_at_ms
    $shutdownGameSessionId = [string]$shutdownSession.session_id
    $shutdownDescriptor = Get-Content -LiteralPath (Join-Path $sessionDirectory ($shutdownSession.session_id + ".json")) -Raw | ConvertFrom-Json
    $shutdownGameSessionToken = [string]$shutdownDescriptor.token
    Assert-True ($shutdownGameSessionToken -match '^[0-9a-f]{64}$') "Shutdown-game descriptor token did not meet the private protocol shape."

    $pauseForShutdownRequests = @(
        (@{ jsonrpc = "2.0"; id = 430; method = "initialize"; params = @{} } | ConvertTo-Json -Compress),
        (Tool-Request 431 "runtime_attach_session" @{ session_id = $shutdownSession.session_id }),
        (Tool-Request 432 "runtime_set_paused" @{ paused = $true })
    )
    $pauseForShutdownResponses = @($pauseForShutdownRequests | & $didiExecutable --project $fixtureRoot |
        Where-Object { $_ -like "{*" } | ForEach-Object { $_ | ConvertFrom-Json })
    $pauseForShutdownById = @{}
    foreach ($response in $pauseForShutdownResponses) { $pauseForShutdownById[[int]$response.id] = $response }
    Assert-True ((Tool-Payload $pauseForShutdownById[432]).paused -eq $true) "Shutdown-cancellation fixture was not paused before stepping."

    $shutdownStepRequests = @(
        (@{ jsonrpc = "2.0"; id = 440; method = "initialize"; params = @{} } | ConvertTo-Json -Compress),
        (Tool-Request 441 "runtime_attach_session" @{ session_id = $shutdownSession.session_id }),
        (Tool-Request 442 "runtime_step" @{ frames = 60 })
    )
    $shutdownStepResponses = @($shutdownStepRequests | & $didiExecutable --project $fixtureRoot |
        Where-Object { $_ -like "{*" } | ForEach-Object { $_ | ConvertFrom-Json })
    $shutdownStepById = @{}
    foreach ($response in $shutdownStepResponses) { $shutdownStepById[[int]$response.id] = $response }
    $shutdownExitDeadline = [DateTime]::UtcNow.AddSeconds(5)
    while ([DateTime]::UtcNow -lt $shutdownExitDeadline -and -not $shutdownGame.HasExited) { Start-Sleep -Milliseconds 100 }
    Assert-True $shutdownGame.HasExited "Game shutdown did not cancel the active 60-frame step."
    Assert-True ([bool]$shutdownStepById[442].result.isError) "An active step returned fake success after its game shut down."
    $shutdownStepError = [string]$shutdownStepById[442].result.content[0].text
    Assert-True ($shutdownStepError -match "cancel|disconnect|closed|503|failed|not connected|terminated|shutting down|shutdown") "Active-step shutdown returned an unactionable error: $shutdownStepError"

    $engineErrors = @(
        Get-Content $stderrPath -ErrorAction SilentlyContinue |
            Where-Object { $_ -match 'UndoRedo history mismatch|Parameter "t" is null|\[ERROR' }
        Get-Content $gameStderrPath -ErrorAction SilentlyContinue |
            Where-Object { $_ -match "Can't retrieve singleton 'EditorInterface' outside of editor|\[ERROR" }
        Get-Content $shutdownGameStderrPath -ErrorAction SilentlyContinue |
            Where-Object { $_ -match "Can't retrieve singleton 'EditorInterface' outside of editor|\[ERROR" }
    )
    Assert-True ($engineErrors.Count -eq 0) "Godot reported bridge errors:`n$($engineErrors -join "`n")"

    $projectConfigPath = [IO.Path]::GetFullPath((Join-Path $fixtureRoot "project.godot"))
    if (-not $projectConfigPath.StartsWith($fixtureRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to alter a persistence-failure fixture outside its disposable project: $projectConfigPath"
    }
    if ($IsWindows) {
        $aclIdentity = [Security.Principal.WindowsIdentity]::GetCurrent().Name
        & icacls $projectConfigPath /deny "${aclIdentity}:(W,D)" | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "Unable to deny project.godot persistence rights for rollback testing." }
        & icacls $fixtureRoot /deny "${aclIdentity}:(WD,AD,DC)" | Out-Null
        if ($LASTEXITCODE -ne 0) {
            & icacls $projectConfigPath /remove:d $aclIdentity | Out-Null
            throw "Unable to deny project-root replacement rights for rollback testing."
        }
    }
    else {
        & chmod a-w -- $projectConfigPath
        & chmod a-w -- $fixtureRoot
    }
    try {
        $failureRequests = @(
            (@{ jsonrpc = "2.0"; id = 200; method = "initialize"; params = @{} } | ConvertTo-Json -Compress),
            (Tool-Request 199 "runtime_attach_session" @{ session_id = $editorSession.session_id }),
            (Tool-Request 201 "project_set_setting" @{ setting = "didi_phase2/rollback_probe"; value = @{ changed = $true } }),
            (Tool-Request 202 "project_get_setting" @{ setting = "didi_phase2/rollback_probe" }),
            (Tool-Request 203 "project_set_autoload" @{ name = "RollbackProbe"; path = "res://subject.gd" }),
            (Tool-Request 204 "project_list_autoloads" @{}),
            (Tool-Request 205 "project_set_input_action" @{ action = "rollback_probe"; events = @() }),
            (Tool-Request 206 "project_list_input_actions" @{})
        )
        $rawFailureResponses = $failureRequests | & $didiExecutable --project $fixtureRoot
        $failureResponses = @($rawFailureResponses | Where-Object { $_ -like "{*" } | ForEach-Object { $_ | ConvertFrom-Json })
        Assert-True ($LASTEXITCODE -eq 0) "Didi rollback MCP process exited with $LASTEXITCODE."
        Assert-True ($failureResponses.Count -eq $failureRequests.Count) "Rollback batch response count mismatch."
        $failureById = @{}
        foreach ($response in $failureResponses) { $failureById[[int]$response.id] = $response }
        Assert-True ([bool]$failureById[201].result.isError) "Project setting save failure returned fake success: $($failureById[201] | ConvertTo-Json -Compress -Depth 20)"
        Assert-True ([bool]$failureById[202].result.isError) "Failed project setting mutation remained in memory after rollback."
        Assert-True ([bool]$failureById[203].result.isError) "Autoload save failure returned fake success."
        Assert-True (-not (@((Tool-Payload $failureById[204]).autoloads.name) -contains "RollbackProbe")) "Failed autoload mutation remained in memory after rollback."
        Assert-True ([bool]$failureById[205].result.isError) "InputMap save failure returned fake success."
        Assert-True (-not (@((Tool-Payload $failureById[206]).actions.action) -contains "rollback_probe")) "Failed InputMap mutation remained live after rollback reload."
    }
    finally {
        if ($IsWindows) {
            & icacls $fixtureRoot /remove:d $aclIdentity | Out-Null
            & icacls $projectConfigPath /remove:d $aclIdentity | Out-Null
        }
        else {
            & chmod u+w -- $fixtureRoot
            & chmod u+w -- $projectConfigPath
        }
    }

    Assert-True (Request-ExactProcessClose $editorEnginePid $editorEngineStartedAtMs $StartupTimeoutSeconds) "Graceful close did not terminate the exact editor process."

    $responseTranscript = @(
        @($rawPrelaunchResponses)
        @($rawDiscoveryResponses)
        @($rawGameDiscovery)
        @($rawRuntimeResponses)
        @($rawResponses)
        @($rawPhase4BaselineResponses)
        @($rawPhase4Responses)
        @($rawPhase5Responses)
        @($rawNextLogResponses)
        @($rawStopResponses)
        @($rawCleanupResponses)
        @($shutdownDiscoveryResponses | ConvertTo-Json -Compress -Depth 100)
        @($pauseForShutdownResponses | ConvertTo-Json -Compress -Depth 100)
        @($shutdownStepResponses | ConvertTo-Json -Compress -Depth 100)
        @($rawFailureResponses)
    )
    $logTranscript = @(
        Get-Content $stdoutPath, $stderrPath, $gameStdoutPath, $gameStderrPath,
            $editorEngineLogPath, $gameEngineLogPath, $shutdownGameStdoutPath,
            $shutdownGameStderrPath, $shutdownGameEngineLogPath -ErrorAction SilentlyContinue
    )
    $completePublicTranscript = (@($responseTranscript) + @($logTranscript)) -join "`n"
    Assert-True ($completePublicTranscript -notmatch [regex]::Escape($editorSessionToken)) "Editor session token leaked into a response or complete engine/process log transcript."
    Assert-True ($completePublicTranscript -notmatch [regex]::Escape($gameSessionToken)) "Game session token leaked into a response or complete engine/process log transcript."
    Assert-True ($completePublicTranscript -notmatch [regex]::Escape($shutdownGameSessionToken)) "Shutdown-game session token leaked into a response or complete engine/process log transcript."

    $unexpectedSourceArtifacts = @(Get-ChildItem -LiteralPath $sourceFixtureRoot -Force -Recurse | Where-Object {
        $_.Name -like "*.didi-retired-*" -or
        $_.Name -in @("packed_branch.tscn", "created_phase2.tscn", "transient_probe.tscn")
    })
    Assert-True ($unexpectedSourceArtifacts.Count -eq 0) "Integration generated artifacts in the checked-in source fixture."
    $integrationSucceeded = $true
    Write-Output "Godot integration passed: Phases 1-6 editor/runtime workflows, deep diagnostics, project isolation, export, MeshLibrary, and live UI hit-testing."
}
catch {
    $primaryFailureMessage = $_.Exception.Message
    throw
}
finally {
    Stop-RuntimeProcess $game $gameEnginePid $gameEngineStartedAtMs
    Stop-RuntimeProcess $shutdownGame $shutdownGameEnginePid $shutdownGameEngineStartedAtMs
    Stop-RuntimeProcess $godot $editorEnginePid $editorEngineStartedAtMs
    $descriptorDeadline = [DateTime]::UtcNow.AddSeconds(5)
    do {
        $activeDescriptors = @(Get-ChildItem -LiteralPath $sessionDirectory -Filter "*.json" -Force -ErrorAction SilentlyContinue)
        if ($activeDescriptors.Count -eq 0) { break }
        Start-Sleep -Milliseconds 100
    }
    while ([DateTime]::UtcNow -lt $descriptorDeadline)
    if ($activeDescriptors.Count -ne 0) {
        $activeMessage = "Active runtime descriptors remained after exact-instance shutdown: $($activeDescriptors.Name -join ', ')"
        if ($primaryFailureMessage) {
            Write-Warning "$activeMessage (preserving primary failure: $primaryFailureMessage)"
        }
        else {
            throw $activeMessage
        }
    }

    $knownSessionIds = @($editorSessionId, $gameSessionId, $shutdownGameSessionId) | Where-Object { $_ -match '^[0-9a-f]{32}$' }
    $descriptorEntries = @(Get-ChildItem -LiteralPath $sessionDirectory -Force -ErrorAction SilentlyContinue | Where-Object { $_.Extension -ne '.json' })
    foreach ($entry in $descriptorEntries) {
        $entryPath = [IO.Path]::GetFullPath($entry.FullName)
        if (-not $entryPath.StartsWith($sessionDirectory + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Refusing to remove a descriptor artifact outside the disposable session directory: $entryPath"
        }
        Assert-True (-not $entry.PSIsContainer) "Refusing to recursively remove unexpected descriptor directory: $($entry.Name)"
        Assert-True (($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) "Refusing to remove descriptor reparse entry: $($entry.Name)"
        $retiredMatch = [regex]::Match($entry.Name, '^([0-9a-f]{32})\.json\.didi-retired-\1-[0-9a-f]{32}$')
        $lockMatch = [regex]::Match($entry.Name, '^([0-9a-f]{32})\.lock$')
        Assert-True ($retiredMatch.Success -or $lockMatch.Success) "Refusing to remove unrecognized descriptor artifact: $($entry.Name)"
        $artifactSessionId = if ($retiredMatch.Success) { $retiredMatch.Groups[1].Value } else { $lockMatch.Groups[1].Value }
        Assert-True ($knownSessionIds -contains $artifactSessionId) "Refusing to remove an artifact for an unknown session: $($entry.Name)"
        Remove-Item -LiteralPath $entryPath -Force
    }
    $descriptorEntries = @(Get-ChildItem -LiteralPath $sessionDirectory -Force -ErrorAction SilentlyContinue)
    Remove-Item Env:DIDI_SESSION_DIR -ErrorAction SilentlyContinue
    if ($null -eq $previousGodotBin) {
        Remove-Item Env:GODOT_BIN -ErrorAction SilentlyContinue
    }
    else {
        $env:GODOT_BIN = $previousGodotBin
    }
    if ($integrationSucceeded -and $descriptorEntries.Count -ne 0) {
        throw "Runtime descriptor directory was not empty after exact-PID cleanup: $($descriptorEntries.Name -join ', ')"
    }
}
