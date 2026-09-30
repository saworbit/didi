# Observed, not asserted: Q2 in docs/BUILD_QUEUE.md, principle P1 in
# docs/DESIGN_PRINCIPLES.md.
#
# Every mutating tool the server implements has one entry in
# observed_post_state.json. Either it names the answer fields that carry the
# state the tool read back after its write, or it exempts the tool with the
# reason and the issue that tracks the gap. run_godot_integration.ps1
# dot-sources this file and checks three things, on every engine line:
#
# - Coverage. The registry names exactly the tools the built server classifies
#   as mutating, and every checked tool has a case below. A new mutating tool
#   with neither a case nor an exemption fails here, and in
#   tests/test_observed_post_state.py on every build.
# - Presence. Every exchange the harness makes goes through Invoke-Didi, which
#   records the checked tools' answers here. Every successful answer, dry runs
#   aside, carries every observed field, wherever in the run it was made.
# - Agreement. Each case drives one tool against observed_post_state.tscn or
#   the attached game, then reads the same state back through something that
#   shares no code with the tool: observed_witness.gd for the editor, the
#   game's own tree for runtime state. What the answer says it observed has to
#   match what the engine then reports.
#
# A field counts as observed when the tool reads it after the write, or reads
# the same state after the write and refuses the call when it differs. A field
# copied from the request, or read before the write, does not, however it is
# named; old_value, before and previous are all of the second kind.

$observedRegistry = Get-Content -LiteralPath (Join-Path $PSScriptRoot "observed_post_state.json") -Raw | ConvertFrom-Json
$observedFields = @{}
foreach ($entry in $observedRegistry.tools.PSObject.Properties) {
    if ($null -ne $entry.Value.PSObject.Properties["observed"]) {
        $observedFields[$entry.Name] = @($entry.Value.observed)
    }
}
$observedExchanges = New-Object System.Collections.ArrayList
$observedScenePath = "res://observed_post_state.tscn"
$observedRoot = "/root/ObservedRoot"

# Called by Invoke-Didi with each batch and what came back. Only the checked
# tools' answers are kept, and only those are parsed: the run's other
# responses include captures and ten-thousand-node trees.
function Add-ObservedExchanges([object[]]$Requests, [object[]]$Lines) {
    $calls = @{}
    foreach ($request in $Requests) {
        $text = [string]$request
        if ($text -notmatch '"tools/call"') { continue }
        $parsed = $text | ConvertFrom-Json
        if ($observedFields.ContainsKey([string]$parsed.params.name)) { $calls[[string]$parsed.id] = $parsed }
    }
    if ($calls.Count -eq 0) { return }
    foreach ($line in $Lines) {
        # The server writes object keys in sorted order, so "id" leads.
        $match = [regex]::Match([string]$line, '^\{"id":(\d+),')
        if (-not $match.Success -or -not $calls.ContainsKey($match.Groups[1].Value)) { continue }
        $call = $calls[$match.Groups[1].Value]
        [void]$observedExchanges.Add([pscustomobject]@{ Tool = [string]$call.params.name; Arguments = $call.params.arguments; Response = ([string]$line | ConvertFrom-Json) })
    }
}

function Get-ObservedPayload($Response) {
    if ($null -eq $Response -or $null -eq $Response.result -or $Response.result.isError) { return $null }
    if ($null -ne $Response.result.PSObject.Properties["structuredContent"]) { return $Response.result.structuredContent }
    return $Response.result.content[0].text | ConvertFrom-Json
}

function Test-ObservedNumber($Value) {
    return $Value -is [int] -or $Value -is [long] -or $Value -is [double] -or $Value -is [decimal] -or $Value -is [single]
}

