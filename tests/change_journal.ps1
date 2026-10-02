# The change journal: Q15 in docs/BUILD_QUEUE.md, principles P1 and P3 in
# docs/DESIGN_PRINCIPLES.md.
#
# Every mutating call is journalled in the project's .didi/journal.json with
# the undo reference the bridge read off the editor's history, and
# editor_undo's journal_entry undoes one entry on its own. This block proves on
# the attached engine line what the native tests cannot:
#
# - Three writes are read back from godot://project/journal with their values
#   and an undo reference each, and a fresh server process reads the same
#   journal and has the same editor judge it.
# - The editor's judgement is right: the newest entry can be undone, the ones
#   under it cannot, and undoing an entry under later history is refused rather
#   than undoing something else.
# - Undoing an entry from the journal changes exactly what that entry changed,
#   read back from the engine, and the entries it undid are marked.
# - An entry whose place in the history a later change took is refused.
#
# run_godot_integration.ps1 dot-sources this file and calls the block once the
# editor session is attached. change_journal.tscn is its fixture, and nothing
# else opens it.

function Invoke-ChangeJournalBlock {
    param(
        [Parameter(Mandatory = $true)] [object]$EditorSession,
        [Parameter(Mandatory = $true)] [string]$FixtureRoot
    )
    $scene = "res://change_journal.tscn"
    $a = "/root/JournalRoot/A"
    $b = "/root/JournalRoot/B"
    $readJournal = { param($id) @{ jsonrpc = "2.0"; id = $id; method = "resources/read"; params = @{ uri = "godot://project/journal" } } | ConvertTo-Json -Compress }
    $parse = { param($raw)
        $byId = @{}
        foreach ($response in @($raw | Where-Object { $_ -like "{*" } | ForEach-Object { $_ | ConvertFrom-Json } | Where-Object { $_.PSObject.Properties.Name -contains "id" })) { $byId[[int]$response.id] = $response }
        $byId
    }
    $near = { param($x, $y) [Math]::Abs([double]$x - [double]$y) -lt 0.002 }

    # Three writes: A's position, B's position, then A's rotation, so the
    # middle one has later history above it in the same scene's history.
    $first = @(
        (@{ jsonrpc = "2.0"; id = 7300; method = "initialize"; params = @{ protocolVersion = "2024-11-05" } } | ConvertTo-Json -Compress),
        (Tool-Request 7301 "runtime_attach_session" @{ session_id = $EditorSession.session_id }),
        (Tool-Request 7302 "scene_open" @{ scene_path = $scene }),
        (Tool-Request 7303 "scene_set_property" @{ target_node = $a; property_name = "position"; value = @{ x = 1; y = 1 } }),
        (Tool-Request 7304 "scene_set_property" @{ target_node = $b; property_name = "position"; value = @{ x = 2; y = 2 } }),
        (Tool-Request 7305 "scene_set_property" @{ target_node = $a; property_name = "rotation"; value = 0.5 }),
        (& $readJournal 7306)
    )
    $firstById = & $parse (Invoke-Didi -Requests $first -Arguments @("--project", $FixtureRoot))
    foreach ($id in 7302, 7303, 7304, 7305) { [void](Tool-Payload $firstById[$id]) }
    $journalText = [string]$firstById[7306].result.contents[0].text
    $journal = $journalText | ConvertFrom-Json
    Assert-True ($journal.editor.judged -eq $true) "godot://project/journal was not judged by the attached editor: $journalText"
    $entries = @($journal.entries)
    Assert-True ($entries.Count -ge 3) "godot://project/journal did not list the three writes: $journalText"
    # Newest first, so the three writes are the first three entries.
    $aRotation = $entries[0]; $bPosition = $entries[1]; $aPosition = $entries[2]
    Assert-True ($aRotation.tool -eq "scene_set_property" -and $aRotation.target.target_node -eq $a -and $aRotation.target.property_name -eq "rotation") "The newest journal entry is not A's rotation: $($aRotation | ConvertTo-Json -Compress -Depth 8)"
    Assert-True ($bPosition.target.target_node -eq $b -and $aPosition.target.target_node -eq $a -and $aPosition.target.property_name -eq "position") "The journal does not list the writes newest first: $journalText"
    Assert-True ($aRotation.id -eq $bPosition.id + 1 -and $bPosition.id -eq $aPosition.id + 1) "The three writes were not journalled as consecutive entries: $journalText"
    # What the entry holds: the value before, the value read back, where the
    # change is, and the step the editor's history gained.
    Assert-True ((& $near $aPosition.before.x 0) -and (& $near $aPosition.after.value.x 1) -and (& $near $aPosition.after.value.y 1)) "A's position entry does not hold the value before and the value read back: $($aPosition | ConvertTo-Json -Compress -Depth 8)"
    Assert-True ($aPosition.scene -eq $scene -and @($aPosition.undo).Count -eq 1 -and $aPosition.undo[0].scene_path -eq $scene -and $aPosition.undo[0].action -like "Didi: set position*") "A's position entry does not carry its undo step: $($aPosition | ConvertTo-Json -Compress -Depth 8)"
    Assert-True ($aRotation.undo_state.state -eq "available") "The newest entry was not judged undoable on its own: $($aRotation.undo_state | ConvertTo-Json -Compress)"
    Assert-True ($bPosition.undo_state.state -eq "later_history" -and $bPosition.undo_state.later_actions -eq 1) "The entry under one later action was not judged as such: $($bPosition.undo_state | ConvertTo-Json -Compress)"
    Assert-True ($aPosition.undo_state.state -eq "later_history" -and $aPosition.undo_state.later_actions -eq 2) "The entry under two later actions was not judged as such: $($aPosition.undo_state | ConvertTo-Json -Compress)"
    # No answer carries the bridge's undo steps; the server takes them off.
    foreach ($id in 7303, 7304, 7305) {
        Assert-True ([string]$firstById[$id].result.content[0].text -notmatch "undo_steps") "An answer carried the bridge's undo_steps (request $id)."
    }

    # A new server process: the journal is the file, and the editor that holds
    # the history still judges it.
    $second = @(
        (@{ jsonrpc = "2.0"; id = 7310; method = "initialize"; params = @{ protocolVersion = "2024-11-05" } } | ConvertTo-Json -Compress),
        (Tool-Request 7311 "runtime_attach_session" @{ session_id = $EditorSession.session_id }),
        # Under later history: refused, and nothing moves.
        (Tool-Request 7312 "editor_undo" @{ journal_entry = $bPosition.id }),
        (Tool-Request 7313 "scene_get_property" @{ reads = @(@{ target_node = $a; property_name = "rotation" }, @{ target_node = $b; property_name = "position" }) }),
        # The newest, then the one that is newest after it.
        (Tool-Request 7314 "editor_undo" @{ journal_entry = $aRotation.id }),
        (Tool-Request 7315 "editor_undo" @{ journal_entry = $bPosition.id }),
        (Tool-Request 7316 "scene_get_property" @{ reads = @(@{ target_node = $a; property_name = "rotation" }, @{ target_node = $a; property_name = "position" }, @{ target_node = $b; property_name = "position" }) }),
        # A new change takes the undone entries' place in the history.
        (Tool-Request 7317 "scene_set_property" @{ target_node = $a; property_name = "scale"; value = @{ x = 2; y = 2 } }),
        (Tool-Request 7318 "editor_undo" @{ journal_entry = $aRotation.id }),
        (& $readJournal 7319),
        (Tool-Request 7320 "scene_close" @{ discard_unsaved = $true }),
        (Tool-Request 7321 "scene_open" @{ scene_path = "res://main.tscn" })
    )
    $secondById = & $parse (Invoke-Didi -Requests $second -Arguments @("--project", $FixtureRoot))
    $text = { param($id) [string]$secondById[$id].result.content[0].text }

    $refused = (& $text 7312) | ConvertFrom-Json
    Assert-True ($secondById[7312].result.isError -and $refused.error.code -eq 409 -and $refused.error.data.code -eq "undo_entry_later_history" -and $refused.error.data.journal_entry -eq $bPosition.id -and $refused.error.data.undo.later_actions -eq 1 -and $refused.error.data.no_remedy) "Undoing an entry under later history was not refused with the reason: $(& $text 7312)"
    $unmoved = @((Tool-Payload $secondById[7313]).reads)
    Assert-True ((& $near $unmoved[0].value 0.5) -and (& $near $unmoved[1].value.x 2)) "A refused undo by entry moved something: $(& $text 7313)"

    $undoneRotation = Tool-Payload $secondById[7314]
    Assert-True ($undoneRotation.journal_entry -eq $aRotation.id -and $undoneRotation.history -eq "scene" -and $undoneRotation.undone -like "Didi: set rotation*" -and $undoneRotation.observed.version -eq ($aRotation.undo[0].version - 1)) "Undoing the newest entry did not report the history it moved: $(& $text 7314)"
    $undonePosition = Tool-Payload $secondById[7315]
    Assert-True ($undonePosition.journal_entry -eq $bPosition.id -and $undonePosition.undone -like "Didi: set position*") "Undoing the entry that became newest did not undo it: $(& $text 7315)"
    # Exactly what the two entries changed, read from the engine: A keeps the
    # position its own entry set.
    $state = @((Tool-Payload $secondById[7316]).reads)
    Assert-True ((& $near $state[0].value 0) -and (& $near $state[1].value.x 1) -and (& $near $state[1].value.y 1) -and (& $near $state[2].value.x 0) -and (& $near $state[2].value.y 0)) "Undoing two entries from the journal did not leave exactly the third applied: $(& $text 7316)"

    [void](Tool-Payload $secondById[7317])
    $gone = (& $text 7318) | ConvertFrom-Json
    Assert-True ($secondById[7318].result.isError -and $gone.error.data.code -eq "undo_entry_gone") "Undoing an entry whose place a later change took was not refused as gone: $(& $text 7318)"

    $after = ([string]$secondById[7319].result.contents[0].text) | ConvertFrom-Json
    $byEntry = @{}
    foreach ($entry in @($after.entries)) { $byEntry[[int]$entry.id] = $entry }
    Assert-True ($byEntry[[int]$aRotation.id].undone_by -gt 0 -and $byEntry[[int]$bPosition.id].undone_by -gt 0 -and -not $byEntry[[int]$aPosition.id].undone_by) "The journal does not mark which entries were undone: $([string]$secondById[7319].result.contents[0].text)"
    $undoEntry = $byEntry[[int]$byEntry[[int]$aRotation.id].undone_by]
    Assert-True ($undoEntry.tool -eq "editor_undo" -and $undoEntry.undoes -eq $aRotation.id) "The undo by entry was not journalled as an entry of its own: $($undoEntry | ConvertTo-Json -Compress -Depth 8)"
    Assert-True ($byEntry[[int]$aRotation.id].undo_state.state -eq "gone" -and $byEntry[[int]$aPosition.id].undo_state.state -eq "later_history") "The journal's judgement after the undos is wrong: $([string]$secondById[7319].result.contents[0].text)"
    foreach ($id in 7320, 7321) { [void](Tool-Payload $secondById[$id]) }
    Write-Output "Change journal: three writes read back with their undo steps, an entry under later history refused, two entries undone from the journal leaving exactly the third, and a replaced entry refused."
}
