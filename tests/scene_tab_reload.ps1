# A file writer rewriting a scene that is open in an editor tab (#1068).
#
# A tab is a node tree the editor built from the file, not a cached copy, so a
# writer that refreshed only the resource cache left it as it was, and the next
# editor_save_scene put the old scene back over the file with nothing said. A
# writer now rebuilds a clean tab from the new file, and stops before writing
# anything at a tab that has unsaved changes or, before Godot 4.7, one the
# engine cannot show has none. discard_unsaved is the caller accepting the loss.
#
# run_godot_integration.ps1 dot-sources this file and calls the block once the
# editor session is attached. The scene is built through the tools, so the
# fixture gains no file an earlier block could trip over.

function Invoke-SceneTabReloadBlock {
    param(
        [Parameter(Mandatory = $true)] [object]$EditorSession,
        [Parameter(Mandatory = $true)] [string]$FixtureRoot,
        [Parameter(Mandatory = $true)] [bool]$DirtyStateReadable
    )
    $tabScene = "res://tab_reload.tscn"
    $tabScript = "res://tab_reload.gd"
    $oldMethod = "_on_tab_reload_child_changed"
    $newMethod = "_on_tab_reload_child_shown"
    $rename = @{ target = $oldMethod; new_name = $newMethod }
    $tabRequests = @(
        (@{ jsonrpc = "2.0"; id = 2740; method = "initialize"; params = @{ protocolVersion = "2024-11-05" } } | ConvertTo-Json -Compress),
        (Tool-Request 2741 "runtime_attach_session" @{ session_id = $EditorSession.session_id }),
        # Both methods, so the connection names one the script has before and
        # after the rename and the editor has nothing to print about it.
        (Tool-Request 2742 "script_create" @{ script_path = $tabScript; overwrite = $true; source_text = "extends Node2D`n`nfunc ${oldMethod}():`n`tpass`n`nfunc ${newMethod}():`n`tpass`n" }),
        (Tool-Request 2743 "scene_create" @{ scene_path = $tabScene; root_type = "Node2D"; root_name = "TabReload"; overwrite = $true }),
        (Tool-Request 2744 "scene_instantiate_node" @{ node_type = "Node2D"; parent_path = "/root/TabReload"; name = "Child" }),
        (Tool-Request 2745 "script_attach_to_node" @{ target_node = "/root/TabReload"; script_path = $tabScript }),
        (Tool-Request 2746 "signal_connect" @{ emitter_node = "/root/TabReload/Child"; signal_name = "visibility_changed"; target_node = "/root/TabReload"; target_method = $oldMethod }),
        (Tool-Request 2747 "editor_save_scene" @{}),
        # An edit the tab has not saved. Refused on every line.
        (Tool-Request 2748 "scene_set_property" @{ target_node = "/root/TabReload/Child"; property_name = "position"; value = @{ x = 5; y = 5 } }),
        (Tool-Request 2749 "project_rename_references" $rename),
        (Tool-Request 2750 "editor_save_scene" @{}),
        # Saved. 4.7 shows the tab clean and rebuilds it; 4.5 and 4.6 cannot.
        (Tool-Request 2751 "project_rename_references" $rename),
        (Tool-Request 2752 "project_rename_references" ($rename + @{ discard_unsaved = $true })),
        # The steps #1068 reported: an edit and a save after the rename.
        (Tool-Request 2753 "scene_set_property" @{ target_node = "/root/TabReload/Child"; property_name = "position"; value = @{ x = 9; y = 9 } }),
        (Tool-Request 2754 "editor_save_scene" @{}),
        (Tool-Request 2755 "scene_get_hierarchy" @{ max_depth = 1 }),
        (Tool-Request 2756 "scene_close" @{ discard_unsaved = $true }),
        (Tool-Request 2757 "scene_open" @{ scene_path = "res://main.tscn" })
    )
    $rawTab = Invoke-Didi -Requests $tabRequests -Arguments @("--project", $FixtureRoot, "--yolo")
    $tabById = @{}
    foreach ($response in @($rawTab | Where-Object { $_ -like "{*" } | ForEach-Object { $_ | ConvertFrom-Json } | Where-Object { $_.PSObject.Properties.Name -contains "id" })) { $tabById[[int]$response.id] = $response }
    foreach ($id in 2742, 2743, 2744, 2745, 2746) { [void](Tool-Payload $tabById[$id]) }
    foreach ($id in 2747, 2750, 2754) {
        Assert-True ((Tool-Payload $tabById[$id]).status -eq "saved") "The tab scene was not saved by request ${id}: $($tabById[$id].result.content[0].text)"
    }
    $refusal = { param($id) $tabById[$id].result.content[0].text | ConvertFrom-Json }
    $dirtyCode = if ($DirtyStateReadable) { "unsaved_changes" } else { "dirty_state_unavailable" }

    Assert-True $tabById[2749].result.isError "A rename rewrote a scene open with unsaved changes: $($tabById[2749].result.content[0].text)"
    $unsavedRefusal = & $refusal 2749
    Assert-True ($unsavedRefusal.error.code -eq 409 -and $unsavedRefusal.error.data.code -eq $dirtyCode -and $unsavedRefusal.error.data.retry_with.discard_unsaved -eq $true -and @($unsavedRefusal.error.data.open_scenes) -contains $tabScene) "A rename over an unsaved tab did not refuse as $dirtyCode, naming the scene and discard_unsaved: $($tabById[2749].result.content[0].text)"

    if ($DirtyStateReadable) {
        $cleanRename = Tool-Payload $tabById[2751]
        Assert-True (@($cleanRename.editor_scenes_reloaded) -contains $tabScene -and $null -eq $cleanRename.editor_copy_errors) "A rename of a scene open in a saved tab did not rebuild the tab: $($cleanRename | ConvertTo-Json -Depth 6 -Compress)"
        # The name is now used by the connection, so asking again is the
        # rename's own collision refusal, not a second rewrite.
        Assert-True $tabById[2752].result.isError "A second rename onto a name the scene already uses was not refused: $($tabById[2752].result.content[0].text)"
    }
    else {
        $unknownRefusal = & $refusal 2751
        Assert-True ($tabById[2751].result.isError -and $unknownRefusal.error.data.code -eq "dirty_state_unavailable" -and $unknownRefusal.error.data.retry_with.discard_unsaved -eq $true) "Before 4.7 a rename over a saved tab did not refuse as dirty_state_unavailable: $($tabById[2751].result.content[0].text)"
        $discardRename = Tool-Payload $tabById[2752]
        Assert-True (@($discardRename.editor_scenes_reloaded) -contains $tabScene -and $null -eq $discardRename.editor_copy_errors) "A rename with discard_unsaved did not rebuild the open tab: $($discardRename | ConvertTo-Json -Depth 6 -Compress)"
    }

    # The rebuilt tab is still the one the scene tools act on, and the save
    # after the rename keeps the rename.
    $tabHierarchy = Tool-Payload $tabById[2755]
    Assert-True ($tabHierarchy.scene_file_path -eq $tabScene) "Rebuilding the tab moved the edited scene: $($tabHierarchy.scene_file_path)"
    $savedText = Get-Content -LiteralPath (Join-Path $FixtureRoot "tab_reload.tscn") -Raw
    Assert-True ($savedText.Contains("method=`"$newMethod`"") -and -not $savedText.Contains("method=`"$oldMethod`"")) "A save after the rename put the old connection back over the file: $savedText"
    Assert-True ($savedText.Contains("position = Vector2(9, 9)")) "The edit made after the rename did not reach the file: $savedText"
    Assert-True (-not $tabById[2757].result.isError) "main.tscn did not reopen after the tab reload block: $($tabById[2757].result.content[0].text)"
}