# Whether what an answer says it observed matches what the witness read. An
# object in the answer has to match on every field it carries; the witness may
# say more. Numbers agree to float32 precision, because every engine value here
# passes through a float somewhere and the two readers print it differently.
function Test-ObservedAgreement($Observed, $Witnessed) {
    if ($null -eq $Observed -or $null -eq $Witnessed) { return ($null -eq $Observed) -and ($null -eq $Witnessed) }
    if ((Test-ObservedNumber $Observed) -and (Test-ObservedNumber $Witnessed)) {
        $scale = [Math]::Max(1.0, [Math]::Max([Math]::Abs([double]$Observed), [Math]::Abs([double]$Witnessed)))
        return [Math]::Abs([double]$Observed - [double]$Witnessed) -le (1e-6 * $scale)
    }
    if ($Observed -is [bool] -or $Witnessed -is [bool] -or $Observed -is [string] -or $Witnessed -is [string]) {
        return ($Observed.GetType() -eq $Witnessed.GetType()) -and ($Observed -ceq $Witnessed)
    }
    if ($Observed -is [System.Collections.IList]) {
        if ($Witnessed -isnot [System.Collections.IList] -or $Observed.Count -ne $Witnessed.Count) { return $false }
        for ($index = 0; $index -lt $Observed.Count; $index++) {
            if (-not (Test-ObservedAgreement $Observed[$index] $Witnessed[$index])) { return $false }
        }
        return $true
    }
    # Not -is [pscustomobject], which is true of every object PowerShell wraps.
    if ($Observed -is [System.Management.Automation.PSCustomObject]) {
        if ($Witnessed -isnot [System.Management.Automation.PSCustomObject]) { return $false }
        foreach ($property in $Observed.PSObject.Properties) {
            $other = $Witnessed.PSObject.Properties[$property.Name]
            if ($null -eq $other -or -not (Test-ObservedAgreement $property.Value $other.Value)) { return $false }
        }
        return $true
    }
    return $false
}

function Step([string]$Name, [string]$Tool, [hashtable]$Arguments) {
    return @{ Name = $Name; Tool = $Tool; Arguments = $Arguments }
}

function Witness([string]$Name, [string]$Method, [object[]]$Arguments) {
    return Step $Name "scene_call_method" @{ target_node = "$observedRoot/Witness"; method_name = $Method; arguments = @($Arguments) }
}

function Agree([string]$Field, $Observed, $Witnessed) {
    return @{ Field = $Field; Observed = $Observed; Witnessed = $Witnessed }
}

# The one child a call added, as the witness saw it, or everything it saw
# added when that is not exactly one, so a disagreement says what appeared.
function Get-ObservedNewChild($Before, $After) {
    $added = @(@($After) | Where-Object { @($Before) -notcontains $_ })
    if ($added.Count -eq 1) { return $added[0] }
    return ,$added
}

