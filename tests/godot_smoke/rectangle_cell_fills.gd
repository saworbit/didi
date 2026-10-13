@tool
extends Node

var state: Dictionary:
    get:
        var tiles: TileMapLayer = get_node("Tiles")
        var grid: GridMap = get_node("Grid")
        var floor_count := 0
        var wall_count := 0
        for coords in tiles.get_used_cells():
            var atlas := tiles.get_cell_atlas_coords(coords)
            if atlas == Vector2i(0, 0):
                floor_count += 1
            elif atlas == Vector2i(1, 0):
                wall_count += 1
        return {"tile_count": tiles.get_used_cells().size(), "floor_count": floor_count,
                "wall_count": wall_count, "grid_count": grid.get_used_cells().size()}
