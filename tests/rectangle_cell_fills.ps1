# #1322. Compact rectangular records reuse each cell writer's one undo action.
function Invoke-RectangleCellFillsBlock {
    param($EditorSession, [string]$FixtureRoot)
    $server = Start-DidiSession @("--project", $FixtureRoot)
    $tile = "/root/RectangleFills/Tiles"
    $grid = "/root/RectangleFills/Grid"
    $call = {
        param($name, $arguments)
        $id = 8600 + $server.Requests.Count
        Send-DidiSession $server (Tool-Request $id $name $arguments) 15
    }
    $get = { param($name, $arguments) Tool-Payload (& $call $name $arguments) }
    $state = { (Tool-Payload (& $call "scene_get_property" @{ target_node = "/root/RectangleFills"; property_name = "state" })).value }
    $wall = @{ rect = @{ position = @{ x = 0; y = 0 }; size = @{ x = 40; y = 24 } }; source_id = 0; atlas_coords = @(1, 0) }
    $floor = @{ rect = @{ position = @(1, 1); size = @{ x = 38; y = 22 } }; source_id = 0; atlas_coords = @(0, 0) }
    $erase = @{ rect = $wall.rect; erase = $true }
    $large = @{ rect = @{ position = @(100, 100); size = @{ x = 64; y = 64 } }; source_id = 0; atlas_coords = @(0, 0) }
    $cube = @{ rect = @{ position = @(0, 0, 0); size = @{ x = 16; y = 16; z = 16 } }; item = 0; orientation = 1 }
    $durations = @()
    try {
        [void](Send-DidiSession $server (@{ jsonrpc = "2.0"; id = 8599; method = "initialize"; params = @{ protocolVersion = "2024-11-05" } } | ConvertTo-Json -Compress) 10)
        [void](& $get "runtime_attach_session" @{ session_id = $EditorSession.session_id })
        [void](& $get "scene_open" @{ scene_path = "res://rectangle_cell_fills.tscn" })
        $preview = & $get "tilemap_set_cells" @{ tilemap_path = $tile; cells = @($wall); dry_run = $true }
        Assert-True ($null -ne $preview.mutation_preview -and (& $state).tile_count -eq 0) "A rectangle preview changed the layer."
        $invalidPreview = & $call "tilemap_set_cells" @{ tilemap_path = $tile; cells = @($large, @{ coords = @(200, 200); erase = $true }); dry_run = $true }
        Assert-True ($invalidPreview.result.isError -eq $true -and (& $state).tile_count -eq 0) "An over-limit rectangle was previewed as executable."
        $filled = & $get "tilemap_set_cells" @{ tilemap_path = $tile; cells = @($wall) }
        Assert-True ($filled.requested_cells -eq 960 -and $filled.changed_cells -eq 960 -and $filled.cells.Count -eq 1 -and $filled.cells[0].cell_count -eq 960 -and $filled.cells[0].atlas_coords.x -eq 1) "Rectangle fill did not return compact observed state."
        [void](& $get "tilemap_set_cells" @{ tilemap_path = $tile; cells = @($floor) })
        $room = & $state
        Assert-True ($room.tile_count -eq 960 -and $room.floor_count -eq 836 -and $room.wall_count -eq 124) "Two rectangles did not make a 40 by 24 room with its border."
        $same = & $get "tilemap_set_cells" @{ tilemap_path = $tile; cells = @($floor) }
        Assert-True ($same.changed_cells -eq 0 -and $same.unchanged_cells -eq 836 -and $same.undo_redo_registered -eq $false) "An identical rectangle created another undo action."
        [void](& $get "editor_undo" @{})
        Assert-True ((& $state).wall_count -eq 960) "One undo did not restore every floor cell."
        [void](& $get "editor_redo" @{})
        Assert-True ((& $state).floor_count -eq 836) "One redo did not restore every floor cell."
        $bad = & $call "tilemap_set_cells" @{ tilemap_path = $tile; cells = @(
            @{ rect = @{ position = @(80, 0); size = @{ x = 2; y = 2 } }; source_id = 0; atlas_coords = @(0, 0) },
            @{ coords = @(90, 0); source_id = 999; atlas_coords = @(0, 0) }
        ) }
        Assert-True ($bad.result.isError -eq $true -and (& $state).tile_count -eq 960) "A late resource refusal partially filled a rectangle."
        $overlap = & $call "tilemap_set_cells" @{ tilemap_path = $tile; cells = @($wall, @{ coords = @(0, 0); erase = $true }) }
        Assert-True ($overlap.result.isError -eq $true -and (& $state).floor_count -eq 836) "Overlapping records changed the layer."
        $erased = & $get "tilemap_set_cells" @{ tilemap_path = $tile; cells = @($erase) }
        Assert-True ($erased.changed_cells -eq 960 -and $erased.cells[0].source_id -eq -1 -and (& $state).tile_count -eq 0) "Rectangle erase did not erase and read back every cell."
        [void](& $get "editor_undo" @{})
        Assert-True ((& $state).tile_count -eq 960) "One undo did not restore a rectangle erase."
        [void](& $get "editor_redo" @{})
        $watch = [Diagnostics.Stopwatch]::StartNew()
        $maxTile = & $get "tilemap_set_cells" @{ tilemap_path = $tile; cells = @($large) }
        $durations += "tile4096=$($watch.ElapsedMilliseconds)ms"
        Assert-True ($maxTile.changed_cells -eq 4096 -and $maxTile.cells.Count -eq 1 -and (& $state).tile_count -eq 4096) "The maximum rectangle did not fill all 4096 cells."
        $tooMuch = & $call "tilemap_set_cells" @{ tilemap_path = $tile; cells = @($large, @{ coords = @(200, 200); erase = $true }) }
        Assert-True ($tooMuch.result.isError -eq $true -and (& $state).tile_count -eq 4096) "The expanded work limit was ignored."
        $watch.Restart()
        $maxGrid = & $get "gridmap_set_cells" @{ gridmap_path = $grid; cells = @($cube) }
        $durations += "grid4096=$($watch.ElapsedMilliseconds)ms"
        Assert-True ($maxGrid.changed_cells -eq 4096 -and $maxGrid.cells.Count -eq 1 -and $maxGrid.cells[0].cell_count -eq 4096 -and $maxGrid.cells[0].orientation -eq 1 -and (& $state).grid_count -eq 4096) "The maximum box did not fill and read back every grid cell."
        $gridSame = & $get "gridmap_set_cells" @{ gridmap_path = $grid; cells = @($cube) }
        Assert-True ($gridSame.changed_cells -eq 0 -and $gridSame.undo_redo_registered -eq $false) "An identical box created another undo action."
        $badGrid = & $call "gridmap_set_cells" @{ gridmap_path = $grid; cells = @(
            @{ rect = @{ position = @(30, 0, 0); size = @{ x = 2; y = 2; z = 2 } }; item = 0 },
            @{ position = @(40, 0, 0); item = 999 }
        ) }
        Assert-True ($badGrid.result.isError -eq $true -and (& $state).grid_count -eq 4096) "A late mesh-library refusal partially filled a box."
        $gridErase = & $get "gridmap_set_cells" @{ gridmap_path = $grid; cells = @(@{ rect = $cube.rect; item = -1 }) }
        Assert-True ($gridErase.changed_cells -eq 4096 -and $gridErase.cells[0].item -eq -1 -and (& $state).grid_count -eq 0) "Box erase did not erase and read back every cell."
        [void](& $get "editor_undo" @{})
        Assert-True ((& $state).grid_count -eq 4096) "One undo did not restore the grid box."
        [void](& $get "editor_save_scene" @{})
        [void](& $get "scene_close" @{ discard_unsaved = $true })
        [void](& $get "scene_open" @{ scene_path = "res://rectangle_cell_fills.tscn" })
        $saved = & $state
        Assert-True ($saved.tile_count -eq 4096 -and $saved.grid_count -eq 4096) "Rectangle writes did not survive save and reload."
        [void](& $get "scene_close" @{ discard_unsaved = $true })
    } finally {
        Stop-DidiSession $server
    }
    Write-Output ("Rectangle fills: room, erase, atomic refusal, undo and save verified; " + ($durations -join ", "))
}