# The cases, in the order they run. Each has exactly one step named "call",
# which is the tool under test; the rest set up or read. Agree gets every
# step's payload by name, and scene_call_method's payload carries the
# witness's answer in returned.
function Get-ObservedPostStateCases {
    return @(
        @{ Tool = "scene_open"; Session = "editor"; Steps = @(
            (Step "call" "scene_open" @{ scene_path = $observedScenePath }),
            (Witness "witness" "edited_scene_path" @()))
           Agree = { param($s) Agree "scene_path" $s.call.scene_path $s.witness.returned } },
        @{ Tool = "scene_set_property"; Session = "editor"; Steps = @(
            (Step "call" "scene_set_property" @{ target_node = "$observedRoot/Subject"; property_name = "process_priority"; value = 11 }),
            (Witness "witness" "property" @("Subject", "process_priority")))
           Agree = { param($s)
               Agree "value" $s.call.value $s.witness.returned
               Agree "applied" $s.call.applied ($s.witness.returned -eq 11) } },
        # A write the engine discards, so an answer that repeats the request
        # disagrees with the witness here, where the case above cannot tell the
        # two apart. A Control nested in a Control starts in layout_mode 0, which
        # ignores anchors_preset; see request 926 in run_godot_integration.ps1.
        @{ Tool = "scene_set_property"; Session = "editor"; Steps = @(
            (Step "holder" "scene_instantiate_node" @{ node_type = "Control"; parent_path = $observedRoot; name = "Holder" }),
            (Step "nested" "scene_instantiate_node" @{ node_type = "Control"; parent_path = "$observedRoot/Holder"; name = "Discarding" }),
            (Step "call" "scene_set_property" @{ target_node = "$observedRoot/Holder/Discarding"; property_name = "anchors_preset"; value = 15 }),
            (Witness "witness" "property" @("Holder/Discarding", "anchors_preset")))
           Agree = { param($s)
               Agree "value" $s.call.value $s.witness.returned
               Agree "applied" $s.call.applied ($s.witness.returned -eq 15) } },
        @{ Tool = "scene_instantiate_node"; Session = "editor"; Steps = @(
            (Witness "before" "children" @(".")),
            (Step "call" "scene_instantiate_node" @{ node_type = "Node3D"; parent_path = $observedRoot; name = "Spawned" }),
            (Witness "after" "children" @(".")))
           Agree = { param($s) Agree "node_path" $s.call.node_path (Get-ObservedNewChild $s.before.returned $s.after.returned) } },
        @{ Tool = "scene_duplicate_node"; Session = "editor"; Steps = @(
            (Witness "before" "children" @(".")),
            (Step "call" "scene_duplicate_node" @{ target_node = "$observedRoot/Spawned" }),
            (Witness "after" "children" @(".")))
           Agree = { param($s) Agree "duplicated_node" $s.call.duplicated_node (Get-ObservedNewChild $s.before.returned $s.after.returned) } },
        # The copy just made, moved and then removed, each answer read after
        # its commit (#1019).
        @{ Tool = "scene_reparent_node"; Session = "editor"; Steps = @(
            (Step "call" "scene_reparent_node" @{ target_node = "$observedRoot/SpawnedCopy"; new_parent_path = "$observedRoot/Subject" }),
            (Witness "witness" "node_info" @("Subject/SpawnedCopy")))
           Agree = { param($s) Agree "node_path" $s.call.node_path $s.witness.returned.path } },
        @{ Tool = "scene_remove_node"; Session = "editor"; Steps = @(
            (Step "call" "scene_remove_node" @{ target_node = "$observedRoot/Subject/SpawnedCopy" }),
            (Witness "witness" "node_info" @("Subject/SpawnedCopy")))
           Agree = { param($s) Agree "exists" $s.call.exists $s.witness.returned.exists } },
        @{ Tool = "script_attach_to_node"; Session = "editor"; Steps = @(
            (Step "call" "script_attach_to_node" @{ target_node = "$observedRoot/Spawned"; script_path = "res://subject.gd" }),
            (Witness "witness" "node_info" @("Spawned")))
           Agree = { param($s) Agree "script_path" $s.call.script_path $s.witness.returned.script_path } },
        @{ Tool = "script_detach_from_node"; Session = "editor"; Steps = @(
            (Step "call" "script_detach_from_node" @{ target_node = "$observedRoot/Spawned" }),
            (Witness "witness" "node_info" @("Spawned")))
           Agree = { param($s) Agree "script_path" $s.call.script_path $s.witness.returned.script_path } },
        @{ Tool = "signal_connect"; Session = "editor"; Steps = @(
            (Step "call" "signal_connect" @{ emitter_node = "$observedRoot/Subject"; signal_name = "visibility_changed"; target_node = "$observedRoot/Spawned"; target_method = "notify_property_list_changed" }),
            (Witness "witness" "connection_flags" @("Subject", "visibility_changed", "Spawned", "notify_property_list_changed")))
           Agree = { param($s) Agree "connected" $s.call.connected ($s.witness.returned -ge 0) } },
        @{ Tool = "signal_disconnect"; Session = "editor"; Steps = @(
            (Step "call" "signal_disconnect" @{ emitter_node = "$observedRoot/Subject"; signal_name = "visibility_changed"; target_node = "$observedRoot/Spawned"; target_method = "notify_property_list_changed" }),
            (Witness "witness" "connection_flags" @("Subject", "visibility_changed", "Spawned", "notify_property_list_changed")))
           Agree = { param($s) Agree "disconnected" $s.call.disconnected ($s.witness.returned -eq -1) } },
        @{ Tool = "shader_set_uniform"; Session = "editor"; Steps = @(
            (Step "call" "shader_set_uniform" @{ target_node = "$observedRoot/Shaded"; property_name = "material_override"; uniform_name = "strength"; value = 0.25 }),
            (Witness "witness" "shader_parameter" @("Shaded", "material_override", "strength")))
           Agree = { param($s)
               Agree "value" $s.call.value $s.witness.returned
               Agree "applied" $s.call.applied ($s.witness.returned -eq 0.25) } },
        @{ Tool = "viewport_set_camera_transform"; Session = "editor"; Steps = @(
            (Step "call" "viewport_set_camera_transform" @{ camera_path = "$observedRoot/Camera"; position = @{ x = 1.25; y = -2.5; z = 3.75 }; rotation_degrees = @{ x = 10; y = 20; z = 30 }; fov = 73 }),
            (Witness "witness" "camera" @("Camera")))
           Agree = { param($s) Agree "new" $s.call.new $s.witness.returned } },
        @{ Tool = "viewport_toggle_debug_draw"; Session = "editor"; Steps = @(
            (Witness "before" "debug_hints" @()),
            (Step "call" "viewport_toggle_debug_draw" @{ collision_shapes = $true; navigation_mesh = $true }),
            (Witness "witness" "debug_hints" @()),
            # Put back what the editor had, so a game run later draws as before.
            (Step "restore" "viewport_toggle_debug_draw" @{ collision_shapes = $false; navigation_mesh = $false }))
           Agree = { param($s) Agree "observed" $s.call.observed $s.witness.returned } },
        @{ Tool = "editor_undo"; Session = "editor"; Steps = @(
            (Step "setup" "scene_set_property" @{ target_node = "$observedRoot/Subject"; property_name = "process_priority"; value = 21 }),
            (Step "call" "editor_undo" @{}),
            (Witness "witness" "property" @("Subject", "process_priority")))
           Agree = { param($s) Agree "history" $s.call.history $(if ($s.witness.returned -eq 11) { "scene" } else { "not the scene" }) } },
        @{ Tool = "editor_redo"; Session = "editor"; Steps = @(
            (Step "call" "editor_redo" @{}),
            (Witness "witness" "property" @("Subject", "process_priority")))
           Agree = { param($s) Agree "history" $s.call.history $(if ($s.witness.returned -eq 21) { "scene" } else { "not the scene" }) } },
        @{ Tool = "anim_add_library"; Session = "editor"; Steps = @(
            (Step "animation" "resource_create" @{ save_path = "res://observed_anim.tres"; resource_type = "Animation"; properties = @{ length = 0.5 } }),
            (Step "library" "resource_create" @{ save_path = "res://observed_library.tres"; resource_type = "AnimationLibrary"; properties = @{ _data = @{ walk = @{ type = "ExtResource"; path = "res://observed_anim.tres" } } } }),
            (Step "call" "anim_add_library" @{ animation_player_path = "$observedRoot/Player"; library_path = "res://observed_library.tres"; library_name = "observed" }),
            (Witness "witness" "animation_libraries" @("Player")))
           Agree = { param($s) Agree "library_names" @($s.call.library_names | Sort-Object) $s.witness.returned.library_names } },
        @{ Tool = "scene_pack_branch"; Session = "editor"; Steps = @(
            (Step "call" "scene_pack_branch" @{ target_node = "$observedRoot/Subject"; scene_path = "res://observed_packed.tscn" }),
            (Witness "witness" "load_fresh" @("res://observed_packed.tscn", @())))
           Agree = { param($s)
               Agree "uid" $s.call.uid $s.witness.returned.uid
               Agree "uid_registered" $s.call.uid_registered $s.witness.returned.uid_registered } },
        @{ Tool = "script_create"; Session = "editor"; Steps = @(
            (Step "call" "script_create" @{ script_path = "res://observed_created.gd"; source_text = "extends Node`n`n`nfunc answer() -> int:`n`treturn 1`n" }),
            (Witness "witness" "load_fresh" @("res://observed_created.gd", @())))
           Agree = { param($s) Agree "has_errors" $s.call.has_errors (@($s.witness.returned.methods) -notcontains "answer") } },
        @{ Tool = "script_patch_method"; Session = "editor"; Steps = @(
            (Step "call" "script_patch_method" @{ file_path = "res://observed_created.gd"; method_name = "answer"; new_definition = "func answer() -> int:`n`treturn 2`n" }),
            (Witness "witness" "load_fresh" @("res://observed_created.gd", @())))
           Agree = { param($s) Agree "has_errors" $s.call.has_errors (@($s.witness.returned.methods) -notcontains "answer") } },
        @{ Tool = "project_add_export_preset"; Session = "editor"; Steps = @(
            (Step "call" "project_add_export_preset" @{ name = "Observed Preset"; platform = "Linux" }),
            (Witness "witness" "export_preset" @("Observed Preset")))
           Agree = { param($s)
               Agree "preset" $s.call.preset $s.witness.returned.preset
               Agree "preset_count" $s.call.preset_count $s.witness.returned.preset_count } },
        @{ Tool = "audio_add_bus"; Session = "editor"; Steps = @(
            (Step "call" "audio_add_bus" @{ name = "Observed" }),
            (Witness "witness" "audio_bus" @("Observed")))
           Agree = { param($s)
               Agree "after" $s.call.after $s.witness.returned.after
               Agree "bus_count" $s.call.bus_count $s.witness.returned.bus_count } },
        @{ Tool = "audio_configure_bus"; Session = "editor"; Steps = @(
            (Step "call" "audio_configure_bus" @{ bus = "Observed"; volume_db = -6.5; mute = $true }),
            (Witness "witness" "audio_bus" @("Observed")))
           Agree = { param($s) Agree "after" $s.call.after $s.witness.returned.after } },
        # The OGG the loop block wrote and imported earlier in the run.
        @{ Tool = "asset_configure_import"; Session = "editor"; Steps = @(
            (Step "call" "asset_configure_import" @{ asset_path = "res://loop_track.ogg"; options = @{ loop = $false; loop_offset = 0.2 } }),
            (Witness "witness" "load_fresh" @("res://loop_track.ogg", @("loop", "loop_offset"))))
           Agree = { param($s)
               Agree "stream.class" $s.call.stream.class $s.witness.returned.class
               Agree "stream.properties" $s.call.stream.properties $s.witness.returned.properties } },
        # Last among the editor cases: creating a scene makes it the edited one,
        # and the witness lives in observed_post_state.tscn, so the case opens
        # that again before reading.
        @{ Tool = "scene_create"; Session = "editor"; Steps = @(
            (Step "call" "scene_create" @{ scene_path = "res://observed_created.tscn"; root_type = "Node2D"; root_name = "Created" }),
            (Step "return" "scene_open" @{ scene_path = $observedScenePath }),
            (Witness "witness" "load_fresh" @("res://observed_created.tscn", @())))
           Agree = { param($s)
               Agree "uid" $s.call.uid $s.witness.returned.uid
               Agree "uid_registered" $s.call.uid_registered $s.witness.returned.uid_registered } },
        # Membership read back after the commit, not the constant the answer
        # used to carry (#1019).
        @{ Tool = "scene_add_to_group"; Session = "editor"; Steps = @(
            (Step "call" "scene_add_to_group" @{ target_node = "$observedRoot/Subject"; group = "observed_group" }),
            (Witness "witness" "in_group" @("Subject", "observed_group")))
           Agree = { param($s) Agree "in_group" $s.call.in_group $s.witness.returned } },
        @{ Tool = "scene_remove_from_group"; Session = "editor"; Steps = @(
            (Step "call" "scene_remove_from_group" @{ target_node = "$observedRoot/Subject"; group = "observed_group" }),
            (Witness "witness" "in_group" @("Subject", "observed_group")))
           Agree = { param($s) Agree "in_group" $s.call.in_group $s.witness.returned } },
        # Didi's own file, read back after the save and compared with the file
        # as Godot parses it (#1019). One board, in order: a write, a patch,
        # a task through its life, then the clear that leaves it empty.
        @{ Tool = "blackboard_write"; Session = "editor"; Steps = @(
            (Step "call" "blackboard_write" @{ board = "observed_probe"; path = "probe.alpha"; value = 7; author = "observed" }),
            (Witness "witness" "board_file" @("observed_probe")))
           Agree = { param($s)
               Agree "revision" $s.call.revision $s.witness.returned.revision
               Agree "metadata" $s.call.metadata $s.witness.returned.meta.'probe.alpha'
               Agree "value" $s.call.value $s.witness.returned.state.probe.alpha } },
        @{ Tool = "blackboard_patch"; Session = "editor"; Steps = @(
            (Step "call" "blackboard_patch" @{ board = "observed_probe"; operations = @(@{ op = "replace"; path = "/probe/alpha"; value = 8 }) }),
            (Witness "witness" "board_file" @("observed_probe")))
           Agree = { param($s) Agree "revision" $s.call.revision $s.witness.returned.revision } },
        @{ Tool = "blackboard_task_create"; Session = "editor"; Steps = @(
            (Step "call" "blackboard_task_create" @{ board = "observed_probe"; task_id = "observed-task"; title = "Observed task" }),
            (Witness "witness" "board_file" @("observed_probe")))
           Agree = { param($s) Agree "task" $s.call.task $s.witness.returned.tasks.'observed-task' } },
        @{ Tool = "blackboard_task_claim"; Session = "editor"; Steps = @(
            (Step "call" "blackboard_task_claim" @{ board = "observed_probe"; agent_id = "observed-agent"; task_id = "observed-task" }),
            (Witness "witness" "board_file" @("observed_probe")))
           Agree = { param($s) Agree "task" $s.call.task $s.witness.returned.tasks.'observed-task' } },
        @{ Tool = "blackboard_task_update"; Session = "editor"; Steps = @(
            (Step "call" "blackboard_task_update" @{ board = "observed_probe"; task_id = "observed-task"; agent_id = "observed-agent"; progress = 50; note = "halfway" }),
            (Witness "witness" "board_file" @("observed_probe")))
           Agree = { param($s) Agree "task" $s.call.task $s.witness.returned.tasks.'observed-task' } },
        @{ Tool = "blackboard_task_complete"; Session = "editor"; Steps = @(
            (Step "call" "blackboard_task_complete" @{ board = "observed_probe"; task_id = "observed-task"; agent_id = "observed-agent" }),
            (Witness "witness" "board_file" @("observed_probe")))
           Agree = { param($s) Agree "task" $s.call.task $s.witness.returned.tasks.'observed-task' } },
        @{ Tool = "blackboard_clear"; Session = "editor"; Steps = @(
            (Step "call" "blackboard_clear" @{ board = "observed_probe"; path = "probe" }),
            (Witness "witness" "board_file" @("observed_probe")))
           Agree = { param($s) Agree "revision" $s.call.revision $s.witness.returned.revision } },
        # The save reads the file it wrote, since save_scene answers OK whether
        # or not the editor wrote it (#1019). Last of the editor cases, so the
        # observed scene goes to disk with every edit above.
        @{ Tool = "editor_save_scene"; Session = "editor"; Steps = @(
            (Step "call" "editor_save_scene" @{}),
            (Witness "witness" "file_length" @($observedScenePath)))
           Agree = { param($s) Agree "file_bytes" $s.call.file_bytes $s.witness.returned } },
        @{ Tool = "runtime_set_paused"; Session = "game"; Steps = @(
            (Step "call" "runtime_set_paused" @{ paused = $false }),
            (Step "witness" "runtime_get_tree" @{ root_path = "/root/RuntimeRoot"; max_depth = 1 }))
           Agree = { param($s) Agree "paused" $s.call.paused $s.witness.paused } },
        # Slow on purpose: the probe animation is a second long, and a software
        # rendered runner can take that long to reach the read.
        @{ Tool = "anim_play_track"; Session = "game"; Steps = @(
            (Step "call" "anim_play_track" @{ animation_player_path = "/root/RuntimeRoot/Spatial/Player"; animation_name = "probe"; custom_speed = 0.01 }),
            (Step "witness" "eval_gdscript" @{ expression = "node.get('current_animation')"; context_node = "/root/RuntimeRoot/Spatial/Player" }))
           Agree = { param($s) Agree "playing" $s.call.playing ($s.witness.value -eq "probe") } },
        @{ Tool = "runtime_set_paused"; Session = "game"; Steps = @(
            (Step "call" "runtime_set_paused" @{ paused = $true }),
            (Step "witness" "runtime_get_tree" @{ root_path = "/root/RuntimeRoot"; max_depth = 1 }))
           Agree = { param($s) Agree "paused" $s.call.paused $s.witness.paused } },
        @{ Tool = "runtime_step"; Session = "game"; Steps = @(
            (Step "call" "runtime_step" @{ frames = 2 }),
            (Step "witness" "runtime_get_tree" @{ root_path = "/root/RuntimeRoot"; max_depth = 1 }))
           Agree = { param($s) Agree "paused" $s.call.paused $s.witness.paused } }
    )
}

