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
$observedBatchFields = @{}
foreach ($entry in $observedRegistry.tools.PSObject.Properties) {
    if ($null -ne $entry.Value.PSObject.Properties["observed"]) {
        $observedFields[$entry.Name] = @($entry.Value.observed)
    }
    if ($null -ne $entry.Value.PSObject.Properties["batch"]) {
        $observedBatchFields[$entry.Name] = [string]$entry.Value.batch
    }
}
$observedExchanges = New-Object System.Collections.ArrayList
$observedScenePath = "res://observed_post_state.tscn"
# The script the rename case's scene carries, declaring the handler under both
# of the names project_rename_references moves between (#1020).
$observedRenameScript = "extends Node`n`nfunc observed_rename_handler() -> void:`n`tpass`n`nfunc observed_rename_renamed() -> void:`n`tpass`n"
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

# The observed fields an answer leaves out. A tool that also answers a batch
# keeps each write's fields in its own entry of the list the registry names,
# so a batch answer is read entry by entry (Q7).
function Get-ObservedFieldGaps([string]$Tool, $Payload) {
    $gaps = @()
    $batchField = $observedBatchFields[$Tool]
    if ($batchField -and $null -ne $Payload.PSObject.Properties[$batchField]) {
        $items = @($Payload.$batchField)
        if ($items.Count -eq 0) { return @("$batchField (empty)") }
        for ($index = 0; $index -lt $items.Count; $index++) {
            foreach ($field in $observedFields[$Tool]) {
                if ($null -eq $items[$index].PSObject.Properties[$field]) { $gaps += "$batchField[$index].$field" }
            }
        }
        return $gaps
    }
    foreach ($field in $observedFields[$Tool]) {
        if ($null -eq $Payload.PSObject.Properties[$field]) { $gaps += $field }
    }
    return $gaps
}

function Test-ObservedNumber($Value) {
    return $Value -is [int] -or $Value -is [long] -or $Value -is [double] -or $Value -is [decimal] -or $Value -is [single]
}

# Whether what an answer says it observed matches what the witness read. An
# object in the answer has to match on every field it carries; the witness may
# say more. An answer object with no fields carries nothing to match, so it
# agrees only with a witness that has none either (#1097). Numbers agree to float32 precision, because every engine value here
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
        if (@($Observed.PSObject.Properties).Count -eq 0) { return @($Witnessed.PSObject.Properties).Count -eq 0 }
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

