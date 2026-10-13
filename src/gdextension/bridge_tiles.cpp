#include "bridge_internal.hpp"
#include "didi/gdextension/godot_bridge.hpp"
#include "didi/common/cell_records.hpp"
#include <algorithm>
#include <tuple>
#include <utility>

namespace didi::godot {
using namespace bridge_internal;

json GodotBridge::bridgeTilemapGetUsedRect(const std::string& /*method*/, const json& params, const std::string& session_kind, GDExtensionObjectPtr editor) {
    if (session_kind != "editor") return bridgeError(409, "session_kind_rejected");
    if (!hasOnlyKeys(params, {"tilemap_path"}) || !params.contains("tilemap_path") ||
        !params["tilemap_path"].is_string() || params["tilemap_path"].get<std::string>().empty() ||
        params["tilemap_path"].get<std::string>().size() > 1024) {
        return bridgeError(400, "invalid_tilemap_get_used_rect_request");
    }
    if (requireMethodBind("TileMapLayer", "get_used_rect", 410525958LL).isErr() ||
        requireMethodBind("Object", "is_class", 3927539163LL).isErr()) {
        return bridgeError(501, "required_bind_unavailable");
    }
    auto root = editedSceneRoot(editor);
    if (root.isErr()) return errorJson(root.error());
    auto layer = resolveNode(root.value(), params["tilemap_path"].get<std::string>());
    const auto tilemap_path = params["tilemap_path"].get<std::string>();
    if (layer.isErr()) {
        return bridgeError(404, "tilemap_target_not_found", {{"path", tilemap_path}},
                           "Path " + tilemap_path + " resolves to nothing.");
    }
    auto correct_class = objectIsClass(layer.value(), "TileMapLayer");
    if (correct_class.isErr()) return errorJson(500, correct_class.error().message);
    if (!correct_class.value()) {
        return wrongNodeTypeError("tilemap_target_wrong_type", tilemap_path, layer.value());
    }
    auto rect = callObject(layer.value(), "TileMapLayer", "get_used_rect", 410525958LL);
    if (rect.isErr()) return errorJson(500, rect.error().message);
    auto& api = GodotApi::instance();
    if (api.variant_get_type(rect.value().ptr()) != GDEXTENSION_VARIANT_TYPE_RECT2I ||
        !api.variant_get_ptr_getter) return bridgeError(500, "extension_protocol_error");
    auto converter = api.get_variant_to_type_constructor(GDEXTENSION_VARIANT_TYPE_RECT2I);
    NativeName position_name("position"), size_name("size");
    auto get_position = position_name.valid()
                            ? api.variant_get_ptr_getter(GDEXTENSION_VARIANT_TYPE_RECT2I, position_name.ptr())
                            : nullptr;
    auto get_size = size_name.valid()
                        ? api.variant_get_ptr_getter(GDEXTENSION_VARIANT_TYPE_RECT2I, size_name.ptr())
                        : nullptr;
    if (!converter || !get_position || !get_size) return bridgeError(501, "required_bind_unavailable");
    NativeValue native_rect(GDEXTENSION_VARIANT_TYPE_RECT2I);
    converter(native_rect.ptr(), rect.value().ptr());
    native_rect.markInitialized();
    NativeValue native_position(GDEXTENSION_VARIANT_TYPE_VECTOR2I);
    NativeValue native_size(GDEXTENSION_VARIANT_TYPE_VECTOR2I);
    get_position(native_rect.ptr(), native_position.ptr());
    get_size(native_rect.ptr(), native_size.ptr());
    native_position.markInitialized();
    native_size.markInitialized();
    auto position_variant = variantFromNative(GDEXTENSION_VARIANT_TYPE_VECTOR2I, native_position.ptr());
    auto size_variant = variantFromNative(GDEXTENSION_VARIANT_TYPE_VECTOR2I, native_size.ptr());
    if (position_variant.isErr() || size_variant.isErr()) return bridgeError(500, "extension_protocol_error");
    auto position = integerVectorToJson(position_variant.value(), 2);
    auto size = integerVectorToJson(size_variant.value(), 2);
    if (position.isErr() || size.isErr()) return bridgeError(500, "extension_protocol_error");
    return liveResult({{"tilemap_path", params["tilemap_path"]},
                       {"position", position.value()}, {"size", size.value()},
                       {"end", {{"x", position.value()["x"].get<int64_t>() + size.value()["x"].get<int64_t>()},
                                {"y", position.value()["y"].get<int64_t>() + size.value()["y"].get<int64_t>()}}}});
}


json GodotBridge::bridgeTilemapSetCells(const std::string& /*method*/, const json& params, const std::string& session_kind, GDExtensionObjectPtr editor) {
    if (session_kind != "editor") return bridgeError(409, "session_kind_rejected");
    if (!hasOnlyKeys(params, {"tilemap_path", "cells"}) ||
        !params.contains("tilemap_path") || !params["tilemap_path"].is_string() ||
        params["tilemap_path"].get<std::string>().empty() ||
        params["tilemap_path"].get<std::string>().size() > 1024 ||
        !params.contains("cells") || !params["cells"].is_array() ||
        params["cells"].empty() || params["cells"].size() > 256) {
        return bridgeError(400, "invalid_tilemap_set_cells_request");
    }
    auto prepared = cells::prepare(params["cells"], 2);
    if (prepared.isErr()) return errorJson(prepared.error());
    for (const auto& bind : {
             std::make_tuple("Object", "is_class", 3927539163LL),
             std::make_tuple("TileMapLayer", "get_tile_set", 2678226422LL),
             std::make_tuple("TileMapLayer", "set_cell", 2428518503LL),
             std::make_tuple("TileMapLayer", "erase_cell", 1130785943LL),
             std::make_tuple("TileMapLayer", "get_cell_source_id", 2485466453LL),
             std::make_tuple("TileMapLayer", "get_cell_atlas_coords", 3050897911LL),
             std::make_tuple("TileMapLayer", "get_cell_alternative_tile", 2485466453LL),
             std::make_tuple("TileSet", "has_source", 1116898809LL),
             std::make_tuple("TileSet", "get_source", 1763540252LL),
             std::make_tuple("TileSetAtlasSource", "get_tile_data", 3534028207LL)}) {
        if (requireMethodBind(std::get<0>(bind), std::get<1>(bind), std::get<2>(bind)).isErr())
            return bridgeError(501, "required_bind_unavailable");
    }
    if (preflightUndoManagerBindings().isErr() || preflightUndoRollbackBindings().isErr())
        return bridgeError(501, "required_bind_unavailable");
    auto root = editedSceneRoot(editor);
    if (root.isErr()) return errorJson(root.error());
    auto layer = resolveNode(root.value(), params["tilemap_path"].get<std::string>());
    const auto tilemap_path = params["tilemap_path"].get<std::string>();
    if (layer.isErr()) {
        return bridgeError(404, "tilemap_target_not_found", {{"path", tilemap_path}},
                           "Path " + tilemap_path + " resolves to nothing.");
    }
    if (auto refused = refuseUnsavableEdit(root.value(), layer.value(), tilemap_path,
                                           SceneEdit::Property)) {
        return *refused;
    }
    auto correct_class = objectIsClass(layer.value(), "TileMapLayer");
    if (correct_class.isErr() || !correct_class.value()) {
        return wrongNodeTypeError("tilemap_target_wrong_type", tilemap_path, layer.value());
    }
    auto tile_set_variant = callObject(layer.value(), "TileMapLayer", "get_tile_set", 2678226422LL);
    if (tile_set_variant.isErr()) return errorJson(500, tile_set_variant.error().message);
    auto tile_set = objectFromVariant(tile_set_variant.value());
    if (tile_set.isErr()) return errorJson(500, tile_set.error().message);
    // A layer with no TileSet used to be reported as tilemap_tile_not_found
    // once the first cell was examined, which reads as a bad source id or
    // bad atlas coordinates and sends the caller looking at their payload.
    // The cause is one unset property on the node, and it is worth saying so
    // before any cell is looked at.
    if (!tile_set.value()) {
        return bridgeError(409, "tilemap_layer_has_no_tileset",
                         {{"tilemap_path", params["tilemap_path"].get<std::string>()},
                          {"retryable", false}});
    }

    struct Cell {
        VariantValue coords;
        bool erase{false};
        int64_t source{-1};
        VariantValue atlas;
        int64_t alternative{-1};
        int64_t old_source{-1};
        VariantValue old_atlas;
        int64_t old_alternative{-1};
        bool changed{false};
    };
    std::vector<Cell> cells;
    std::set<std::pair<int64_t, int64_t>> seen;
    std::set<std::tuple<int64_t, int64_t, int64_t, int64_t>> validated_tiles;
    for (const auto& record : prepared.value().expanded) {
        if (!record.is_object() || !record.contains("coords") || !record["coords"].is_array() ||
            record["coords"].size() != 2) return bridgeError(400, "invalid_tilemap_set_cells_request");
        const auto x_value = boundedJsonInteger(record["coords"][0], -1048576, 1048576);
        const auto y_value = boundedJsonInteger(record["coords"][1], -1048576, 1048576);
        if (!x_value.has_value() || !y_value.has_value())
            return bridgeError(400, "invalid_tilemap_set_cells_request");
        const int64_t x = *x_value;
        const int64_t y = *y_value;
        if (!seen.emplace(x, y).second) return bridgeError(409, "duplicate_tilemap_coordinate");
        const bool erase = record.contains("erase");
        if ((erase && (!hasOnlyKeys(record, {"coords", "erase"}) || !record["erase"].is_boolean() || !record["erase"].get<bool>())) ||
            (!erase && (!hasOnlyKeys(record, {"coords", "source_id", "atlas_coords", "alternative_tile"}) ||
                        !record.contains("source_id") || !record["source_id"].is_number_integer() ||
                        !record.contains("atlas_coords") || !record["atlas_coords"].is_array() || record["atlas_coords"].size() != 2)))
            return bridgeError(400, "invalid_tilemap_set_cells_request");
        auto coords = makeVector2i(x, y);
        auto empty_atlas = makeVector2i(-1, -1);
        if (coords.isErr() || empty_atlas.isErr()) return bridgeError(501, "required_bind_unavailable");
        int64_t source = -1, alternative = -1;
        VariantValue atlas = std::move(empty_atlas.value());
        if (!erase) {
            const auto source_value_int = boundedJsonInteger(record["source_id"], 0, 2147483647LL);
            const auto alternative_value_int = record.contains("alternative_tile")
                ? boundedJsonInteger(record["alternative_tile"], 0, 65535)
                : std::optional<int64_t>(0);
            if (!source_value_int.has_value() || !alternative_value_int.has_value())
                return bridgeError(400, "invalid_tilemap_set_cells_request");
            source = *source_value_int;
            alternative = *alternative_value_int;
            const auto ax_value = boundedJsonInteger(record["atlas_coords"][0], 0, 1048576);
            const auto ay_value = boundedJsonInteger(record["atlas_coords"][1], 0, 1048576);
            if (!ax_value.has_value() || !ay_value.has_value())
                return bridgeError(400, "invalid_tilemap_set_cells_request");
            const int64_t ax = *ax_value;
            const int64_t ay = *ay_value;
            auto requested_atlas = makeVector2i(ax, ay);
            if (requested_atlas.isErr()) return bridgeError(501, "required_bind_unavailable");
            const auto tile_key = std::make_tuple(source, ax, ay, alternative);
            if (!validated_tiles.count(tile_key)) {
                auto source_value = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, source);
                auto has_source = callObject(tile_set.value(), "TileSet", "has_source", 1116898809LL, {&source_value.value()});
                auto has_flag = has_source.isOk() ? scalarFromVariant<GDExtensionBool>(has_source.value(), GDEXTENSION_VARIANT_TYPE_BOOL)
                                                  : Result<GDExtensionBool>(has_source.error());
                if (has_flag.isErr() || !has_flag.value()) return bridgeError(404, "tilemap_tile_not_found");
                auto source_object_value = callObject(tile_set.value(), "TileSet", "get_source", 1763540252LL, {&source_value.value()});
                auto source_object = source_object_value.isOk() ? objectFromVariant(source_object_value.value())
                                                                : Result<GDExtensionObjectPtr>(source_object_value.error());
                if (source_object.isErr() || !source_object.value()) return bridgeError(404, "tilemap_tile_not_found");
                auto atlas_class = objectIsClass(source_object.value(), "TileSetAtlasSource");
                if (atlas_class.isErr() || !atlas_class.value()) return bridgeError(404, "tilemap_tile_not_found");
                auto alternative_value = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, alternative);
                auto tile_data_value = callObject(source_object.value(), "TileSetAtlasSource", "get_tile_data", 3534028207LL,
                                                  {&requested_atlas.value(), &alternative_value.value()});
                auto tile_data = tile_data_value.isOk() ? objectFromVariant(tile_data_value.value())
                                                        : Result<GDExtensionObjectPtr>(tile_data_value.error());
                if (tile_data.isErr() || !tile_data.value()) return bridgeError(404, "tilemap_tile_not_found");
                validated_tiles.insert(tile_key);
            }
            atlas = std::move(requested_atlas.value());
        }
        auto old_source_value = callObject(layer.value(), "TileMapLayer", "get_cell_source_id", 2485466453LL, {&coords.value()});
        auto old_atlas = callObject(layer.value(), "TileMapLayer", "get_cell_atlas_coords", 3050897911LL, {&coords.value()});
        auto old_alt_value = callObject(layer.value(), "TileMapLayer", "get_cell_alternative_tile", 2485466453LL, {&coords.value()});
        if (old_source_value.isErr() || old_atlas.isErr() || old_alt_value.isErr()) return bridgeError(500, "tilemap_snapshot_failed");
        auto old_source = scalarFromVariant<int64_t>(old_source_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
        auto old_alt = scalarFromVariant<int64_t>(old_alt_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
        auto old_atlas_json = integerVectorToJson(old_atlas.value(), 2);
        auto atlas_json = integerVectorToJson(atlas, 2);
        if (old_source.isErr() || old_alt.isErr() || old_atlas_json.isErr() || atlas_json.isErr()) return bridgeError(500, "tilemap_snapshot_failed");
        const bool changed = old_source.value() != source || old_alt.value() != alternative || old_atlas_json.value() != atlas_json.value();
        cells.push_back(Cell{std::move(coords.value()), erase, source, std::move(atlas), alternative,
                             old_source.value(), std::move(old_atlas.value()), old_alt.value(), changed});
    }
    const size_t changed_count = std::count_if(cells.begin(), cells.end(), [](const Cell& cell) { return cell.changed; });
    // What each requested cell holds, read from the layer: the snapshot
    // for a cell nothing changed, the read after the commit for the rest.
    // The counts are about the request, and nothing in the answer came
    // from the layer after the write (#1019).
    json observed_cells = json::array();
    const auto observedCell = [](VariantValue& coords, int64_t source, const json& atlas,
                                 int64_t alternative) {
        auto coords_json = integerVectorToJson(coords, 2);
        return json{{"coords", coords_json.isOk() ? coords_json.value() : json(nullptr)},
                    {"source_id", source}, {"atlas_coords", atlas}, {"alternative_tile", alternative}};
    };
    const auto snapshotCell = [&](Cell& cell) {
        auto atlas = integerVectorToJson(cell.old_atlas, 2);
        return observedCell(cell.coords, cell.old_source, atlas.isOk() ? atlas.value() : json(nullptr),
                            cell.old_alternative);
    };
    if (changed_count == 0) {
        for (auto& cell : cells) observed_cells.push_back(snapshotCell(cell));
        return liveResult({{"requested_cells", cells.size()}, {"changed_cells", 0},
            {"unchanged_cells", cells.size()}, {"cells", cells::compactObserved(prepared.value(), observed_cells, 2)}, {"undo_redo_registered", false}, {"outcome", "completed"}, {"rollback", "not_required"}});
    }
    auto manager = undoManager(editor);
    if (manager.isErr()) return errorJson(manager.error());
    auto action = createAction(manager.value(), "Didi: set TileMapLayer cells", layer.value());
    if (action.isErr()) return errorJson(500, action.error().message);
    for (auto& cell : cells) if (cell.changed) {
        auto source_value = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, cell.source);
        auto alt_value = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, cell.alternative);
        auto old_source_value = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, cell.old_source);
        auto old_alt_value = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, cell.old_alternative);
        auto registered = cell.erase
            ? managerMethod(manager.value(), "add_do_method", layer.value(), "erase_cell", {&cell.coords})
            : managerMethod(manager.value(), "add_do_method", layer.value(), "set_cell", {&cell.coords, &source_value.value(), &cell.atlas, &alt_value.value()});
        if (registered.isOk()) registered = cell.old_source < 0
            ? managerMethod(manager.value(), "add_undo_method", layer.value(), "erase_cell", {&cell.coords})
            : managerMethod(manager.value(), "add_undo_method", layer.value(), "set_cell", {&cell.coords, &old_source_value.value(), &cell.old_atlas, &old_alt_value.value()});
        if (registered.isErr()) { abandonAction(manager.value()); return bridgeError(500, "tilemap_undo_registration_failed"); }
    }
    auto committed = commitAction(manager.value());
    if (committed.isErr()) return errorJson(500, committed.error().message);
    for (auto& cell : cells) {
        if (!cell.changed) {
            observed_cells.push_back(snapshotCell(cell));
            continue;
        }
        auto observed_source_value = callObject(layer.value(), "TileMapLayer", "get_cell_source_id", 2485466453LL, {&cell.coords});
        auto observed_atlas_value = callObject(layer.value(), "TileMapLayer", "get_cell_atlas_coords", 3050897911LL, {&cell.coords});
        auto observed_alt_value = callObject(layer.value(), "TileMapLayer", "get_cell_alternative_tile", 2485466453LL, {&cell.coords});
        auto observed_source = observed_source_value.isOk() ? scalarFromVariant<int64_t>(observed_source_value.value(), GDEXTENSION_VARIANT_TYPE_INT)
                                                            : Result<int64_t>(observed_source_value.error());
        auto observed_alt = observed_alt_value.isOk() ? scalarFromVariant<int64_t>(observed_alt_value.value(), GDEXTENSION_VARIANT_TYPE_INT)
                                                      : Result<int64_t>(observed_alt_value.error());
        auto observed_atlas = observed_atlas_value.isOk() ? integerVectorToJson(observed_atlas_value.value(), 2)
                                                          : Result<json>(observed_atlas_value.error());
        auto expected_atlas = integerVectorToJson(cell.atlas, 2);
        if (observed_source.isErr() || observed_alt.isErr() || observed_atlas.isErr() ||
            expected_atlas.isErr() || observed_source.value() != cell.source ||
            observed_alt.value() != cell.alternative || observed_atlas.value() != expected_atlas.value()) {
            const auto restored = undoLastAction(manager.value(), root.value());
            return bridgeError(500, "tilemap_postcondition_mismatch", {{"outcome", restored.isOk() ? "rolled_back" : "unknown"}});
        }
        observed_cells.push_back(observedCell(cell.coords, observed_source.value(),
                                              observed_atlas.value(), observed_alt.value()));
    }
    return liveSceneMutation({{"requested_cells", cells.size()}, {"changed_cells", changed_count},
        {"unchanged_cells", cells.size() - changed_count}, {"cells", cells::compactObserved(prepared.value(), observed_cells, 2)}, {"undo_redo_registered", true}, {"outcome", "completed"}, {"rollback", "undo_redo"}});
}