# The registry against the built server, and the cases against the registry.
# Run before any live work, so a surface change fails in seconds.
function Assert-ObservedPostStateCoverage([string]$ManifestJson) {
    $manifest = $ManifestJson | ConvertFrom-Json
    Assert-True ($null -ne $manifest.names.mutating) "The tool manifest has no names.mutating; the server is older than the observed post-state check."
    $mutating = @($manifest.names.mutating)
    $registered = @($observedRegistry.tools.PSObject.Properties.Name)
    $unaccounted = @($mutating | Where-Object { $registered -notcontains $_ })
    $stale = @($registered | Where-Object { $mutating -notcontains $_ })
    Assert-True ($unaccounted.Count -eq 0) "Mutating tools with neither observed fields nor an exemption in tests/observed_post_state.json: $($unaccounted -join ', ')."
    Assert-True ($stale.Count -eq 0) "tests/observed_post_state.json names tools the server does not implement as mutations: $($stale -join ', ')."
    $cased = @(Get-ObservedPostStateCases | ForEach-Object { $_.Tool } | Sort-Object -Unique)
    $uncased = @($observedFields.Keys | Where-Object { $cased -notcontains $_ })
    $caseForExempt = @($cased | Where-Object { -not $observedFields.ContainsKey($_) })
    Assert-True ($uncased.Count -eq 0) "Tools whose observed fields no case compares with the engine: $($uncased -join ', ')."
    Assert-True ($caseForExempt.Count -eq 0) "Cases for tools the registry does not check: $($caseForExempt -join ', ')."
}

