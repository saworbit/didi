param([string]$InputPath, [ValidateSet('Apply','Scenario')][string]$Case)
$ErrorActionPreference = 'Stop'
function Assert-True([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
. (Join-Path $PSScriptRoot 'observed_post_state.ps1')
. (Join-Path $PSScriptRoot 'observed_external.ps1')
$inputData = Get-Content -LiteralPath $InputPath -Raw | ConvertFrom-Json
if ($Case -eq 'Apply') {
    Assert-ProjectApplyObservedState $inputData.answer $inputData.held $inputData.path $inputData.expected $inputData.file_text
} else {
    Assert-ScenarioObservedTeardown $inputData.answer $inputData.game_running $inputData.editor_running
}
Write-Output "External observed $Case case agrees with the engine."
