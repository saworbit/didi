# Exercise process lifetime independently of a late lookup by PID.
$ErrorActionPreference = 'Stop'
function Assert-True([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
. (Join-Path $PSScriptRoot 'observed_post_state.ps1')
. (Join-Path $PSScriptRoot 'observed_external.ps1')
. (Join-Path $PSScriptRoot 'scenario_runner.ps1')

$original = $null
$replacement = $null
$observer = $null
$root = Join-Path ([IO.Path]::GetTempPath()) ('didi-scenario-witness-' + [guid]::NewGuid())
$editor = Microsoft.PowerShell.Management\Get-Process -Id $PID
try {
    $executable = $editor.Path
    $original = Start-Process -FilePath $executable -ArgumentList @('-NoProfile', '-Command', 'Start-Sleep -Seconds 30') -PassThru -WindowStyle Hidden
    $replacement = Start-Process -FilePath $executable -ArgumentList @('-NoProfile', '-Command', 'Start-Sleep -Seconds 30') -PassThru -WindowStyle Hidden
    $null = $original.Handle
    $null = $replacement.Handle
    $oldPid = $original.Id
    $observer = [DidiScenarioProcessWitness]::new($root)
    $directory = Join-Path $root '.didi/scenario-process-witness'
    foreach ($process in @($original, $replacement)) {
        [IO.File]::WriteAllText((Join-Path $directory ($process.Id.ToString() + '.request')), '')
        $ready = Join-Path $directory ($process.Id.ToString() + '.ready')
        $deadline = [DateTime]::UtcNow.AddSeconds(5)
        while (-not (Test-Path -LiteralPath $ready) -and [DateTime]::UtcNow -lt $deadline -and $null -eq $observer.Error) {
            Start-Sleep -Milliseconds 10
        }
        Assert-True (Test-Path -LiteralPath $ready) "The live process $($process.Id) was not captured: $($observer.Error)"
    }
    $captured = $observer.Processes
    $original.Kill()
    Assert-True ($original.WaitForExit(5000)) 'The owned original process did not exit.'
    # Model a numeric PID now naming a replacement process. The captured
    # original object must remain the witness even when a late lookup changes.
    function Get-Process {
        param([int]$Id, $ErrorAction)
        if ($Id -eq $oldPid) { return Microsoft.PowerShell.Management\Get-Process -Id $replacement.Id }
        return Microsoft.PowerShell.Management\Get-Process -Id $Id -ErrorAction Stop
    }
    $answer = [pscustomobject]@{
        game = [pscustomobject]@{ pid = $oldPid }
        teardown = [pscustomobject]@{ session_gone = $true }
    }
    Assert-ScenarioProcessWitness $answer ([pscustomobject]@{ pid = $editor.Id }) '' $captured
    Assert-True (-not $replacement.HasExited) 'The replacement control must stay alive.'
    $answer.game.pid = $replacement.Id
    $rejected = $false
    try { Assert-ScenarioProcessWitness $answer ([pscustomobject]@{ pid = $editor.Id }) '' $captured }
    catch { if ($_.Exception.Message -like '*disagrees with the engine*') { $rejected = $true } else { throw } }
    Assert-True $rejected 'A game that is still alive must fail the teardown comparison.'
    $answer.game.pid = [int]::MaxValue
    $rejected = $false
    try { Assert-ScenarioProcessWitness $answer ([pscustomobject]@{ pid = $editor.Id }) '' $captured }
    catch { if ($_.Exception.Message -like '*never captured while alive*') { $rejected = $true } else { throw } }
    Assert-True $rejected 'A game never captured while alive must not pass the teardown comparison.'
    Write-Host 'Scenario witness keeps the exited original across a changed PID lookup.'
    Write-Host 'A live game and a missing capture both reject a successful teardown claim.'
    $observer.Dispose()
    $observer.Dispose()
} finally {
    if ($null -ne $observer) { $observer.Dispose() }
    foreach ($process in @($original, $replacement)) {
        if ($null -eq $process) { continue }
        try {
            if (-not $process.HasExited) { $process.Kill(); $process.WaitForExit(5000) | Out-Null }
        } finally { $process.Dispose() }
    }
    $editor.Dispose()
    if ([IO.Directory]::Exists((Join-Path $root '.didi'))) { [IO.Directory]::Delete((Join-Path $root '.didi')) }
    if ([IO.Directory]::Exists($root)) { [IO.Directory]::Delete($root) }
}