function Invoke-ObservedPostStateBatch([string]$SessionId, [object[]]$Steps, [string[]]$Arguments) {
    $requests = @(
        (@{ jsonrpc = "2.0"; id = 7900; method = "initialize"; params = @{ protocolVersion = "2024-11-05" } } | ConvertTo-Json -Compress),
        (Tool-Request 7901 "runtime_attach_session" @{ session_id = $SessionId })
    )
    for ($index = 0; $index -lt $Steps.Count; $index++) {
        $requests += Tool-Request (8000 + $index) $Steps[$index].Tool $Steps[$index].Arguments
    }
    $byId = @{}
    foreach ($response in @(Invoke-Didi -Requests $requests -Arguments $Arguments | Where-Object { $_ -like "{*" } | ForEach-Object { $_ | ConvertFrom-Json } | Where-Object { $_.PSObject.Properties.Name -contains "id" })) {
        $byId[[int]$response.id] = $response
    }
    Assert-True ($null -ne (Get-ObservedPayload $byId[7901])) "The observed post-state batch could not attach to session ${SessionId}: $($byId[7901] | ConvertTo-Json -Compress -Depth 20)"
    return $byId
}

# Drives every case against the editor's observed_post_state.tscn and the
# attached game. The editor batch runs with --yolo because the witness is
# reached through scene_call_method, which is always confirmed otherwise.
function Invoke-ObservedPostStateCases([string]$FixtureRoot, [string]$EditorSessionId, [string]$GameSessionId) {
    $cases = @(Get-ObservedPostStateCases)
    # The scene the editor had in front, so it is in front again afterwards and
    # the harness's last scene_close still closes the scene it expects.
    # The same for the game's pause state, which the cases change.
    $front = Invoke-ObservedPostStateBatch $EditorSessionId @(Step "front" "scene_get_hierarchy" @{ max_depth = 1 }) @("--project", $FixtureRoot)
    $frontPath = [string](Get-ObservedPayload $front[8000]).scene_file_path
    $gameFront = Invoke-ObservedPostStateBatch $GameSessionId @(Step "front" "runtime_get_tree" @{ root_path = "/root/RuntimeRoot"; max_depth = 1 }) @("--project", $FixtureRoot)
    $gamePaused = [bool](Get-ObservedPayload $gameFront[8000]).paused

    $placed = @()
    foreach ($session in @("editor", "game")) {
        $steps = @()
        foreach ($case in @($cases | Where-Object { $_.Session -eq $session })) {
            $first = $steps.Count
            $steps += $case.Steps
            $placed += @{ Case = $case; First = $first; Session = $session }
        }
        if ($session -eq "editor") {
            $steps += Step "close" "scene_close" @{ discard_unsaved = $true }
            if ($frontPath -and $frontPath -ne $observedScenePath) { $steps += Step "front" "scene_open" @{ scene_path = $frontPath } }
            $editorById = Invoke-ObservedPostStateBatch $EditorSessionId $steps @("--project", $FixtureRoot, "--yolo")
            Assert-True ($null -ne (Get-ObservedPayload $editorById[8000 + $steps.Count - 1])) "The observed post-state block did not put the editor back as it found it: $($editorById[8000 + $steps.Count - 1] | ConvertTo-Json -Compress -Depth 20)"
        }
        else {
            $steps += Step "front" "runtime_set_paused" @{ paused = $gamePaused }
            $gameById = Invoke-ObservedPostStateBatch $GameSessionId $steps @("--project", $FixtureRoot)
            Assert-True ((Get-ObservedPayload $gameById[8000 + $steps.Count - 1]).paused -eq $gamePaused) "The observed post-state block did not leave the game paused as it found it: $($gameById[8000 + $steps.Count - 1] | ConvertTo-Json -Compress -Depth 20)"
        }
    }

    $disagreements = @()
    foreach ($entry in $placed) {
        $byId = if ($entry.Session -eq "editor") { $editorById } else { $gameById }
        $payloads = @{}
        for ($index = 0; $index -lt $entry.Case.Steps.Count; $index++) {
            $step = $entry.Case.Steps[$index]
            $response = $byId[8000 + $entry.First + $index]
            $payload = Get-ObservedPayload $response
            Assert-True ($null -ne $payload) "$($entry.Case.Tool): step '$($step.Name)' ($($step.Tool)) failed: $($response | ConvertTo-Json -Compress -Depth 20)"
            $payloads[$step.Name] = $payload
        }
        foreach ($field in $observedFields[$entry.Case.Tool]) {
            Assert-True ($null -ne $payloads.call.PSObject.Properties[$field]) "$($entry.Case.Tool) answered without its observed field '$field': $($payloads.call | ConvertTo-Json -Compress -Depth 20)"
        }
        foreach ($pair in @(& $entry.Case.Agree $payloads)) {
            if (-not (Test-ObservedAgreement $pair.Observed $pair.Witnessed)) {
                $disagreements += "$($entry.Case.Tool).$($pair.Field) answered $($pair.Observed | ConvertTo-Json -Compress -Depth 20) but the engine reports $($pair.Witnessed | ConvertTo-Json -Compress -Depth 20)"
            }
        }
    }
    Assert-True ($disagreements.Count -eq 0) "Mutations answered with a state the engine does not hold:`n$($disagreements -join "`n")"
    Write-Output "Observed post-state: $($placed.Count) cases over $(@($placed | ForEach-Object { $_.Case.Tool } | Sort-Object -Unique).Count) tools agreed with the engine."
}

# Presence, over every answer the run recorded.
function Assert-ObservedAnswersRecorded {
    $missing = @()
    foreach ($tool in @($observedFields.Keys | Sort-Object)) {
        $answered = @($observedExchanges | Where-Object {
            $_.Tool -eq $tool -and -not ($null -ne $_.Arguments.PSObject.Properties["dry_run"] -and $_.Arguments.dry_run) -and $null -ne (Get-ObservedPayload $_.Response)
        })
        if ($answered.Count -eq 0) {
            $missing += "$tool never answered successfully"
            continue
        }
        foreach ($exchange in $answered) {
            $payload = Get-ObservedPayload $exchange.Response
            foreach ($field in $observedFields[$tool]) {
                if ($null -eq $payload.PSObject.Properties[$field]) { $missing += "$tool answered request $($exchange.Response.id) without '$field'" }
            }
        }
    }
    Assert-True ($missing.Count -eq 0) "Answers without their observed post-state:`n$($missing -join "`n")"
    Write-Output "Observed post-state: $($observedExchanges.Count) answers from $($observedFields.Count) checked tools carried every observed field."
}
