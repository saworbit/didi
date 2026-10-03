# asset_reimport as a job: Q8 in docs/BUILD_QUEUE.md, principle P7 in
# docs/DESIGN_PRINCIPLES.md, and #996.
#
# A reimport that has to wait for the editor to apply a scan was answered
# within the bridge's fifteen-second wait or not at all, so its timeout_ms
# stopped at ten seconds. On a software-rendered editor that is a handful of
# new scripts. Given a request_id it runs as a job: the bridge answers at once
# with an id, and the server reads it off the editor's main thread until the
# editor has applied the scan. This block proves on the attached engine line
# what the native tests cannot:
#
# - One server process, kept alive across reads the way a client keeps one,
#   starts the job and reads it with the same request_id until it finishes.
#   Invoke-Didi cannot: its server exits when its input ends, and an exiting
#   server cancels its jobs.
# - The answer is the reimport's own: every new script's class is registered,
#   read from the editor's class list on disk, and the SVG in the batch was
#   reimported, read from its imported texture.
# - A repeat with the same request_id reads the answer and does not reimport
#   again, the same id with other arguments is refused, and a long wait without
#   a request_id is refused with the argument that lifts it.
#
# Forty scripts is the size of batch #996 measured past the old ceiling on the
# software-rendered 4.5.1 runner, where it takes most of a minute; on a machine
# with a GPU it can finish inside the first wait, so the block reads the job
# however long it takes rather than asserting that it ran long.
#
# run_godot_integration.ps1 dot-sources this file and calls the block once the
# editor session is attached. Everything under res://reimport_job is its own.

function Start-DidiSession([string[]]$Arguments) {
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $didiExecutable
    $info.Arguments = (@($Arguments | ForEach-Object { '"' + $_ + '"' }) -join " ")
    $info.UseShellExecute = $false
    $info.RedirectStandardInput = $true
    $info.RedirectStandardOutput = $true
    $info.StandardOutputEncoding = New-Object System.Text.UTF8Encoding($false)
    $info.CreateNoWindow = $true
    $process = [System.Diagnostics.Process]::Start($info)
    return @{ Process = $process; Pending = $null; Requests = (New-Object System.Collections.ArrayList); Lines = (New-Object System.Collections.ArrayList) }
}

# Sends one request and returns its answer, reading past anything else the
# server writes. The pending read is kept across calls, so a read that ran out
# of time is not lost to the next one.
function Send-DidiSession($Session, [string]$Request, [int]$TimeoutSeconds = 60) {
    $id = [int]($Request | ConvertFrom-Json).id
    [void]$Session.Requests.Add($Request)
    $Session.Process.StandardInput.WriteLine($Request)
    $Session.Process.StandardInput.Flush()
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ($true) {
        if ($null -eq $Session.Pending) { $Session.Pending = $Session.Process.StandardOutput.ReadLineAsync() }
        $remaining = [int][Math]::Max(0, ($deadline - [DateTime]::UtcNow).TotalMilliseconds)
        Assert-True ($remaining -gt 0 -and $Session.Pending.Wait($remaining)) "The kept server did not answer request $id within $TimeoutSeconds seconds."
        $line = $Session.Pending.Result
        $Session.Pending = $null
        Assert-True ($null -ne $line) "The kept server exited before answering request $id."
        [void]$Session.Lines.Add($line)
        if ($line -notlike "{*") { continue }
        $response = $line | ConvertFrom-Json
        if ($response.PSObject.Properties.Name -contains "id" -and [int]$response.id -eq $id) { return $response }
    }
}

# Ends the server the way a client does, by closing its input, and hands what
# it answered to the same run-wide checks every Invoke-Didi batch feeds.
function Stop-DidiSession($Session) {
    try { $Session.Process.StandardInput.Close() } catch { }
    if (-not $Session.Process.WaitForExit(15000)) {
        try { $Session.Process.Kill() } catch { }
        Assert-True $false "The kept server did not exit within 15 seconds of its input closing."
    }
    $requests = @($Session.Requests)
    $lines = @($Session.Lines)
    Add-ObservedExchanges -Requests $requests -Lines $lines
    Add-BoundedExchanges -Requests $requests -Lines $lines
    Add-RefusalExchanges -Requests $requests -Lines $lines
    Add-FollowUpExchanges -Requests $requests -Lines $lines
    Add-Phase7Exchanges -Requests $requests -Lines $lines
}

