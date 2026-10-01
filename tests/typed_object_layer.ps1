# The typed object layer: Q7 in docs/BUILD_QUEUE.md, principles P6 and P3 in
# docs/DESIGN_PRINCIPLES.md.
#
# scene_get_property and scene_set_property reach a property by path, in the
# grammar Godot's own get_indexed and set_indexed take, and take a batch. This
# block proves on the attached engine line what the native tests cannot:
#
# - A batch of writes on three nodes, through two kinds of sub-resource and a
#   plain property, undoes as one step and redoes as one step.
# - A save keeps what it was said to keep. The edited scene holds its embedded
#   resources, a resource kept in its own file is rewritten in that file and
#   the answer names it, and a resource built into an instanced scene is
#   refused before anything is written, because the save would drop it.
# - A read reports each property's declared type and what the engine declares
#   about its values.
# - A dry run of a batch fails where the batch would, and a batch that fails
#   applies nothing.
#
# run_godot_integration.ps1 dot-sources this file and calls the block once the
# editor session is attached. typed_object_layer.tscn, typed_shared_box.tres and
# typed_child.tscn are its fixture, and nothing else opens them.

function Invoke-TypedObjectLayerBlock {
    param(
        [Parameter(Mandatory = $true)] [object]$EditorSession,
        [Parameter(Mandatory = $true)] [string]$FixtureRoot
    )
    $scene = "res://typed_object_layer.tscn"
    $root = "/root/TypedRoot"
    $box = "theme_override_styles/panel:bg_color"
    $tint = "material_override:shader_parameter/tint"
    $reads = @(
        @{ target_node = "$root/Embedded"; property_name = $box },
        @{ target_node = "$root/Mesh"; property_name = $tint },
        @{ target_node = "$root/Mover"; property_name = "position" }
    )
    # Two sub-resources on two nodes and a property of a third, so the one
    # undo step has to cover writes recorded through set_indexed and through
    # a plain property alike.
    $batch = @(
        @{ target_node = "$root/Embedded"; property_name = $box; value = "#204060" },
        @{ target_node = "$root/Mesh"; property_name = $tint; value = @{ r = 0; g = 1; b = 0 } },
        @{ target_node = "$root/Mover"; property_name = "position"; value = @{ x = 5; y = 6 } }
    )
    $typedRequests = @(
        (@{ jsonrpc = "2.0"; id = 7100; method = "initialize"; params = @{ protocolVersion = "2024-11-05" } } | ConvertTo-Json -Compress),
        (Tool-Request 7101 "runtime_attach_session" @{ session_id = $EditorSession.session_id }),
        (Tool-Request 7102 "scene_open" @{ scene_path = $scene }),
        (Tool-Request 7103 "scene_get_property" @{ reads = @($reads + @(@{ target_node = "$root/Embedded"; property_name = "theme_override_styles/panel" })) }),
        (Tool-Request 7104 "scene_set_property" @{ writes = $batch; dry_run = $true }),
        (Tool-Request 7105 "scene_set_property" @{ writes = $batch }),
        (Tool-Request 7106 "scene_get_property" @{ reads = $reads }),
        (Tool-Request 7107 "editor_undo" @{}),
        (Tool-Request 7108 "scene_get_property" @{ reads = $reads }),
        (Tool-Request 7109 "editor_redo" @{}),
        (Tool-Request 7110 "scene_get_property" @{ reads = $reads }),
        # A resource kept in its own file.
        (Tool-Request 7111 "scene_set_property" @{ target_node = "$root/Shared"; property_name = $box; value = "#00ff00" }),
        # A resource built into the instanced scene, which a save would drop.
        (Tool-Request 7112 "scene_set_property" @{ target_node = "$root/Child"; property_name = $box; value = "#00ff00" }),
        # A step that names nothing on the StyleBox, spelled the way a person
        # would mistype the property it means.
        (Tool-Request 7113 "scene_set_property" @{ target_node = "$root/Embedded"; property_name = "theme_override_styles/panel:bg_colour"; value = "#00ff00" }),
        # A slot and a property inside it, in one batch.
        (Tool-Request 7114 "scene_set_property" @{ writes = @(
            @{ target_node = "$root/Embedded"; property_name = "theme_override_styles/panel"; value = $null },
            @{ target_node = "$root/Embedded"; property_name = $box; value = "#00ff00" }) }),
        # A write the layer refuses by name, dry run and real.
        (Tool-Request 7115 "scene_set_property" @{ target_node = "$root/Mover"; property_name = "owner"; value = $null; dry_run = $true }),
        (Tool-Request 7116 "scene_set_property" @{ target_node = "$root/Mover"; property_name = "owner"; value = $null }),
        # A batch whose second write is refused leaves the first unapplied.
        (Tool-Request 7117 "scene_set_property" @{ writes = @(
            @{ target_node = "$root/Mover"; property_name = "position"; value = @{ x = 9; y = 9 } },
            @{ target_node = "$root/Embedded"; property_name = $box; value = "teal" }) }),
        (Tool-Request 7118 "scene_get_property" @{ target_node = "$root/Mover"; property_name = "position" }),
        # A member of a value is not a step into a resource.
        (Tool-Request 7119 "scene_get_property" @{ target_node = "$root/Mover"; property_name = "position:x" }),
        (Tool-Request 7120 "editor_save_scene" @{}),
        # Saved by the request before, so nothing is discarded; before 4.7 the
        # engine cannot report that, and a close without the flag is refused.
        (Tool-Request 7121 "scene_close" @{ discard_unsaved = $true }),
        (Tool-Request 7122 "scene_open" @{ scene_path = "res://main.tscn" })
    )
    $rawTyped = Invoke-Didi -Requests $typedRequests -Arguments @("--project", $FixtureRoot)
    $typedById = @{}
    foreach ($response in @($rawTyped | Where-Object { $_ -like "{*" } | ForEach-Object { $_ | ConvertFrom-Json } | Where-Object { $_.PSObject.Properties.Name -contains "id" })) { $typedById[[int]$response.id] = $response }
    $text = { param($id) [string]$typedById[$id].result.content[0].text }
    $refusal = { param($id) (& $text $id) | ConvertFrom-Json }
    $near = { param($a, $b) [Math]::Abs([double]$a - [double]$b) -lt 0.002 }
    # A shader parameter the material does not override reads null in some
    # editors and the shader's declared default in others (a 4.7.2 editor with
    # a window answers the default), so an undone one is compared with what
    # the same editor read before the batch rather than with either.
    $sameColour = { param($a, $b)
        if ($null -eq $a -or $null -eq $b) { return ($null -eq $a) -and ($null -eq $b) }
        return (& $near $a.r $b.r) -and (& $near $a.g $b.g) -and (& $near $a.b $b.b) -and (& $near $a.a $b.a)
    }
    [void](Tool-Payload $typedById[7102])

    # Each property's declared type, and what the engine declares about it.
    $before = @((Tool-Payload $typedById[7103]).reads)
    Assert-True ($before.Count -eq 4) "A batch read did not answer every read in order: $(& $text 7103)"
    Assert-True ($before[0].type -eq "Color" -and (& $near $before[0].value.r 1) -and (& $near $before[0].value.g 0)) "A path into an embedded StyleBox did not read its bg_color as a Color: $(& $text 7103)"
    Assert-True ($before[1].type -eq "Color") "A shader parameter reached by path did not report its declared type: $(& $text 7103)"
    Assert-True ($before[2].type -eq "Vector2" -and $before[2].engine_constraint.kind -eq "range") "A Node2D position did not carry the range the engine declares: $(& $text 7103)"
    Assert-True ($before[3].type -eq "Object" -and $before[3].holds -eq "StyleBoxFlat" -and $before[3].engine_constraint.kind -eq "resource_type" -and $before[3].engine_constraint.hint_string -eq "StyleBox") "A theme override slot did not say what it holds and what it takes: $(& $text 7103)"

    # The dry run read every target through the batch's own checks.
    $preview = Tool-Payload $typedById[7104]
    Assert-True ($preview.dry_run -eq $true -and $preview.mutation_preview.changes[0].kind -eq "planned_mutation" -and @($preview.mutation_preview.changes[0].before.writes).Count -eq 3) "A dry run of a batch did not preview each write's before state: $(& $text 7104)"

    $written = Tool-Payload $typedById[7105]
    $writes = @($written.writes)
    Assert-True ($written.applied -eq $true -and $writes.Count -eq 3 -and $written.undo_redo_registered -eq $true -and $written.scene_saved -eq $false) "A batch write did not land as one registered action: $(& $text 7105)"
    # value is read back from the engine: a hex string comes back as the Color
    # the engine stores, so an answer that echoed the request would fail here.
    Assert-True ((& $near $writes[0].value.r (32 / 255)) -and (& $near $writes[0].value.b (96 / 255)) -and (& $near $writes[0].value.a 1)) "A Color written by hex into a StyleBox was not read back as the Color stored: $(& $text 7105)"
    Assert-True ((& $near $writes[1].value.g 1) -and (& $near $writes[2].value.x 5) -and (& $near $writes[2].value.y 6)) "A batch did not read back every write: $(& $text 7105)"
    Assert-True (@($written.follow_up | Where-Object { $_.work -eq "save" }).Count -eq 1) "A batch write did not name the save it leaves: $(& $text 7105)"

    # One undo reverts all three writes, and one redo restores all three.
    $applied = @((Tool-Payload $typedById[7106]).reads)
    [void](Tool-Payload $typedById[7107])
    $undone = @((Tool-Payload $typedById[7108]).reads)
    [void](Tool-Payload $typedById[7109])
    $redone = @((Tool-Payload $typedById[7110]).reads)
    Assert-True ((& $near $applied[0].value.r (32 / 255)) -and (& $near $applied[2].value.x 5)) "A read after the batch did not see it: $(& $text 7106)"
    Assert-True ((& $sameColour $undone[0].value $before[0].value) -and (& $sameColour $undone[1].value $before[1].value) -and (& $near $undone[2].value.x 0) -and (& $near $undone[2].value.y 0)) "One editor_undo did not revert every write of the batch: before $(& $text 7103) after $(& $text 7108)"
    Assert-True ((& $near $redone[0].value.r (32 / 255)) -and (& $near $redone[1].value.g 1) -and (& $near $redone[2].value.x 5)) "One editor_redo did not restore every write of the batch: $(& $text 7110)"

    # A resource kept in its own file: written, and named.
    $shared = Tool-Payload $typedById[7111]
    Assert-True ($shared.applied -eq $true -and $shared.resource_file -eq "res://typed_shared_box.tres") "A write into a resource kept in its own file did not name that file: $(& $text 7111)"

    $instanced = & $refusal 7112
    Assert-True ($typedById[7112].result.isError -and $instanced.error.code -eq 409 -and $instanced.error.data.code -eq "subresource_not_saved" -and $instanced.error.data.resource_file -eq "res://typed_child.tscn" -and $instanced.error.data.next_call.tool -eq "scene_open" -and $instanced.error.data.next_call.arguments.scene_path -eq "res://typed_child.tscn") "A write into a resource the instanced scene keeps was not refused with the scene to open: $(& $text 7112)"

    $missing = & $refusal 7113
    Assert-True ($typedById[7113].result.isError -and $missing.error.code -eq 404 -and $missing.error.data.code -eq "property_not_found" -and @($missing.error.data.candidates)[0] -eq "bg_color" -and $missing.error.data.holder_class -eq "StyleBoxFlat") "A step naming nothing on a StyleBox did not answer with what is there: $(& $text 7113)"

    $overlap = & $refusal 7114
    Assert-True ($typedById[7114].result.isError -and $overlap.error.data.code -eq "batch_writes_overlap" -and $overlap.error.data.index -eq 1 -and $overlap.error.data.overlaps -eq 0) "A slot and a write inside it were accepted in one batch: $(& $text 7114)"

    foreach ($id in 7115, 7116) {
        $excluded = & $refusal $id
        Assert-True ($typedById[$id].result.isError -and $excluded.error.data.code -eq "property_write_excluded" -and $excluded.error.data.use_tool -eq "scene_reparent_node") "A write to a node's owner was not refused by name (request $id): $(& $text $id)"
    }

    $partial = & $refusal 7117
    Assert-True ($typedById[7117].result.isError -and $partial.error.data.index -eq 1 -and $partial.error.data.field -eq "writes") "A batch with a refused second write did not name it: $(& $text 7117)"
    $unmoved = Tool-Payload $typedById[7118]
    Assert-True ((& $near $unmoved.value.x 5) -and (& $near $unmoved.value.y 6)) "A refused batch applied its first write: $(& $text 7118)"

    $member = & $refusal 7119
    Assert-True ($typedById[7119].result.isError -and $member.error.data.code -eq "property_path_not_resource") "A path into a Vector2's member was not refused: $(& $text 7119)"

    # What the save kept, read from the files rather than from the tools.
    Assert-True ((Tool-Payload $typedById[7120]).status -eq "saved") "The typed object layer scene was not saved: $(& $text 7120)"
    $sceneText = Get-Content -LiteralPath (Join-Path $FixtureRoot "typed_object_layer.tscn") -Raw
    Assert-True ($sceneText -match 'bg_color = Color\(0\.125' -and $sceneText -match 'shader_parameter/tint = Color\(0, 1, 0, 1\)' -and $sceneText -match 'position = Vector2\(5, 6\)') "The saved scene does not hold the batch:`n$sceneText"
    $sharedText = Get-Content -LiteralPath (Join-Path $FixtureRoot "typed_shared_box.tres") -Raw
    Assert-True ($sharedText -match 'bg_color = Color\(0, 1, 0, 1\)') "Saving the scene did not rewrite the resource file the write named:`n$sharedText"
    $childText = Get-Content -LiteralPath (Join-Path $FixtureRoot "typed_child.tscn") -Raw
    Assert-True ($childText -match 'bg_color = Color\(0\.2, 0\.2, 0\.2, 1\)') "The refused write reached the instanced scene's file:`n$childText"
    foreach ($id in 7121, 7122) { [void](Tool-Payload $typedById[$id]) }
    Write-Output "Typed object layer: a batch over three nodes undid and redid as one step, and the save kept what the answers said."
}
