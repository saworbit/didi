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

# scene_pack_branch over a scene open in another tab (#1072). The pack saves
# inside the editor, not through the file writers above, and the tab kept the
# tree it had, so saving it put the old scene back over the pack.
# overwrite: true is the consent to lose that tab's changes, so the tab is
# rebuilt on every line with no discard_unsaved. A second session then closes
# the target and overwrites it with scene_create, the case where no tab holds
# the path: that reload printed "Can't reload scene" on 4.7, which the
# engine-output gate reads.
function Invoke-PackBranchTabBlock {
    param(
        [Parameter(Mandatory = $true)] [object]$EditorSession,
        [Parameter(Mandatory = $true)] [string]$FixtureRoot,
        [Parameter(Mandatory = $true)] [bool]$DirtyStateReadable
    )
    $target = "res://pack_target.tscn"
    $source = "res://pack_source.tscn"
    $attach = {
        param($first)
        (@{ jsonrpc = "2.0"; id = $first; method = "initialize"; params = @{ protocolVersion = "2024-11-05" } } | ConvertTo-Json -Compress)
        (Tool-Request ($first + 1) "runtime_attach_session" @{ session_id = $EditorSession.session_id })
    }
    $responses = {
        param($raw)
        $byId = @{}
        foreach ($response in @($raw | Where-Object { $_ -like "{*" } | ForEach-Object { $_ | ConvertFrom-Json } | Where-Object { $_.PSObject.Properties.Name -contains "id" })) { $byId[[int]$response.id] = $response }
        $byId
    }
    $packRequests = @(& $attach 2758) + @(
        (Tool-Request 2760 "scene_create" @{ scene_path = $target; root_type = "Node2D"; root_name = "PackTarget"; overwrite = $true }),
        (Tool-Request 2761 "editor_save_scene" @{}),
        # An edit the target's tab has not saved, which the pack throws away.
        # The answer says so on 4.7 and cannot on 4.5 and 4.6 (#1082).
        (Tool-Request 2793 "scene_set_property" @{ target_node = "/root/PackTarget"; property_name = "position"; value = @{ x = 3; y = 3 } }),
        (Tool-Request 2762 "scene_create" @{ scene_path = $source; root_type = "Node2D"; root_name = "PackSource"; overwrite = $true }),
        (Tool-Request 2763 "scene_instantiate_node" @{ node_type = "Sprite2D"; parent_path = "/root/PackSource"; name = "Child" }),
        (Tool-Request 2764 "editor_save_scene" @{}),
        (Tool-Request 2765 "scene_pack_branch" @{ target_node = "/root/PackSource/Child"; scene_path = $target; overwrite = $true }),
        (Tool-Request 2766 "scene_get_hierarchy" @{ max_depth = 1 }),
        # The steps #1072 reported: switch to the target's tab and save it.
        (Tool-Request 2767 "scene_open" @{ scene_path = $target }),
        (Tool-Request 2768 "scene_get_hierarchy" @{ max_depth = 1 }),
        (Tool-Request 2769 "editor_save_scene" @{})
    )
    $packById = & $responses (Invoke-Didi -Requests $packRequests -Arguments @("--project", $FixtureRoot, "--yolo"))
    foreach ($id in 2760, 2762, 2763, 2793) { [void](Tool-Payload $packById[$id]) }
    foreach ($id in 2761, 2764, 2769) {
        Assert-True ((Tool-Payload $packById[$id]).status -eq "saved") "The pack scenes were not saved by request ${id}: $($packById[$id].result.content[0].text)"
    }
    $packed = Tool-Payload $packById[2765]
    Assert-True ($packed.saved -eq $true -and $packed.editor_scene_reloaded -eq $true -and $null -eq $packed.editor_copy_error) "A pack over a scene open in another tab did not rebuild that tab: $($packed | ConvertTo-Json -Depth 6 -Compress)"
    $expectedDiscard = if ($DirtyStateReadable) { $true } else { $null }
    Assert-True ($packed.PSObject.Properties.Name -contains "editor_scene_discarded_unsaved" -and $packed.editor_scene_discarded_unsaved -eq $expectedDiscard) "A pack over a tab with an unsaved edit did not say it threw the edit away ($expectedDiscard expected): $($packed | ConvertTo-Json -Depth 6 -Compress)"
    Assert-True ((Tool-Payload $packById[2766]).scene_file_path -eq $source) "Rebuilding the target's tab moved the edited scene off the pack's source."
    $targetTab = Tool-Payload $packById[2768]
    Assert-True ($targetTab.scene_tree.name -eq "Child") "The target's tab still held the scene from before the pack: $($targetTab | ConvertTo-Json -Depth 4 -Compress)"
    $savedText = Get-Content -LiteralPath (Join-Path $FixtureRoot "pack_target.tscn") -Raw
    Assert-True ($savedText.Contains("[node name=`"Child`" type=`"Sprite2D`"") -and -not $savedText.Contains("PackTarget")) "Saving the target's tab put the old scene back over the pack: $savedText"

    $recreateRequests = @(& $attach 2770) + @(
        (Tool-Request 2772 "scene_close" @{ discard_unsaved = $true }),
        (Tool-Request 2773 "scene_create" @{ scene_path = $target; root_type = "Node2D"; root_name = "Recreated"; overwrite = $true }),
        (Tool-Request 2774 "scene_get_hierarchy" @{ max_depth = 1 }),
        (Tool-Request 2775 "scene_close" @{ discard_unsaved = $true }),
        (Tool-Request 2776 "scene_close" @{ discard_unsaved = $true }),
        (Tool-Request 2777 "scene_open" @{ scene_path = "res://main.tscn" })
    )
    $recreateById = & $responses (Invoke-Didi -Requests $recreateRequests -Arguments @("--project", $FixtureRoot, "--yolo"))
    foreach ($id in 2772, 2773, 2775, 2776) { [void](Tool-Payload $recreateById[$id]) }
    Assert-True ((Tool-Payload $recreateById[2774]).scene_tree.name -eq "Recreated") "scene_create did not replace a scene no tab held."
    Assert-True (-not $recreateById[2777].result.isError) "main.tscn did not reopen after the pack block: $($recreateById[2777].result.content[0].text)"
}

# scene_create with overwrite: true over a scene open in a tab that is not the
# current one (#1079). It rebuilt the tab and then opened it in the same
# request, and on 4.5 and 4.6 the editor ignores a scene change for the rest of
# the frame after a rebuild, so a tab right of the current one never came to
# the front and the call answered opened: false. The tab is now made current
# first and rebuilt on a later request. The second overwrite is of a tab left
# of a current last tab, which the rebuild used to leave current by accident on
# those lines, with previous_scene_file_path naming the scene just written.
function Invoke-CreateOverOpenTabBlock {
    param(
        [Parameter(Mandatory = $true)] [object]$EditorSession,
        [Parameter(Mandatory = $true)] [string]$FixtureRoot,
        [Parameter(Mandatory = $true)] [bool]$DirtyStateReadable
    )
    $left = "res://create_tab_left.tscn"
    $right = "res://create_tab_right.tscn"
    $requests = @(
        (@{ jsonrpc = "2.0"; id = 2778; method = "initialize"; params = @{ protocolVersion = "2024-11-05" } } | ConvertTo-Json -Compress),
        (Tool-Request 2779 "runtime_attach_session" @{ session_id = $EditorSession.session_id }),
        (Tool-Request 2780 "scene_create" @{ scene_path = $left; root_type = "Node2D"; root_name = "LeftOld"; overwrite = $true }),
        (Tool-Request 2781 "editor_save_scene" @{}),
        (Tool-Request 2782 "scene_create" @{ scene_path = $right; root_type = "Node2D"; root_name = "RightOld"; overwrite = $true }),
        (Tool-Request 2783 "editor_save_scene" @{}),
        # The right tab is now to the right of the current one.
        (Tool-Request 2784 "scene_open" @{ scene_path = $left }),
        (Tool-Request 2785 "scene_create" @{ scene_path = $right; root_type = "Node2D"; root_name = "RightNew"; overwrite = $true }),
        (Tool-Request 2786 "scene_get_hierarchy" @{ max_depth = 1 }),
        # And the left tab is left of the current last one.
        (Tool-Request 2787 "scene_create" @{ scene_path = $left; root_type = "Node2D"; root_name = "LeftNew"; overwrite = $true }),
        (Tool-Request 2788 "scene_get_hierarchy" @{ max_depth = 1 }),
        (Tool-Request 2789 "scene_close" @{ discard_unsaved = $true }),
        (Tool-Request 2790 "scene_open" @{ scene_path = $right }),
        (Tool-Request 2791 "scene_close" @{ discard_unsaved = $true }),
        (Tool-Request 2792 "scene_open" @{ scene_path = "res://main.tscn" })
    )
    $raw = Invoke-Didi -Requests $requests -Arguments @("--project", $FixtureRoot, "--yolo")
    $byId = @{}
    foreach ($response in @($raw | Where-Object { $_ -like "{*" } | ForEach-Object { $_ | ConvertFrom-Json } | Where-Object { $_.PSObject.Properties.Name -contains "id" })) { $byId[[int]$response.id] = $response }
    foreach ($id in 2780, 2782, 2784, 2789, 2790, 2791) { [void](Tool-Payload $byId[$id]) }
    foreach ($id in 2781, 2783) {
        Assert-True ((Tool-Payload $byId[$id]).status -eq "saved") "The create-over-tab scenes were not saved by request ${id}: $($byId[$id].result.content[0].text)"
    }
    foreach ($case in @(
            @{ Id = 2785; Hierarchy = 2786; Scene = $right; Root = "RightNew"; Previous = $left; Where = "right of the current one" },
            @{ Id = 2787; Hierarchy = 2788; Scene = $left; Root = "LeftNew"; Previous = $right; Where = "left of a current last tab" })) {
        $created = $byId[$case.Id]
        Assert-True (-not $created.result.isError) "scene_create over a scene open in a tab $($case.Where) did not open it: $($created.result.content[0].text)"
        $payload = Tool-Payload $created
        Assert-True ($payload.opened -eq $true -and $payload.editor_scene_reloaded -eq $true -and $null -eq $payload.editor_copy_error -and $null -eq $payload.scene_tab_stale) "scene_create over a tab $($case.Where) did not open and rebuild it: $($payload | ConvertTo-Json -Depth 6 -Compress)"
        Assert-True ($payload.previous_scene_file_path -eq $case.Previous -and $payload.edited_scene_changed -eq $true) "scene_create over a tab $($case.Where) named the wrong previous scene: $($payload | ConvertTo-Json -Depth 6 -Compress)"
        # Both tabs were saved, so nothing was thrown away, where the engine can say.
        $expectedDiscard = if ($DirtyStateReadable) { $false } else { $null }
        Assert-True ($payload.PSObject.Properties.Name -contains "editor_scene_discarded_unsaved" -and $payload.editor_scene_discarded_unsaved -eq $expectedDiscard) "scene_create over a saved tab $($case.Where) did not say whether it lost changes ($expectedDiscard expected): $($payload | ConvertTo-Json -Depth 6 -Compress)"
        $hierarchy = Tool-Payload $byId[$case.Hierarchy]
        Assert-True ($hierarchy.scene_file_path -eq $case.Scene -and $hierarchy.scene_tree.name -eq $case.Root) "After scene_create over a tab $($case.Where), the edited scene was not the new one: $($hierarchy | ConvertTo-Json -Depth 4 -Compress)"
    }
    Assert-True (-not $byId[2792].result.isError) "main.tscn did not reopen after the create-over-tab block: $($byId[2792].result.content[0].text)"
}