# What the engine has to hold after the call whatever the answer says. A case's
# Expect names state the tool owes the engine and does not report.
function Expect([string]$Field, $Expected, $Witnessed) {
    return @{ Field = $Field; Expected = $Expected; Witnessed = $Witnessed }
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
#
# A tool that answered with its request instead of what it read would still
# agree with the witness whenever the engine stores what it was sent. So where
# an input exists that the engine stores differently, a case sends it, and the
# comment above each case names that input or says why there is none (#1022).
# A float that rounds to float32 crosses nothing here: Test-ObservedAgreement
# allows float32 precision on purpose, so no case relies on one. A field with
# no counterpart in the request, such as a uid the engine mints or a file's
# length, cannot be echoed; a case for it guards against a constant instead.
function Get-ObservedPostStateCases([string]$GameSessionId = "", [string]$LaunchLogPath = "") {
    return @(
        # Paths are checked as normalized res:// .tscn paths before the editor
        # sees them, so no spelling reaches it that it would store differently.
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
        # A path into the material Shaded holds (Q7). The colour is sent as
        # #rrggbb and the engine stores a Color, so an answer that echoed the
        # request would be a string where the witness reads an object.
        @{ Tool = "scene_set_property"; Session = "editor"; Steps = @(
            (Step "call" "scene_set_property" @{ target_node = "$observedRoot/Shaded"; property_name = "material_override:shader_parameter/tint"; value = "#336699" }),
            (Witness "witness" "property_path" @("Shaded", "material_override:shader_parameter/tint")))
           Agree = { param($s)
               Agree "value" $s.call.value $s.witness.returned
               Agree "applied" $s.call.applied ([Math]::Abs([double]$s.witness.returned.b - 0.6) -lt 0.002) } },
        # The same through a batch, with a plain property beside it, so each
        # entry of writes is compared with what the engine holds. Not
        # process_priority: the editor_undo case below reads that one to tell
        # which history it undid.
        @{ Tool = "scene_set_property"; Session = "editor"; Steps = @(
            (Step "call" "scene_set_property" @{ writes = @(
                @{ target_node = "$observedRoot/Shaded"; property_name = "material_override:shader_parameter/tint"; value = "#ccddee" },
                @{ target_node = "$observedRoot/Subject"; property_name = "process_physics_priority"; value = 13 }) }),
            (Witness "tint" "property_path" @("Shaded", "material_override:shader_parameter/tint")),
            (Witness "priority" "property" @("Subject", "process_physics_priority")))
           Agree = { param($s)
               Agree "writes[0].value" $s.call.writes[0].value $s.tint.returned
               Agree "writes[1].value" $s.call.writes[1].value $s.priority.returned
               Agree "applied" $s.call.applied (([Math]::Abs([double]$s.tint.returned.r - 0.8) -lt 0.002) -and $s.priority.returned -eq 13) } },
        # Built-ins made of other values, and arrays (Q7 part 2). The witness
        # spells each from the engine's own members, so an answer in any other
        # shape disagrees. The basis is a quarter turn, not the identity, and
        # 8.5 is a point that is not whole.
        @{ Tool = "scene_set_property"; Session = "editor"; Steps = @(
            (Step "sprite" "scene_instantiate_node" @{ node_type = "Sprite2D"; parent_path = $observedRoot; name = "Regioned" }),
            (Step "call" "scene_set_property" @{ target_node = "$observedRoot/Regioned"; property_name = "region_rect"; value = @{ position = @{ x = 1; y = 2 }; size = @{ x = 30; y = 40 } } }),
            (Witness "witness" "property" @("Regioned", "region_rect")))
           Agree = { param($s)
               Agree "value" $s.call.value $s.witness.returned
               Agree "applied" $s.call.applied ([double]$s.witness.returned.size.y -eq 40) } },
        @{ Tool = "scene_set_property"; Session = "editor"; Steps = @(
            (Step "turned" "scene_instantiate_node" @{ node_type = "Node3D"; parent_path = $observedRoot; name = "Turned" }),
            (Step "call" "scene_set_property" @{ target_node = "$observedRoot/Turned"; property_name = "transform"; value = @{ basis = @{ x = @{ x = 0; y = 0; z = -1 }; y = @{ x = 0; y = 1; z = 0 }; z = @{ x = 1; y = 0; z = 0 } }; origin = @{ x = 1; y = 2; z = 3 } } }),
            (Witness "witness" "property" @("Turned", "transform")))
           Agree = { param($s)
               Agree "value" $s.call.value $s.witness.returned
               Agree "applied" $s.call.applied ([double]$s.witness.returned.basis.x.z -eq -1) } },
        @{ Tool = "scene_set_property"; Session = "editor"; Steps = @(
            (Step "stroke" "scene_instantiate_node" @{ node_type = "Line2D"; parent_path = $observedRoot; name = "Stroke" }),
            (Step "call" "scene_set_property" @{ target_node = "$observedRoot/Stroke"; property_name = "points"; value = @(@{ x = 0; y = 0 }, @{ x = 32; y = 8.5 }) }),
            (Witness "witness" "property" @("Stroke", "points")))
           Agree = { param($s)
               Agree "value" $s.call.value $s.witness.returned
               Agree "applied" $s.call.applied (@($s.witness.returned).Count -eq 2) } },
        # A typed array on a script that is not a tool, which is most of them:
        # the editor holds a placeholder for it. A typed array keeps what it
        # holds when handed an untyped one, with no error, so the witness reads
        # the element type as well as the values.
        @{ Tool = "scene_set_property"; Session = "editor"; Steps = @(
            (Step "script" "script_create" @{ script_path = "res://observed_points.gd"; source_text = "extends Node`n`n@export var points: Array[Vector2] = []`n"; overwrite = $true }),
            (Step "pointed" "scene_instantiate_node" @{ node_type = "Node"; parent_path = $observedRoot; name = "Pointed" }),
            (Step "attach" "script_attach_to_node" @{ target_node = "$observedRoot/Pointed"; script_path = "res://observed_points.gd" }),
            (Step "call" "scene_set_property" @{ target_node = "$observedRoot/Pointed"; property_name = "points"; value = @(@{ x = 1; y = 1 }, @{ x = 2; y = 3 }) }),
            (Witness "witness" "property" @("Pointed", "points")),
            (Witness "typed" "array_type" @("Pointed", "points")))
           Agree = { param($s)
               Agree "value" $s.call.value $s.witness.returned
               Agree "applied" $s.call.applied (@($s.witness.returned).Count -eq 2) }
           Expect = { param($s) Expect "typed" 5 $s.typed.returned.builtin } },
        # Dictionaries on a script that is not a tool (#1195): an untyped one
        # takes scalars, a typed one is built with the key and value types it
        # holds, and an int key travels as the digits of a string.
        @{ Tool = "scene_set_property"; Session = "editor"; Steps = @(
            (Step "script" "script_create" @{ script_path = "res://observed_tables.gd"; source_text = "extends Node`n`n@export var tags: Dictionary = {}`n@export var scores: Dictionary[String, int] = {}`n@export var waypoints: Dictionary[int, Vector2] = {}`n@export var table: Dictionary = {1: `"a`", 2: `"b`"}`n"; overwrite = $true }),
            (Step "tabled" "scene_instantiate_node" @{ node_type = "Node"; parent_path = $observedRoot; name = "Tabled" }),
            (Step "attach" "script_attach_to_node" @{ target_node = "$observedRoot/Tabled"; script_path = "res://observed_tables.gd" }),
            (Step "call" "scene_set_property" @{ target_node = "$observedRoot/Tabled"; property_name = "tags"; value = @{ speed = 3; label = "fast"; on = $true } }),
            (Witness "witness" "property" @("Tabled", "tags")))
           Agree = { param($s)
               Agree "value" $s.call.value $s.witness.returned
               Agree "applied" $s.call.applied ($s.witness.returned.label -eq "fast") } },
        @{ Tool = "scene_set_property"; Session = "editor"; Steps = @(
            (Step "call" "scene_set_property" @{ target_node = "$observedRoot/Tabled"; property_name = "scores"; value = @{ ada = 3; bob = 5 } }),
            (Witness "witness" "property" @("Tabled", "scores")),
            (Witness "typed" "dictionary_type" @("Tabled", "scores")))
           Agree = { param($s)
               Agree "value" $s.call.value $s.witness.returned
               Agree "applied" $s.call.applied ($s.witness.returned.bob -eq 5) }
           Expect = { param($s)
               Expect "key type" 4 $s.typed.returned.key_builtin
               Expect "value type" 2 $s.typed.returned.value_builtin } },
        @{ Tool = "scene_set_property"; Session = "editor"; Steps = @(
            (Step "call" "scene_set_property" @{ target_node = "$observedRoot/Tabled"; property_name = "waypoints"; value = @{ "1" = @{ x = 4; y = 8 }; "-2" = @{ x = 0.5; y = 0 } } }),
            (Witness "witness" "property" @("Tabled", "waypoints")),
            (Witness "typed" "dictionary_type" @("Tabled", "waypoints")))
           Agree = { param($s)
               Agree "value" $s.call.value $s.witness.returned
               Agree "applied" $s.call.applied ($s.witness.returned."1".y -eq 8) }
           Expect = { param($s)
               Expect "key type" 2 $s.typed.returned.key_builtin
               Expect "value type" 5 $s.typed.returned.value_builtin } },
        # An untyped dictionary reads its int keys as digits, and a read sent
        # back keeps them ints, so table[1] still finds its value (#1249).
        @{ Tool = "scene_set_property"; Session = "editor"; Steps = @(
            (Step "call" "scene_set_property" @{ target_node = "$observedRoot/Tabled"; property_name = "table"; value = @{ "1" = "a"; "2" = "z" } }),
            (Witness "witness" "property" @("Tabled", "table")),
            (Witness "keys" "dictionary_key_types" @("Tabled", "table")))
           Agree = { param($s)
               Agree "value" $s.call.value $s.witness.returned
               Agree "applied" $s.call.applied ($s.witness.returned."2" -eq "z") }
           Expect = { param($s) Expect "key types" "2,2" (@($s.keys.returned) -join ",") } },
        @{ Tool = "scene_instantiate_node"; Session = "editor"; Steps = @(
            (Witness "before" "children" @(".")),
            (Step "call" "scene_instantiate_node" @{ node_type = "Node3D"; parent_path = $observedRoot; name = "Spawned" }),
            (Witness "after" "children" @(".")))
           Agree = { param($s) Agree "node_path" $s.call.node_path (Get-ObservedNewChild $s.before.returned $s.after.returned) } },
        # The same name again, which the engine makes unique: Spawned2.
        @{ Tool = "scene_instantiate_node"; Session = "editor"; Steps = @(
            (Witness "before" "children" @(".")),
            (Step "call" "scene_instantiate_node" @{ node_type = "Node3D"; parent_path = $observedRoot; name = "Spawned" }),
            (Witness "after" "children" @(".")))
           Agree = { param($s) Agree "node_path" $s.call.node_path (Get-ObservedNewChild $s.before.returned $s.after.returned) } },
        # The request names no copy, so the engine always picks the name. The
        # second copy of one node cannot have the first copy's name.
        @{ Tool = "scene_duplicate_node"; Session = "editor"; Steps = @(
            (Witness "before" "children" @(".")),
            (Step "call" "scene_duplicate_node" @{ target_node = "$observedRoot/Spawned" }),
            (Witness "after" "children" @(".")))
           Agree = { param($s) Agree "duplicated_node" $s.call.duplicated_node (Get-ObservedNewChild $s.before.returned $s.after.returned) } },
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
        # Into a parent that already has a child of the same name, so the
        # engine renames the node it moves. The name it picks is a readable
        # one, Spawned3, as in the editor's own reparent, not an internal
        # @Node3D@N, and the answer says it was substituted (#1126).
        @{ Tool = "scene_reparent_node"; Session = "editor"; Steps = @(
            (Step "nest" "scene_instantiate_node" @{ node_type = "Node3D"; parent_path = $observedRoot; name = "Nest" }),
            (Step "namesake" "scene_instantiate_node" @{ node_type = "Node3D"; parent_path = "$observedRoot/Nest"; name = "Spawned2" }),
            (Witness "before" "children" @("Nest")),
            (Step "call" "scene_reparent_node" @{ target_node = "$observedRoot/Spawned2"; new_parent_path = "$observedRoot/Nest" }),
            (Witness "after" "children" @("Nest")))
           Agree = { param($s)
               $moved = Get-ObservedNewChild $s.before.returned $s.after.returned
               $name = ([string]$moved).Substring(([string]$moved).LastIndexOf("/") + 1)
               Agree "node_path" $s.call.node_path $moved
               Agree "node_name" $s.call.node_name $name
               Agree "name_substituted" ($true -eq $s.call.name_substituted) ($name -ne "Spawned2")
               Agree "node_name, readable" "a readable name" $(if ($name.StartsWith("@")) { $name } else { "a readable name" }) } },
        # A removal that succeeded cannot leave the node, and one that cannot
        # is refused, so no input makes exists differ from false.
        @{ Tool = "scene_remove_node"; Session = "editor"; Steps = @(
            (Step "call" "scene_remove_node" @{ target_node = "$observedRoot/Subject/SpawnedCopy" }),
            (Witness "witness" "node_info" @("Subject/SpawnedCopy")))
           Agree = { param($s) Agree "exists" $s.call.exists $s.witness.returned.exists } },
        # script_path has to be a normalized res:// path ending in .gd or .cs,
        # so a uid or a loose spelling is refused before the engine sees it.
        @{ Tool = "script_attach_to_node"; Session = "editor"; Steps = @(
            (Step "call" "script_attach_to_node" @{ target_node = "$observedRoot/Spawned"; script_path = "res://subject.gd" }),
            (Witness "witness" "node_info" @("Spawned")))
           Agree = { param($s) Agree "script_path" $s.call.script_path $s.witness.returned.script_path } },
        # The request names no script; the answer is the empty path read back.
        @{ Tool = "script_detach_from_node"; Session = "editor"; Steps = @(
            (Step "call" "script_detach_from_node" @{ target_node = "$observedRoot/Spawned" }),
            (Witness "witness" "node_info" @("Spawned")))
           Agree = { param($s) Agree "script_path" $s.call.script_path $s.witness.returned.script_path } },
        # connected and disconnected have no counterpart in the request, and
        # the flags the engine adds are not among the observed fields.
        @{ Tool = "signal_connect"; Session = "editor"; Steps = @(
            (Step "call" "signal_connect" @{ emitter_node = "$observedRoot/Subject"; signal_name = "visibility_changed"; target_node = "$observedRoot/Spawned"; target_method = "notify_property_list_changed" }),
            (Witness "witness" "connection_flags" @("Subject", "visibility_changed", "Spawned", "notify_property_list_changed")))
           Agree = { param($s) Agree "connected" $s.call.connected ($s.witness.returned -ge 0) } },
        @{ Tool = "signal_disconnect"; Session = "editor"; Steps = @(
            (Step "call" "signal_disconnect" @{ emitter_node = "$observedRoot/Subject"; signal_name = "visibility_changed"; target_node = "$observedRoot/Spawned"; target_method = "notify_property_list_changed" }),
            (Witness "witness" "connection_flags" @("Subject", "visibility_changed", "Spawned", "notify_property_list_changed")))
           Agree = { param($s) Agree "disconnected" $s.call.disconnected ($s.witness.returned -eq -1) } },
        # The material keeps the Variant it is given, and an int sent to a
        # float uniform reads back as the same number, so nothing crosses.
        @{ Tool = "shader_set_uniform"; Session = "editor"; Steps = @(
            (Step "call" "shader_set_uniform" @{ target_node = "$observedRoot/Shaded"; property_name = "material_override"; uniform_name = "strength"; value = 0.25 }),
            (Witness "witness" "shader_parameter" @("Shaded", "material_override", "strength")))
           Agree = { param($s)
               Agree "value" $s.call.value $s.witness.returned
               Agree "applied" $s.call.applied ($s.witness.returned -eq 0.25) } },
        # A Node3D keeps its Euler angles as given, past a full turn too, and
        # fov outside 1 to 179 is refused by the schema.
        @{ Tool = "viewport_set_camera_transform"; Session = "editor"; Steps = @(
            (Step "call" "viewport_set_camera_transform" @{ camera_path = "$observedRoot/Camera"; position = @{ x = 1.25; y = -2.5; z = 3.75 }; rotation_degrees = @{ x = 10; y = 20; z = 30 }; fov = 73 }),
            (Witness "witness" "camera" @("Camera")))
           Agree = { param($s) Agree "new" $s.call.new $s.witness.returned } },
        # The editor's tree takes both hints as sent.
        @{ Tool = "viewport_toggle_debug_draw"; Session = "editor"; Steps = @(
            (Witness "before" "debug_hints" @()),
            (Step "call" "viewport_toggle_debug_draw" @{ collision_shapes = $true; navigation_mesh = $true }),
            (Witness "witness" "debug_hints" @()),
            # Put back what the editor had, so a game run later draws as before.
            (Step "restore" "viewport_toggle_debug_draw" @{ collision_shapes = $false; navigation_mesh = $false }))
           Agree = { param($s) Agree "observed" $s.call.observed $s.witness.returned } },
        # history has no counterpart in the request.
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
        # A second library on the same player, from its own file because Godot
        # refuses one library twice. The answer lists every library the player
        # holds, which the request names only one of.
        @{ Tool = "anim_add_library"; Session = "editor"; Steps = @(
            (Step "library" "resource_create" @{ save_path = "res://observed_library_again.tres"; resource_type = "AnimationLibrary"; properties = @{ _data = @{ walk = @{ type = "ExtResource"; path = "res://observed_anim.tres" } } } }),
            (Step "call" "anim_add_library" @{ animation_player_path = "$observedRoot/Player"; library_path = "res://observed_library_again.tres"; library_name = "observed_again" }),
            (Witness "witness" "animation_libraries" @("Player")))
           Agree = { param($s) Agree "library_names" @($s.call.library_names | Sort-Object) $s.witness.returned.library_names } },
        # The project writers, each answered with project.godot read back after
        # the save (#1019). The witness reads the file with ConfigFile, which
        # shares nothing with Didi's reader, and each pair leaves the file as
        # it found it.
        @{ Tool = "project_set_setting"; Session = "editor"; Steps = @(
            (Step "call" "project_set_setting" @{ setting = "didi_observed/answer"; value = 42; create = $true }),
            (Witness "witness" "project_setting_text" @("didi_observed/answer")))
           Agree = { param($s) Agree "value_written" $s.call.value_written $s.witness.returned } },
        @{ Tool = "project_set_setting"; Session = "editor"; Steps = @(
            (Step "call" "project_set_setting" @{ setting = "didi_observed/answer"; remove = $true }),
            (Witness "witness" "project_setting_text" @("didi_observed/answer")))
           Agree = { param($s) Agree "value_written" $s.call.value_written $s.witness.returned } },
        # A built-in set to its default, which Godot does not write: the file
        # has no line for it while the request carried a value.
        @{ Tool = "project_set_setting"; Session = "editor"; Steps = @(
            (Step "call" "project_set_setting" @{ setting = "application/run/max_fps"; value = 0 }),
            (Witness "witness" "project_setting_text" @("application/run/max_fps")),
            (Step "restore" "project_set_setting" @{ setting = "application/run/max_fps"; remove = $true }))
           Agree = { param($s) Agree "value_written" $s.call.value_written $s.witness.returned } },
        @{ Tool = "project_set_autoload"; Session = "editor"; Steps = @(
            (Step "call" "project_set_autoload" @{ name = "ObservedLoad"; path = "res://subject.gd"; singleton = $false }),
            (Witness "witness" "autoload_entry" @("ObservedLoad")))
           Agree = { param($s) Agree "autoload" $s.call.autoload $s.witness.returned } },
        # A singleton, which the file keeps as a * before the path. An answer
        # that repeated the request would still agree, since the witness reads
        # the * back as singleton: true; this crosses the reader instead.
        @{ Tool = "project_set_autoload"; Session = "editor"; Steps = @(
            (Step "call" "project_set_autoload" @{ name = "ObservedSingle"; path = "res://subject.gd"; singleton = $true }),
            (Witness "witness" "autoload_entry" @("ObservedSingle")),
            (Step "restore" "project_remove_autoload" @{ name = "ObservedSingle" }))
           Agree = { param($s) Agree "autoload" $s.call.autoload $s.witness.returned } },
        # The removals and the input action writers answer with what the file
        # declares afterwards, which no request field names.
        @{ Tool = "project_remove_autoload"; Session = "editor"; Steps = @(
            (Step "call" "project_remove_autoload" @{ name = "ObservedLoad" }),
            (Witness "witness" "autoload_entry" @("ObservedLoad")))
           Agree = { param($s) Agree "autoload" $s.call.autoload $s.witness.returned } },
        @{ Tool = "project_set_input_action"; Session = "editor"; Steps = @(
            (Step "call" "project_set_input_action" @{ action = "observed_action"; deadzone = 0.25; events = @(@{ type = "key"; keycode = 70 }) }),
            (Witness "witness" "input_action_declared" @("observed_action")))
           Agree = { param($s) Agree "defined_by_project" $s.call.defined_by_project $s.witness.returned } },
        @{ Tool = "project_remove_input_action"; Session = "editor"; Steps = @(
            (Step "call" "project_remove_input_action" @{ action = "observed_action" }),
            (Witness "witness" "input_action_declared" @("observed_action")))
           Agree = { param($s) Agree "defined_by_project" $s.call.defined_by_project $s.witness.returned } },
        # The file read back after the write, not the request (#1019). The
        # witness loads it past the editor's cache and reports the class the
        # engine made from it, or the class_name of the script it carries.
        @{ Tool = "resource_create"; Session = "editor"; Steps = @(
            (Step "call" "resource_create" @{ save_path = "res://observed_shape.tres"; resource_type = "CircleShape2D"; properties = @{ radius = 3 }; overwrite = $true }),
            (Witness "witness" "load_fresh" @("res://observed_shape.tres", @("radius"))),
            (Witness "length" "file_length" @("res://observed_shape.tres")),
            (Witness "indexed" "indexed" @("res://observed_shape.tres")))
           Agree = { param($s)
               Agree "resource_type" $s.call.resource_type $s.witness.returned.class
               Agree "file_bytes" $s.call.file_bytes $s.length.returned }
           Expect = { param($s) Expect "indexed" $true $s.indexed.returned } },
        # A class_name type, which the header cannot name: the engine makes a
        # Resource carrying the script, and named in the header it loaded as a
        # MissingResource while the answer named the class (#1125).
        @{ Tool = "resource_create"; Session = "editor"; Steps = @(
            (Step "script" "script_create" @{ script_path = "res://observed_item.gd"; source_text = "class_name ObservedItem`nextends Resource`n`n@export var power := 1`n" }),
            (Step "call" "resource_create" @{ save_path = "res://observed_item.tres"; resource_type = "ObservedItem"; properties = @{ power = 5; "metadata/held" = @{ type = "SubResource"; id = "held" } }; sub_resources = @(@{ id = "held"; resource_type = "ObservedItem"; properties = @{ power = 3 } }) }),
            (Witness "witness" "load_fresh" @("res://observed_item.tres", @("power", "metadata/held:power", "metadata/held:script"))),
            (Witness "length" "file_length" @("res://observed_item.tres")),
            (Witness "indexed" "indexed" @("res://observed_item.tres")))
           Agree = { param($s)
               Agree "resource_type" $s.call.resource_type $(if ($s.witness.returned.script_class) { $s.witness.returned.script_class } else { $s.witness.returned.class })
               Agree "engine_type" $s.call.engine_type $s.witness.returned.class
               Agree "file_bytes" $s.call.file_bytes $s.length.returned
               Agree "properties.power" 5 $s.witness.returned.properties.power
               Agree "sub_resources.held.power" 3 $s.witness.returned.properties."metadata/held:power"
               Agree "sub_resources.held.script" "res://observed_item.gd" $s.witness.returned.properties."metadata/held:script" }
           Expect = { param($s) Expect "indexed" $true $s.indexed.returned } },
        # uid and uid_registered are the engine's; the request names neither.
        @{ Tool = "scene_pack_branch"; Session = "editor"; Steps = @(
            (Step "call" "scene_pack_branch" @{ target_node = "$observedRoot/Subject"; scene_path = "res://observed_packed.tscn" }),
            (Witness "witness" "load_fresh" @("res://observed_packed.tscn", @())))
           Agree = { param($s)
               Agree "uid" $s.call.uid $s.witness.returned.uid
               Agree "uid_registered" $s.call.uid_registered $s.witness.returned.uid_registered }
           Expect = { param($s) Expect "uid_registered" $true $s.witness.returned.uid_registered } },
        # The script is in the editor's index, its uid registered, when the call
        # returns. Left for the editor's next scan, which runs whenever its window
        # takes focus, a script whose .uid sidecar project_export's Godot wrote in
        # the meantime made 4.5 and 4.6 print `Unrecognized UID` for it.
        @{ Tool = "script_create"; Session = "editor"; Steps = @(
            (Step "call" "script_create" @{ script_path = "res://observed_created.gd"; source_text = "extends Node`n`n`nfunc answer() -> int:`n`treturn 1`n" }),
            (Witness "witness" "load_fresh" @("res://observed_created.gd", @())))
           Agree = { param($s) Agree "has_errors" $s.call.has_errors (@($s.witness.returned.methods) -notcontains "answer") }
           Expect = { param($s) Expect "uid_registered" $true $s.witness.returned.uid_registered } },
        # A script that does not compile, so has_errors cannot be a constant.
        @{ Tool = "script_create"; Session = "editor"; Steps = @(
            (Step "call" "script_create" @{ script_path = "res://observed_broken.gd"; source_text = "extends Node`n`n`nfunc answer() -> int:`n`treturn undeclared_name`n" }),
            (Witness "witness" "load_fresh" @("res://observed_broken.gd", @())))
           Agree = { param($s) Agree "has_errors" $s.call.has_errors (@($s.witness.returned.methods) -notcontains "answer") }
           Expect = { param($s) Expect "uid_registered" $true $s.witness.returned.uid_registered } },
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
        # An export path spelled loosely, which the file keeps normalized.
        @{ Tool = "project_add_export_preset"; Session = "editor"; Steps = @(
            (Step "call" "project_add_export_preset" @{ name = "Observed Loose Path"; platform = "Linux"; export_path = "./builds//observed.x86_64" }),
            (Witness "witness" "export_preset" @("Observed Loose Path")))
           Agree = { param($s)
               Agree "preset" $s.call.preset $s.witness.returned.preset
               Agree "preset_count" $s.call.preset_count $s.witness.returned.preset_count } },
        # The lab, written behind the editor, read back by its length; no input
        # gives it a length, so the case guards against a constant. Its target
        # is the shape the first resource_create case wrote (#1164). The lab's
        # path is fixed and an earlier scan in the run has indexed it, so the
        # index check holds it there without isolating the indexing call; the
        # resource_create cases do that.
        @{ Tool = "viewport_create_test_lab"; Session = "editor"; Steps = @(
            (Step "call" "viewport_create_test_lab" @{ target_resource_path = "res://observed_shape.tres"; overwrite = $true }),
            (Witness "length" "file_length" @("res://didi_test_lab.tscn")),
            (Witness "indexed" "indexed" @("res://didi_test_lab.tscn")))
           Agree = { param($s) Agree "file_bytes" $s.call.file_bytes $s.length.returned }
           Expect = { param($s) Expect "indexed" $true $s.indexed.returned } },
        # The two exporters each start a Godot of their own and answer with
        # what they read back from the file it wrote (#1020). Neither field has
        # a counterpart in the request, so the cases guard against a constant.
        # The pack uses the preset the case above added, and needs no export
        # templates.
        @{ Tool = "project_export"; Session = "editor"; Steps = @(
            (Step "call" "project_export" @{ preset = "Observed Preset"; output_path = "res://observed_export.pck"; mode = "pack"; timeout_seconds = 120 }),
            (Witness "witness" "file_length" @("res://observed_export.pck")))
           Agree = { param($s) Agree "size_bytes" $s.call.size_bytes $s.witness.returned } },
        @{ Tool = "gridmap_export_mesh_library"; Session = "editor"; Steps = @(
            (Step "call" "gridmap_export_mesh_library" @{ source_scene = "res://phase5_mesh_source.tscn"; output_path = "res://observed_items.meshlib"; generate_collisions = $false; overwrite = $true; timeout_seconds = 60 }),
            (Witness "witness" "load_fresh" @("res://observed_items.meshlib", @())),
            (Witness "indexed" "indexed" @("res://observed_items.meshlib")))
           Agree = { param($s) Agree "item_count" $s.call.item_count $s.witness.returned.item_count }
           Expect = { param($s) Expect "indexed" $true $s.indexed.returned } },
        # A name already taken is refused rather than renamed, though
        # AudioServer would call the new bus "Observed 2", so no input crosses.
        @{ Tool = "audio_add_bus"; Session = "editor"; Steps = @(
            (Step "call" "audio_add_bus" @{ name = "Observed" }),
            (Witness "witness" "audio_bus" @("Observed")))
           Agree = { param($s)
               Agree "after" $s.call.after $s.witness.returned.after
               Agree "bus_count" $s.call.bus_count $s.witness.returned.bus_count } },
        # AudioServer keeps any volume inside the schema's -80 to 24 as sent.
        @{ Tool = "audio_configure_bus"; Session = "editor"; Steps = @(
            (Step "call" "audio_configure_bus" @{ bus = "Observed"; volume_db = -6.5; mute = $true }),
            (Witness "witness" "audio_bus" @("Observed")))
           Agree = { param($s) Agree "after" $s.call.after $s.witness.returned.after } },
        # The OGG the loop block wrote and imported earlier in the run; Needs
        # says so when it is missing. The importer keeps loop and loop_offset as
        # sent, the offset to float32.
        @{ Tool = "asset_configure_import"; Session = "editor"
           Needs = @{ File = "loop_track.ogg.import"; From = "the loop block (#958) in run_godot_integration.ps1, which writes and imports res://loop_track.ogg" }
           Steps = @(
            (Step "call" "asset_configure_import" @{ asset_path = "res://loop_track.ogg"; options = @{ loop = $false; loop_offset = 0.2 } }),
            (Witness "witness" "load_fresh" @("res://loop_track.ogg", @("loop", "loop_offset"))))
           Agree = { param($s)
               Agree "stream.class" $s.call.stream.class $s.witness.returned.class
               Agree "stream.properties" $s.call.stream.properties $s.witness.returned.properties } },
        # Last among the editor cases: creating a scene makes it the edited one,
        # and the witness lives in observed_post_state.tscn, so the case opens
        # that again before reading. uid and uid_registered are the engine's.
        @{ Tool = "scene_create"; Session = "editor"; Steps = @(
            (Step "call" "scene_create" @{ scene_path = "res://observed_created.tscn"; root_type = "Node2D"; root_name = "Created" }),
            (Step "return" "scene_open" @{ scene_path = $observedScenePath }),
            (Witness "witness" "load_fresh" @("res://observed_created.tscn", @())))
           Agree = { param($s)
               Agree "uid" $s.call.uid $s.witness.returned.uid
               Agree "uid_registered" $s.call.uid_registered $s.witness.returned.uid_registered }
           Expect = { param($s) Expect "uid_registered" $true $s.witness.returned.uid_registered } },
        # The tabs read after the close, not a constant (#1019). The scene the
        # case above created is still open in a tab; it is brought to the front,
        # closed, and the observed scene brought back for the witness. No input
        # makes a close that succeeded leave its tab open, so an answer that
        # always said still_open: false would pass; only a source read of the
        # tool rules that out.
        @{ Tool = "scene_close"; Session = "editor"; Steps = @(
            (Step "open" "scene_open" @{ scene_path = "res://observed_created.tscn" }),
            (Step "call" "scene_close" @{ discard_unsaved = $true }),
            (Step "return" "scene_open" @{ scene_path = $observedScenePath }),
            (Witness "witness" "scene_open" @("res://observed_created.tscn")))
           Agree = { param($s) Agree "still_open" $s.call.still_open $s.witness.returned } },
        # The rename's own count of the lines it changed, against the scene
        # file's [connection] lines, and none left naming the old method
        # (#1020). The scene is made, saved and closed first, so no tab holds
        # it, and its script declares the method under both names, so it loads
        # before and after the rename.
        @{ Tool = "project_rename_references"; Session = "editor"; Steps = @(
            (Step "script" "script_create" @{ script_path = "res://observed_rename.gd"; source_text = $observedRenameScript; overwrite = $true }),
            (Step "scene" "scene_create" @{ scene_path = "res://observed_rename.tscn"; root_type = "Node"; root_name = "ObservedRename"; overwrite = $true }),
            (Step "attach" "script_attach_to_node" @{ target_node = "/root/ObservedRename"; script_path = "res://observed_rename.gd" }),
            (Step "ready" "signal_connect" @{ emitter_node = "/root/ObservedRename"; signal_name = "ready"; target_node = "/root/ObservedRename"; target_method = "observed_rename_handler" }),
            (Step "entered" "signal_connect" @{ emitter_node = "/root/ObservedRename"; signal_name = "tree_entered"; target_node = "/root/ObservedRename"; target_method = "observed_rename_handler" }),
            (Step "save" "editor_save_scene" @{}),
            (Step "close" "scene_close" @{ discard_unsaved = $true }),
            (Step "return" "scene_open" @{ scene_path = $observedScenePath }),
            (Step "call" "project_rename_references" @{ target = "observed_rename_handler"; new_name = "observed_rename_renamed" }),
            (Witness "witness" "connection_lines" @("res://observed_rename.tscn", "observed_rename_renamed")),
            (Witness "old" "connection_lines" @("res://observed_rename.tscn", "observed_rename_handler")))
           Agree = { param($s) Agree "updated_files" $s.call.updated_files @($s.witness.returned) }
           Expect = { param($s) Expect "old connections" 0 $s.old.returned.changed_lines } },
        # Membership read back after the commit, not the constant the answer
        # used to carry (#1019). in_group has no counterpart in the request.
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
        # a task through its life, then the clear that leaves it empty. The
        # board is JSON that Didi writes and reads, so it keeps every value as
        # sent, and the revisions and task records are Didi's own.
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
        # The cells each writer read back after its commit (#1019). The one
        # input either grid would store differently, an erase that names an
        # atlas tile or an orientation, is refused before the engine sees it.
        @{ Tool = "tilemap_set_cells"; Session = "editor"; Steps = @(
            (Step "call" "tilemap_set_cells" @{ tilemap_path = "$observedRoot/Tiles"; cells = @(@{ coords = @(2, 3); source_id = 0; atlas_coords = @(0, 0) }) }),
            (Witness "witness" "tilemap_cell" @("Tiles", 2, 3)))
           # Field by field: an answer object may say less than the witness,
           # and a cell that says nothing would agree with anything.
           Agree = { param($s)
               $cell = @($s.call.cells)[0]
               Agree "cells.source_id" $cell.source_id $s.witness.returned.source_id
               Agree "cells.atlas_coords" $cell.atlas_coords $s.witness.returned.atlas_coords
               Agree "cells.alternative_tile" $cell.alternative_tile $s.witness.returned.alternative_tile } },
        @{ Tool = "gridmap_set_cells"; Session = "editor"; Steps = @(
            (Step "call" "gridmap_set_cells" @{ gridmap_path = "$observedRoot/Grid"; cells = @(@{ position = @(1, 0, 2); item = 0; orientation = 10 }) }),
            (Witness "witness" "gridmap_cell" @("Grid", 1, 0, 2)))
           Agree = { param($s)
               $cell = @($s.call.cells)[0]
               Agree "cells.item" $cell.item $s.witness.returned.item
               Agree "cells.orientation" $cell.orientation $s.witness.returned.orientation } },
        # announced is read off the disk after the editor's import pass: the
        # paths that have no .import sidecar then. A script never gets one and
        # the imported SVG keeps its own, so an answer copied from the request,
        # or a constant, disagrees. imported is not cased: whether a path lands
        # there depends on whether the editor had indexed it before the call.
        @{ Tool = "asset_reimport"; Session = "editor"; Steps = @(
            (Step "call" "asset_reimport" @{ paths = @("res://subject.gd", "res://reimport_probe.svg"); timeout_ms = 10000 }),
            (Witness "witness" "without_import_sidecar" @(,@("res://subject.gd", "res://reimport_probe.svg"))))
           Agree = { param($s) Agree "announced" @($s.call.announced) @($s.witness.returned) } },
        # The save reads the file it wrote, since save_scene answers OK whether
        # or not the editor wrote it (#1019). Last of the editor cases, so the
        # observed scene goes to disk with every edit above. file_bytes has no
        # counterpart in the request.
        @{ Tool = "editor_save_scene"; Session = "editor"; Steps = @(
            (Step "call" "editor_save_scene" @{}),
            (Witness "witness" "file_length" @($observedScenePath)))
           Agree = { param($s) Agree "file_bytes" $s.call.file_bytes $s.witness.returned } },
        # The game's own tree is the witness. A pause the tree takes as sent;
        # runtime_step leaves it paused whatever it was asked.
        @{ Tool = "runtime_set_paused"; Session = "game"; Steps = @(
            (Step "call" "runtime_set_paused" @{ paused = $false }),
            (Step "witness" "runtime_get_tree" @{ root_path = "/root/RuntimeRoot"; max_depth = 1 }))
           Agree = { param($s) Agree "paused" $s.call.paused $s.witness.paused } },
        # Slow on purpose: the probe animation is a second long, and a software
        # rendered runner can take that long to reach the read. playing has no
        # counterpart in the request; a missing animation is refused.
        @{ Tool = "anim_play_track"; Session = "game"; Steps = @(
            (Step "call" "anim_play_track" @{ animation_player_path = "/root/RuntimeRoot/Spatial/Player"; animation_name = "probe"; custom_speed = 0.01 }),
            (Step "witness" "eval_gdscript" @{ expression = "node.get('current_animation')"; context_node = "/root/RuntimeRoot/Spatial/Player" }))
           Agree = { param($s) Agree "playing" $s.call.playing ($s.witness.value -eq "probe") } },
        # parse_input_event only buffers, so the tool flushes and then reads
        # what Input holds (#1019). The fixture's _input reads the same state
        # the way a game would. The release puts ui_accept back. Input keeps an
        # action event's strength as sent, and the schema bounds it to 0 to 1.
        @{ Tool = "runtime_inject_input"; Session = "game"; Steps = @(
            (Step "call" "runtime_inject_input" @{ events = @(@{ type = "action"; action_name = "ui_accept"; pressed = $true; strength = 0.75 }) }),
            (Step "witness" "eval_gdscript" @{ expression = "node.get('position')"; context_node = "/root/RuntimeRoot/Spatial/AnimTarget/InputStateProbe" }),
            (Step "release" "runtime_inject_input" @{ events = @(@{ type = "action"; action_name = "ui_accept"; pressed = $false }) }))
           Agree = { param($s)
               $state = @($s.call.input_state)[0]
               Agree "input_state.pressed" $state.pressed ($s.witness.value.x -eq 1)
               Agree "input_state.strength" $state.strength $s.witness.value.y } },
        # The runtime root's child count does not move while nothing touches
        # the tree, so this run finds a stuck interval and pauses there. A
        # constant false would disagree; the pause is read back from the tree
        # rather than taken from the pause call (#1020).
        @{ Tool = "runtime_explore_scene"; Session = "game"; Steps = @(
            (Step "call" "runtime_explore_scene" @{ duration_ms = 1500; stuck_ms = 300; action_hold_ms = 100; seed = 11; actions = @("ui_accept"); probes = @(@{ name = "children"; expression = "node.get_child_count()"; context_node = "/root/RuntimeRoot" }) }),
            (Step "witness" "runtime_get_tree" @{ root_path = "/root/RuntimeRoot"; max_depth = 1 }))
           Agree = { param($s) Agree "paused" $s.call.paused $s.witness.paused } },
        @{ Tool = "runtime_set_paused"; Session = "game"; Steps = @(
            (Step "call" "runtime_set_paused" @{ paused = $true }),
            (Step "witness" "runtime_get_tree" @{ root_path = "/root/RuntimeRoot"; max_depth = 1 }))
           Agree = { param($s) Agree "paused" $s.call.paused $s.witness.paused } },
        @{ Tool = "runtime_step"; Session = "game"; Steps = @(
            (Step "call" "runtime_step" @{ frames = 2 }),
            (Step "witness" "runtime_get_tree" @{ root_path = "/root/RuntimeRoot"; max_depth = 1 }))
           Agree = { param($s) Agree "paused" $s.call.paused $s.witness.paused } },
        # On a game that is already paused, with a condition that holds, so the
        # watch pauses nothing itself. It answered paused: false here while the
        # tree stayed paused, because it reported its own pause call (#1020).
        @{ Tool = "runtime_watch_invariants"; Session = "game"; Steps = @(
            (Step "call" "runtime_watch_invariants" @{ duration_ms = 200; invariants = @(@{ name = "children_present"; kind = "expression_between"; expression = "node.get_child_count()"; context_node = "/root/RuntimeRoot"; minimum = 1 }) }),
            (Step "witness" "runtime_get_tree" @{ root_path = "/root/RuntimeRoot"; max_depth = 1 }))
           Agree = { param($s) Agree "paused" $s.call.paused $s.witness.paused } },
        # The launched game's own process id is the witness: runtime_main.tscn
        # records OS.get_process_id() at _ready, and the read goes through the
        # session the launch selected. A pid taken from the launcher, which on
        # a console build is not the game (#773), or a launch that left the
        # calls going to the harness's game, disagrees. Last of the game cases:
        # it stops the game it started and attaches the harness's game again.
        @{ Tool = "runtime_launch"; Session = "game"; Steps = @(
            (Step "call" "runtime_launch" @{ scene_path = "res://runtime_main.tscn"; timeout_seconds = 30; headless = $true; detach = $true; extra_args = @("--log-file", $LaunchLogPath) }),
            (Step "witness" "eval_gdscript" @{ expression = "node.get('editor_description')"; context_node = "/root/RuntimeRoot" }),
            (Step "stop" "runtime_stop" @{ exit_code = 0 }),
            (Step "back" "runtime_attach_session" @{ session_id = $GameSessionId }))
           Agree = { param($s) Agree "pid" $s.call.pid ([int64]$s.witness.value) } }
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
function Invoke-ObservedPostStateCases([string]$FixtureRoot, [string]$EditorSessionId, [string]$GameSessionId, [string]$LaunchLogPath) {
    $cases = @(Get-ObservedPostStateCases -GameSessionId $GameSessionId -LaunchLogPath $LaunchLogPath)
    # A case that uses a file another block made names that block, so moving
    # or dropping the block fails here and says why, not on a missing asset
    # several steps in (#1022).
    foreach ($case in @($cases | Where-Object { $_.ContainsKey("Needs") })) {
        Assert-True (Test-Path -LiteralPath (Join-Path $FixtureRoot $case.Needs.File)) "The observed post-state case for $($case.Tool) needs $($case.Needs.File), which $($case.Needs.From). That block did not run, or did not make it."
    }
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
            # The engine-output gate reads the launched game's log, and skips a
            # file that is not there.
            Assert-True (Test-Path -LiteralPath $LaunchLogPath) "The game runtime_launch started wrote no engine log at $LaunchLogPath."
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
        $gaps = @(Get-ObservedFieldGaps $entry.Case.Tool $payloads.call)
        Assert-True ($gaps.Count -eq 0) "$($entry.Case.Tool) answered without its observed field '$($gaps -join "', '")': $($payloads.call | ConvertTo-Json -Compress -Depth 20)"
        foreach ($pair in @(& $entry.Case.Agree $payloads)) {
            if (-not (Test-ObservedAgreement $pair.Observed $pair.Witnessed)) {
                $disagreements += "$($entry.Case.Tool).$($pair.Field) answered $($pair.Observed | ConvertTo-Json -Compress -Depth 20) but the engine reports $($pair.Witnessed | ConvertTo-Json -Compress -Depth 20)"
            }
        }
        if ($entry.Case.ContainsKey("Expect")) {
            foreach ($pair in @(& $entry.Case.Expect $payloads)) {
                if (-not (Test-ObservedAgreement $pair.Expected $pair.Witnessed)) {
                    $disagreements += "After $($entry.Case.Tool) the engine reports $($pair.Field) $($pair.Witnessed | ConvertTo-Json -Compress -Depth 20), not $($pair.Expected | ConvertTo-Json -Compress -Depth 20)"
                }
            }
        }
    }
    Assert-True ($disagreements.Count -eq 0) "Mutations answered with a state the engine does not hold:`n$($disagreements -join "`n")"
    Write-Output "Observed post-state: $($placed.Count) cases over $(@($placed | ForEach-Object { $_.Case.Tool } | Sort-Object -Unique).Count) tools agreed with the engine."
}

# The state each of these tools replaced, read before its commit, which the
# change journal records as the entry's before (#1151). Checked over every
# answer the run recorded, not one case each, and against what the call must
# have found: a group add finds the node outside the group, a connect finds no
# connection, and so on. project_set_setting uses previous_value, its offline
# route's word, and only has one when the setting existed.
function Assert-BeforeReported {
    $expected = @{
        "scene_remove_node" = { param($b) $b.parent -is [string] -and $b.path -is [string] -and $b.index -ge 0 -and $b.type -is [string] }
        "scene_reparent_node" = { param($b) $b.parent -is [string] -and $b.path -is [string] }
        "script_attach_to_node" = { param($b) $b.PSObject.Properties.Name -contains "script" }
        "script_detach_from_node" = { param($b) $b.script -is [string] }
        "scene_add_to_group" = { param($b) $b.in_group -eq $false }
        "scene_remove_from_group" = { param($b) $b.in_group -eq $true -and $b.persistent -is [bool] }
        "signal_connect" = { param($b) $b.connected -eq $false }
        "signal_disconnect" = { param($b) $b.connected -eq $true -and $b.flags -ge 2 }
    }
    $missing = @()
    $checked = 0
    $seen = @{}
    foreach ($exchange in $observedExchanges) {
        if ($null -ne $exchange.Arguments.PSObject.Properties["dry_run"] -and $exchange.Arguments.dry_run) { continue }
        $payload = Get-ObservedPayload $exchange.Response
        if ($null -eq $payload) { continue }
        if ($exchange.Tool -eq "project_set_setting") {
            if ($payload.defined_by_engine -ne $true) { continue }
            $checked++
            $seen[$exchange.Tool] = $true
            if ($payload.PSObject.Properties.Name -notcontains "previous_value") {
                $missing += "project_set_setting answered request $($exchange.Response.id) for a setting that existed without previous_value"
            }
            continue
        }
        if (-not $expected.ContainsKey($exchange.Tool)) { continue }
        $checked++
        $seen[$exchange.Tool] = $true
        if ($payload.PSObject.Properties.Name -notcontains "before" -or -not (& $expected[$exchange.Tool] $payload.before)) {
            $missing += "$($exchange.Tool) answered request $($exchange.Response.id) with before $($payload.before | ConvertTo-Json -Compress -Depth 6)"
        }
    }
    foreach ($tool in @($expected.Keys) + @("project_set_setting")) {
        if (-not $seen.ContainsKey($tool)) { $missing += "$tool answered nothing this run checks" }
    }
    Assert-True ($missing.Count -eq 0) "Answers without the state they replaced ($checked checked):`n$($missing -join "`n")"
    Write-Output "Before values: $checked answers said what they replaced."
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
            foreach ($gap in @(Get-ObservedFieldGaps $tool (Get-ObservedPayload $exchange.Response))) {
                $missing += "$tool answered request $($exchange.Response.id) without '$gap'"
            }
        }
    }
    Assert-True ($missing.Count -eq 0) "Answers without their observed post-state:`n$($missing -join "`n")"
    Write-Output "Observed post-state: $($observedExchanges.Count) answers from $($observedFields.Count) checked tools carried every observed field."
}