function Invoke-ReimportJobBlock {
    param(
        [Parameter(Mandatory = $true)] [object]$EditorSession,
        [Parameter(Mandatory = $true)] [string]$FixtureRoot
    )
    $folder = Join-Path $FixtureRoot "reimport_job"
    New-Item -ItemType Directory -Path $folder -Force | Out-Null
    $classes = @()
    for ($index = 0; $index -lt 40; $index++) {
        $className = "ReimportJob{0:D4}" -f $index
        $classes += $className
        [System.IO.File]::WriteAllText((Join-Path $folder ("job_{0:D4}.gd" -f $index)),
            "class_name $className`nextends Node`n`nfunc value_$index() -> int:`n`treturn $index`n")
    }
    $probeTexture = @(Get-ChildItem -LiteralPath (Join-Path $FixtureRoot (Join-Path ".godot" "imported")) -Filter "reimport_probe.svg-*.ctex")
    Assert-True ($probeTexture.Count -eq 1) "The SVG's imported texture was not found under .godot/imported."
    $textureBefore = $probeTexture[0].LastWriteTimeUtc
    $classCachePath = Join-Path $FixtureRoot (Join-Path ".godot" "global_script_class_cache.cfg")

    $requestId = "harness-reimport-job-$PID"
    $call = @{ paths = @("res://reimport_job/job_0000.gd", "res://reimport_probe.svg"); timeout_ms = 300000; request_id = $requestId }
    $session = Start-DidiSession @("--project", $FixtureRoot)
    try {
        [void](Send-DidiSession $session (@{ jsonrpc = "2.0"; id = 7400; method = "initialize"; params = @{ protocolVersion = "2024-11-05" } } | ConvertTo-Json -Compress))
        [void](Tool-Payload (Send-DidiSession $session (Tool-Request 7401 "runtime_attach_session" @{ session_id = $EditorSession.session_id })))

        # Started, then read with the same request_id until it has an answer.
        # Each call waits up to about ten seconds on the server's side.
        $started = [DateTime]::UtcNow
        $id = 7402
        $answer = Send-DidiSession $session (Tool-Request $id "asset_reimport" $call) 30
        $workingReads = 0
        while (-not $answer.result.isError -and ($answer.result.content[0].text | ConvertFrom-Json).status -eq "working") {
            $working = $answer.result.content[0].text | ConvertFrom-Json
            Assert-True ($working.job.request_id -eq $requestId -and $working.follow_up[0].work -eq "poll" -and $working.follow_up[0].tool -eq "asset_reimport") "A working reimport job did not say how to read it: $($answer.result.content[0].text)"
            Assert-True (([DateTime]::UtcNow - $started).TotalSeconds -lt 330) "The reimport job was still working after 330 seconds: $($answer.result.content[0].text)"
            $workingReads++
            Start-Sleep -Milliseconds 1000
            $id++
            $answer = Send-DidiSession $session (Tool-Request $id "asset_reimport" $call) 30
        }
        $answerText = $answer.result.content[0].text
        Assert-True (-not $answer.result.isError) "The reimport job was refused: $answerText"
        $payload = $answerText | ConvertFrom-Json
        $job = $answer.result._meta.didi.job
        Assert-True ($null -ne $job -and $job.request_id -eq $requestId -and $job.state -eq "completed") "The reimport job's answer is not named as the job's: $($answer.result | ConvertTo-Json -Compress -Depth 8)"
        # The bridge's id is on the answer only when the reimport detached, so
        # this is what says the job waited by reading rather than within one call.
        Assert-True ($payload.reimport_id -match '^[0-9a-f]{32}$') "The reimport job did not run detached: $answerText"
        Assert-True ($payload.idle -eq $true -and @($payload.paths).Count -eq 2) "The reimport job's answer is not the reimport's: $answerText"

        # What the editor did, read off the disk rather than off the answer.
        $classCache = ""
        $cacheLocked = $false
        try { $classCache = [System.IO.File]::ReadAllText($classCachePath) } catch [System.IO.IOException] { $cacheLocked = $true }
        $unregistered = @($classes | Where-Object { $classCache -notmatch ('"' + $_ + '"') })
        Assert-True (-not $cacheLocked) "The reimport job answered while the editor was still writing its class list."
        Assert-True ($unregistered.Count -eq 0) "The reimport job answered before the editor had applied its scan: $($unregistered.Count) of the $($classes.Count) new classes ($($unregistered[0]) first) were not registered."
        $textureAfter = (Get-Item -LiteralPath $probeTexture[0].FullName).LastWriteTimeUtc
        Assert-True ($textureAfter -gt $textureBefore) "The reimport job answered and res://reimport_probe.svg's imported texture was not rewritten: $answerText"

        # Read again: the same answer, and nothing reimported a second time.
        $id++
        $repeat = Send-DidiSession $session (Tool-Request $id "asset_reimport" $call) 30
        Assert-True (-not $repeat.result.isError -and $repeat.result._meta.didi.job.job_id -eq $job.job_id) "A repeat of the reimport job's request_id did not read the same job: $($repeat.result | ConvertTo-Json -Compress -Depth 8)"
        Assert-True (($repeat.result.content[0].text | ConvertFrom-Json).reimport_id -eq $payload.reimport_id) "A repeat of the reimport job's request_id answered differently: $($repeat.result.content[0].text)"
        Assert-True ((Get-Item -LiteralPath $probeTexture[0].FullName).LastWriteTimeUtc -eq $textureAfter) "A repeat of the reimport job's request_id reimported the SVG again."

        # The same id for other work runs nothing, and says so.
        $id++
        $conflict = Send-DidiSession $session (Tool-Request $id "asset_reimport" @{ paths = @("res://reimport_probe.svg"); request_id = $requestId }) 30
        $conflictText = $conflict.result.content[0].text
        Assert-True ($conflict.result.isError -and ($conflictText | ConvertFrom-Json).error.data.code -eq "request_id_conflict") "The reimport job's request_id was reused for other work and not refused: $conflictText"
        Assert-True ((Get-Item -LiteralPath $probeTexture[0].FullName).LastWriteTimeUtc -eq $textureAfter) "A conflicting request_id reimported the SVG."

        # Past ten seconds needs a job, and the refusal names the argument.
        $id++
        $unbounded = Send-DidiSession $session (Tool-Request $id "asset_reimport" @{ paths = @("res://reimport_probe.svg"); timeout_ms = 60000 }) 30
        $unboundedError = ($unbounded.result.content[0].text | ConvertFrom-Json).error
        Assert-True ($unbounded.result.isError -and $unboundedError.data.code -eq "reimport_needs_job" -and $unboundedError.data.field -eq "request_id") "A reimport waiting past ten seconds without a request_id was not refused with the fix: $($unbounded.result.content[0].text)"
    } finally {
        Stop-DidiSession $session
    }
    Write-Output "Reimport job: $($classes.Count) new scripts and an SVG, answered after $workingReads working read(s) in $([int]([DateTime]::UtcNow - $started).TotalSeconds) s; the repeat read the job and reimported nothing."
}