json GodotBridge::bridgeGridmapSetCells(const std::string& /*method*/, const json& params, const std::string& session_kind, GDExtensionObjectPtr editor) {
    if (session_kind != "editor") return bridgeError(409, "session_kind_rejected");
    if (!hasOnlyKeys(params, {"gridmap_path", "cells"}) || !params.contains("gridmap_path") ||
        !params["gridmap_path"].is_string() || params["gridmap_path"].get<std::string>().empty() ||
        params["gridmap_path"].get<std::string>().size() > 1024 ||
        !params.contains("cells") || !params["cells"].is_array() || params["cells"].empty() ||
        params["cells"].size() > 256) return bridgeError(400, "invalid_gridmap_set_cells_request");
    auto prepared = cells::prepare(params["cells"], 3);
    if (prepared.isErr()) return errorJson(prepared.error());
    for (const auto& bind : {std::make_tuple("Object", "is_class", 3927539163LL),
             std::make_tuple("GridMap", "get_mesh_library", 3350993772LL),
             std::make_tuple("GridMap", "set_cell_item", 3449088946LL),
             std::make_tuple("GridMap", "get_cell_item", 3724960147LL),
             std::make_tuple("GridMap", "get_cell_item_orientation", 3724960147LL),
             std::make_tuple("MeshLibrary", "get_item_list", 1930428628LL)}) {
        if (requireMethodBind(std::get<0>(bind), std::get<1>(bind), std::get<2>(bind)).isErr()) return bridgeError(501, "required_bind_unavailable");
    }
    if (preflightUndoManagerBindings().isErr() || preflightUndoRollbackBindings().isErr())
        return bridgeError(501, "required_bind_unavailable");
    auto root = editedSceneRoot(editor);
    if (root.isErr()) return errorJson(root.error());
    auto grid = resolveNode(root.value(), params["gridmap_path"].get<std::string>());
    const auto gridmap_path = params["gridmap_path"].get<std::string>();
    if (grid.isErr()) {
        return bridgeError(404, "gridmap_target_not_found", {{"path", gridmap_path}},
                           "Path " + gridmap_path + " resolves to nothing.");
    }
    if (auto refused = refuseUnsavableEdit(root.value(), grid.value(), gridmap_path,
                                           SceneEdit::Property)) {
        return *refused;
    }
    auto correct_class = objectIsClass(grid.value(), "GridMap");
    if (correct_class.isErr() || !correct_class.value()) {
        return wrongNodeTypeError("gridmap_target_wrong_type", gridmap_path, grid.value());
    }
    auto library_value = callObject(grid.value(), "GridMap", "get_mesh_library", 3350993772LL);
    auto library = library_value.isOk() ? objectFromVariant(library_value.value())
                                        : Result<GDExtensionObjectPtr>(library_value.error());
    std::set<int64_t> item_ids;
    if (library.isOk() && library.value()) {
        auto list = callObject(library.value(), "MeshLibrary", "get_item_list", 1930428628LL);
        if (list.isErr()) return errorJson(500, list.error().message);
        auto ids = packedIntegerSet(list.value());
        if (ids.isErr()) return errorJson(500, ids.error().message);
        item_ids = std::move(ids.value());
    }
    struct Cell { VariantValue position; int64_t item; int64_t orientation; int64_t old_item; int64_t old_orientation; bool changed; };
    std::vector<Cell> cells;
    std::set<std::tuple<int64_t, int64_t, int64_t>> seen;
    for (const auto& record : prepared.value().expanded) {
        if (!hasOnlyKeys(record, {"position", "item", "orientation"}) || !record.contains("position") ||
            !record["position"].is_array() || record["position"].size() != 3 || !record.contains("item") ||
            !record["item"].is_number_integer()) return bridgeError(400, "invalid_gridmap_set_cells_request");
        int64_t xyz[3];
        for (int axis = 0; axis < 3; ++axis) {
            const auto coordinate = boundedJsonInteger(record["position"][axis], -1048576, 1048576);
            if (!coordinate.has_value()) return bridgeError(400, "invalid_gridmap_set_cells_request");
            xyz[axis] = *coordinate;
        }
        if (!seen.emplace(xyz[0], xyz[1], xyz[2]).second) return bridgeError(409, "duplicate_gridmap_position");
        const auto item_value = boundedJsonInteger(record["item"], -1, 2147483647LL);
        const auto orientation_value = record.contains("orientation")
            ? boundedJsonInteger(record["orientation"], 0, 23)
            : std::optional<int64_t>(0);
        if (!item_value.has_value() || !orientation_value.has_value())
            return bridgeError(400, "invalid_gridmap_set_cells_request");
        const int64_t item = *item_value;
        const int64_t orientation = *orientation_value;
        if (item == -1 && orientation != 0) return bridgeError(400, "invalid_gridmap_set_cells_request");
        if (item >= 0 && (!library.isOk() || !library.value() || !item_ids.count(item))) return bridgeError(404, "gridmap_item_not_found");
        auto position = makeVector3i(xyz[0], xyz[1], xyz[2]);
        if (position.isErr()) return bridgeError(501, "required_bind_unavailable");
        auto old_item_value = callObject(grid.value(), "GridMap", "get_cell_item", 3724960147LL, {&position.value()});
        auto old_orientation_value = callObject(grid.value(), "GridMap", "get_cell_item_orientation", 3724960147LL, {&position.value()});
        if (old_item_value.isErr() || old_orientation_value.isErr()) return bridgeError(500, "gridmap_snapshot_failed");
        auto old_item = scalarFromVariant<int64_t>(old_item_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
        auto old_orientation = scalarFromVariant<int64_t>(old_orientation_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
        if (old_item.isErr() || old_orientation.isErr()) return bridgeError(500, "gridmap_snapshot_failed");
        const bool changed = item != old_item.value() || (item >= 0 && orientation != old_orientation.value());
        cells.push_back(Cell{std::move(position.value()), item, orientation, old_item.value(), old_orientation.value(), changed});
    }
    const size_t changed_count = std::count_if(cells.begin(), cells.end(), [](const Cell& cell) { return cell.changed; });
    // What each requested cell holds, read from the grid, as the layer
    // writer does (#1019).
    json observed_cells = json::array();
    const auto observedCell = [](VariantValue& position, int64_t item, int64_t orientation) {
        auto position_json = integerVectorToJson(position, 3);
        return json{{"position", position_json.isOk() ? position_json.value() : json(nullptr)},
                    {"item", item}, {"orientation", orientation}};
    };
    if (changed_count == 0) {
        for (auto& cell : cells) observed_cells.push_back(observedCell(cell.position, cell.old_item, cell.old_orientation));
        return liveResult({{"requested_cells", cells.size()}, {"changed_cells", 0},
            {"unchanged_cells", cells.size()}, {"cells", cells::compactObserved(prepared.value(), observed_cells, 3)}, {"undo_redo_registered", false}, {"outcome", "completed"}, {"rollback", "not_required"}});
    }
    auto manager = undoManager(editor);
    if (manager.isErr()) return errorJson(manager.error());
    auto action = createAction(manager.value(), "Didi: set GridMap cells", grid.value());
    if (action.isErr()) return errorJson(500, action.error().message);
    for (auto& cell : cells) if (cell.changed) {
        auto item = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, cell.item);
        auto orientation = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, cell.orientation);
        auto old_item = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, cell.old_item);
        auto old_orientation = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, cell.old_orientation);
        auto registered = managerMethod(manager.value(), "add_do_method", grid.value(), "set_cell_item", {&cell.position, &item.value(), &orientation.value()});
        if (registered.isOk()) registered = managerMethod(manager.value(), "add_undo_method", grid.value(), "set_cell_item", {&cell.position, &old_item.value(), &old_orientation.value()});
        if (registered.isErr()) { abandonAction(manager.value()); return bridgeError(500, "gridmap_undo_registration_failed"); }
    }
    auto committed = commitAction(manager.value());
    if (committed.isErr()) return errorJson(500, committed.error().message);
    for (auto& cell : cells) {
        if (!cell.changed) {
            observed_cells.push_back(observedCell(cell.position, cell.old_item, cell.old_orientation));
            continue;
        }
        auto observed_item_value = callObject(grid.value(), "GridMap", "get_cell_item", 3724960147LL, {&cell.position});
        auto observed_orientation_value = callObject(grid.value(), "GridMap", "get_cell_item_orientation", 3724960147LL, {&cell.position});
        auto observed_item = observed_item_value.isOk() ? scalarFromVariant<int64_t>(observed_item_value.value(), GDEXTENSION_VARIANT_TYPE_INT)
                                                        : Result<int64_t>(observed_item_value.error());
        auto observed_orientation = observed_orientation_value.isOk() ? scalarFromVariant<int64_t>(observed_orientation_value.value(), GDEXTENSION_VARIANT_TYPE_INT)
                                                                      : Result<int64_t>(observed_orientation_value.error());
        if (observed_item.isErr() || observed_orientation.isErr() ||
            observed_item.value() != cell.item ||
            (cell.item >= 0 && observed_orientation.value() != cell.orientation)) {
            const auto restored = undoLastAction(manager.value(), root.value());
            return bridgeError(500, "gridmap_postcondition_mismatch", {{"outcome", restored.isOk() ? "rolled_back" : "unknown"}});
        }
        observed_cells.push_back(observedCell(cell.position, observed_item.value(), observed_orientation.value()));
    }
    return liveSceneMutation({{"requested_cells", cells.size()}, {"changed_cells", changed_count},
        {"unchanged_cells", cells.size() - changed_count}, {"cells", cells::compactObserved(prepared.value(), observed_cells, 3)}, {"undo_redo_registered", true}, {"outcome", "completed"}, {"rollback", "undo_redo"}});
}


} // namespace didi::godot
