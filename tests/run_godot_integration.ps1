param(
    [Parameter(Mandatory = $true)]
    [string]$GodotExecutable,
    [string]$Configuration = "Release",
    [int]$StartupTimeoutSeconds = 30
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$sourceFixtureRoot = Join-Path $PSScriptRoot "godot_smoke"
$buildRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot "build"))
$fixtureRoot = [IO.Path]::GetFullPath((Join-Path $buildRoot "godot_phase2_smoke"))
$didiExecutable = Join-Path $repoRoot "build\$Configuration\didi.exe"
$stdoutPath = Join-Path $repoRoot "build\godot_integration.out"
$stderrPath = Join-Path $repoRoot "build\godot_integration.err"

if (-not (Test-Path -LiteralPath $GodotExecutable)) {
    throw "Godot executable not found: $GodotExecutable"
}
if (-not (Test-Path -LiteralPath $didiExecutable)) {
    throw "Didi executable not found: $didiExecutable"
}
if (-not $fixtureRoot.StartsWith($buildRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to recreate an integration fixture outside the build directory: $fixtureRoot"
}

Remove-Item -LiteralPath $fixtureRoot -Recurse -Force -ErrorAction SilentlyContinue
Copy-Item -LiteralPath $sourceFixtureRoot -Destination $fixtureRoot -Recurse

Remove-Item -LiteralPath $stdoutPath, $stderrPath -Force -ErrorAction SilentlyContinue
$godot = $null

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
    return $Response.result.content[0].text | ConvertFrom-Json -Depth 100
}

try {
    $godot = Start-Process -FilePath $GodotExecutable `
        -ArgumentList @("--editor", "--path", $fixtureRoot) `
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

    $tooDeep = @{ leaf = "value" }
    foreach ($level in 1..18) { $tooDeep = @{ nested = $tooDeep } }

    $requests = @(
        (@{ jsonrpc = "2.0"; id = 1; method = "initialize"; params = @{} } | ConvertTo-Json -Compress),
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
        (Tool-Request 105 "scene_close" @{ discard_unsaved = $true })
    )

    $rawResponses = $requests | & $didiExecutable
    $responses = @($rawResponses | Where-Object { $_ -like "{*" } | ForEach-Object { $_ | ConvertFrom-Json -Depth 100 })
    Assert-True ($LASTEXITCODE -eq 0) "Didi MCP process exited with $LASTEXITCODE."
    Assert-True ($responses.Count -eq $requests.Count) "Expected $($requests.Count) JSON-RPC responses, received $($responses.Count)."

    $byId = @{}
    foreach ($response in $responses) { $byId[[int]$response.id] = $response }

    $hierarchy = Tool-Payload $byId[2]
    Assert-True ($hierarchy.execution_mode -eq "live") "Hierarchy was not attributed to live execution."
    Assert-True ($hierarchy.scene_tree.path -eq "/root/SmokeRoot") "Hierarchy root path is not editor-independent: $($hierarchy.scene_tree.path)"
    Assert-True ($hierarchy.scene_tree.children[0].path -eq "/root/SmokeRoot/Subject") "Hierarchy child path is not editor-independent: $($hierarchy.scene_tree.children[0].path)"

    Assert-True ((Tool-Payload $byId[3]).value -eq 7) "Fixture property did not start at 7."
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
    $captureMetadata = $captureMetadataContent[0].text | ConvertFrom-Json -Depth 20
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
    Assert-True ((Tool-Payload $byId[105]).closed -eq $true) "Smoke scene cleanup failed."

    $engineErrors = @(
        Get-Content $stderrPath -ErrorAction SilentlyContinue |
            Where-Object { $_ -match 'UndoRedo history mismatch|Parameter "t" is null|\[ERROR' }
    )
    Assert-True ($engineErrors.Count -eq 0) "Godot reported bridge errors:`n$($engineErrors -join "`n")"

    Write-Output "Godot integration passed: Phase 1 live editing plus Phase 2 project wiring, typed InputMap, scene lifecycle, persistence, UndoRedo, and rejection paths."
}
finally {
    if ($null -ne $godot -and -not $godot.HasExited) {
        Stop-Process -Id $godot.Id
    }
}
