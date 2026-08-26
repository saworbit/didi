#include "didi/mcp/tool_registry.hpp"
#include "didi/common/logger.hpp"

namespace didi {
namespace mcp {

// External handler forward declarations
CallToolResult handleCaptureViewport(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleViewportSetCameraTransform(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleCreateVisualTestLab(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleViewportToggleDebugDraw(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);

CallToolResult handleGetSceneHierarchy(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleSceneInstantiateNode(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleSceneRemoveNode(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleSceneReparentNode(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleSceneSetProperty(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleSceneGetProperty(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleSceneDuplicateNode(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleMutateSceneTree(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);

CallToolResult handleSignalListConnections(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleSignalConnect(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleSignalDisconnect(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleSignalEmit(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);

CallToolResult handleScriptCheckSyntax(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleScriptReflectClass(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleScriptGetSymbols(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleScriptPatchMethod(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);

CallToolResult handlePhysicsRaycastQuery(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handlePhysicsSimulateStep(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleNavBakeMesh(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleNavQueryPath(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleAnimListTracks(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleAnimPlayTrack(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);

CallToolResult handleTilemapSetCells(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleTilemapGetUsedRect(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleGridmapSetCells(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);

CallToolResult handleQueryProjectResources(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleResourceCreate(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleResourceInspect(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleProjectGetUidMap(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleInstantiateAsset(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);

CallToolResult handleExecuteTestSession(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleInjectInputEvent(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleRuntimeGetCallStack(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleRuntimeReadProfiler(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);

CallToolResult handleEditorUndo(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleEditorRedo(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleEditorSaveScene(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleEditorReloadProject(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);

ToolRegistry& ToolRegistry::instance() {
    static ToolRegistry s_instance;
    return s_instance;
}

void ToolRegistry::registerTool(ToolDefinition tool) {
    std::string name = tool.name;
    m_tools[name] = std::move(tool);
    DIDI_LOG_DEBUG("TOOL_REG", "Registered tool: ", name);
}

const ToolDefinition* ToolRegistry::getTool(const std::string& name) const {
    auto it = m_tools.find(name);
    if (it != m_tools.end()) {
        return &it->second;
    }
    return nullptr;
}

std::vector<ToolDefinition> ToolRegistry::listTools() const {
    std::vector<ToolDefinition> list;
    list.reserve(m_tools.size());
    for (const auto& kv : m_tools) {
        list.push_back(kv.second);
    }
    return list;
}

CallToolResult ToolRegistry::callTool(const std::string& name, const json& arguments) {
    const auto* tool = getTool(name);
    if (!tool) {
        return CallToolResult::error("Tool not found: " + name);
    }
    if (!tool->handler) {
        return CallToolResult::error("Tool handler not set for: " + name);
    }
    try {
        return tool->handler(arguments);
    } catch (const std::exception& e) {
        DIDI_LOG_ERROR("TOOL_EXEC", "Exception calling tool '", name, "': ", e.what());
        return CallToolResult::error("Internal error executing tool: " + std::string(e.what()));
    }
}

void ToolRegistry::setIpcClient(std::shared_ptr<ipc::IIpcClient> ipc_client) {
    m_ipcClient = ipc_client;
}

std::shared_ptr<ipc::IIpcClient> ToolRegistry::getIpcClient() const {
    return m_ipcClient;
}

void ToolRegistry::registerAllDefaultTools() {
    // ==========================================
    // Domain 1: Scene Tree & Node Manipulation
    // ==========================================
    {
        ToolDefinition t;
        t.name = "scene_get_hierarchy";
        t.description = "Returns recursive node tree with node types, script attachments, and global/local transforms.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"root_path", {{"type", "string"}, {"default", "/root"}, {"description", "Node path or .tscn file path"}}},
                {"max_depth", {{"type", "integer"}, {"default", 10}, {"description", "Max tree depth"}}},
                {"include_properties", {{"type", "boolean"}, {"default", true}}},
                {"include_signals", {{"type", "boolean"}, {"default", true}}},
                {"include_scripts", {{"type", "boolean"}, {"default", true}}}
            }}
        };
        t.handler = [this](const json& args) { return handleGetSceneHierarchy(args, m_ipcClient); };
        registerTool(t);

        // Alias
        t.name = "get_scene_hierarchy";
        t.handler = [this](const json& args) { return handleGetSceneHierarchy(args, m_ipcClient); };
        registerTool(t);
    }
    {
        ToolDefinition t;
        t.name = "scene_instantiate_node";
        t.description = "Spawns built-in nodes or instantiates sub-scenes (.tscn) at a target NodePath.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"node_type", {{"type", "string"}, {"default", "Node3D"}, {"description", "Class name to instantiate"}}},
                {"scene_path", {{"type", "string"}, {"description", "Optional .tscn path"}}},
                {"parent_path", {{"type", "string"}, {"default", "/root"}}},
                {"name", {{"type", "string"}, {"description", "Node name"}}},
                {"properties", {{"type", "object"}, {"description", "Initial property values"}}}
            }}
        };
        t.handler = [this](const json& args) { return handleSceneInstantiateNode(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "scene_remove_node";
        t.description = "Deletes a node and safely frees references via queue_free() with UndoRedo.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"target_node", {{"type", "string"}, {"description", "NodePath of target node"}}}
            }},
            {"required", {"target_node"}}
        };
        t.handler = [this](const json& args) { return handleSceneRemoveNode(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "scene_reparent_node";
        t.description = "Moves a node to a new parent while preserving global transforms.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"target_node", {{"type", "string"}, {"description", "NodePath to reparent"}}},
                {"new_parent_path", {{"type", "string"}, {"description", "New parent NodePath"}}},
                {"keep_global_transform", {{"type", "boolean"}, {"default", true}}}
            }},
            {"required", {"target_node", "new_parent_path"}}
        };
        t.handler = [this](const json& args) { return handleSceneReparentNode(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "scene_set_property";
        t.description = "Dynamically mutates any exported or built-in node property with type-coerced Variant values.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"target_node", {{"type", "string"}, {"description", "Target NodePath"}}},
                {"property_name", {{"type", "string"}, {"description", "Property name"}}},
                {"value", {{"description", "New property value"}}}
            }},
            {"required", {"target_node", "property_name", "value"}}
        };
        t.handler = [this](const json& args) { return handleSceneSetProperty(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "scene_get_property";
        t.description = "Queries precise property values, metadata, and export hints.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"target_node", {{"type", "string"}, {"description", "Target NodePath"}}},
                {"property_name", {{"type", "string"}, {"description", "Property name"}}}
            }},
            {"required", {"target_node", "property_name"}}
        };
        t.handler = [this](const json& args) { return handleSceneGetProperty(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "scene_duplicate_node";
        t.description = "Duplicates an existing node branch with unique names.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"target_node", {{"type", "string"}, {"description", "Target NodePath to duplicate"}}}
            }},
            {"required", {"target_node"}}
        };
        t.handler = [this](const json& args) { return handleSceneDuplicateNode(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "mutate_scene_tree";
        t.description = "Adds, removes, reparents, duplicates, or edits nodes via UndoRedo transactions.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"action", {{"type", "string"}, {"enum", {"add", "remove", "modify", "reparent", "duplicate"}}}},
                {"target_node", {{"type", "string"}}},
                {"payload", {{"type", "object"}}}
            }},
            {"required", {"action", "target_node"}}
        };
        t.handler = [this](const json& args) { return handleMutateSceneTree(args, m_ipcClient); };
        registerTool(std::move(t));
    }

    // ==========================================
    // Domain 2: Signals & Event Wiring
    // ==========================================
    {
        ToolDefinition t;
        t.name = "signal_list_connections";
        t.description = "Lists all signals declared on a node, including incoming and outgoing connections.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"target_node", {{"type", "string"}, {"description", "Target NodePath"}}}
            }},
            {"required", {"target_node"}}
        };
        t.handler = [this](const json& args) { return handleSignalListConnections(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "signal_connect";
        t.description = "Binds a signal from an emitter node to a target method or callable.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"emitter_node", {{"type", "string"}, {"description", "Emitter NodePath"}}},
                {"signal_name", {{"type", "string"}, {"description", "Signal name"}}},
                {"target_node", {{"type", "string"}, {"description", "Receiver NodePath"}}},
                {"target_method", {{"type", "string"}, {"description", "Method name to call"}}}
            }},
            {"required", {"emitter_node", "signal_name", "target_node", "target_method"}}
        };
        t.handler = [this](const json& args) { return handleSignalConnect(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "signal_disconnect";
        t.description = "Unbinds existing signal connections.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"emitter_node", {{"type", "string"}, {"description", "Emitter NodePath"}}},
                {"signal_name", {{"type", "string"}, {"description", "Signal name"}}},
                {"target_node", {{"type", "string"}, {"description", "Receiver NodePath"}}},
                {"target_method", {{"type", "string"}, {"description", "Method name"}}}
            }},
            {"required", {"emitter_node", "signal_name", "target_node", "target_method"}}
        };
        t.handler = [this](const json& args) { return handleSignalDisconnect(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "signal_emit";
        t.description = "Emits a custom signal manually with arguments for event testing.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"target_node", {{"type", "string"}, {"description", "Emitter NodePath"}}},
                {"signal_name", {{"type", "string"}, {"description", "Signal name"}}},
                {"arguments", {{"type", "array"}, {"description", "Positional signal arguments"}}}
            }},
            {"required", {"target_node", "signal_name"}}
        };
        t.handler = [this](const json& args) { return handleSignalEmit(args, m_ipcClient); };
        registerTool(std::move(t));
    }

    // ==========================================
    // Domain 3: Scripting, Class Reflection & Diagnostics
    // ==========================================
    {
        ToolDefinition t;
        t.name = "script_check_syntax";
        t.description = "Runs Godot’s internal GDScript compiler to return compile errors, warnings, and byte-code validity.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"file_path", {{"type", "string"}, {"description", "Path to script file"}}},
                {"source_text", {{"type", "string"}, {"description", "Optional unsaved script buffer"}}}
            }}
        };
        t.handler = [this](const json& args) { return handleScriptCheckSyntax(args, m_ipcClient); };
        registerTool(t);

        // Alias
        t.name = "analyze_script_diagnostics";
        t.handler = [this](const json& args) { return handleScriptCheckSyntax(args, m_ipcClient); };
        registerTool(t);
    }
    {
        ToolDefinition t;
        t.name = "script_reflect_class";
        t.description = "Looks up engine documentation, methods, properties, and constants for any engine class.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"class_name", {{"type", "string"}, {"description", "Godot class name (e.g. CharacterBody3D)"}}}
            }},
            {"required", {"class_name"}}
        };
        t.handler = [this](const json& args) { return handleScriptReflectClass(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "script_get_symbols";
        t.description = "Extracts AST symbols, functions, signals, and typed variables from any script file.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"file_path", {{"type", "string"}, {"description", "Path to script"}}},
                {"source_text", {{"type", "string"}, {"description", "Optional source code"}}}
            }}
        };
        t.handler = [this](const json& args) { return handleScriptGetSymbols(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "script_patch_method";
        t.description = "Safely rewrites a single method or symbol body in a .gd file without touching other functions.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"file_path", {{"type", "string"}, {"description", "Target script path"}}},
                {"method_name", {{"type", "string"}, {"description", "Method name to replace"}}},
                {"new_definition", {{"type", "string"}, {"description", "New method implementation"}}},
                {"symbol_type", {{"type", "string"}, {"default", "function"}}}
            }},
            {"required", {"file_path", "method_name", "new_definition"}}
        };
        t.handler = [this](const json& args) { return handleScriptPatchMethod(args, m_ipcClient); };
        registerTool(t);

        // Alias
        t.name = "patch_script_symbols";
        t.handler = [this](const json& args) { return handleScriptPatchMethod(args, m_ipcClient); };
        registerTool(t);
    }

    // ==========================================
    // Domain 4: Visual Verification & Viewport Rendering
    // ==========================================
    {
        ToolDefinition t;
        t.name = "viewport_capture_frame";
        t.description = "Captures Base64 PNG snapshots from active 2D/3D viewports or designated camera nodes.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"camera_identifier", {{"type", "string"}, {"default", "active_editor_view"}}},
                {"resolution", {{"type", "object"}, {"default", {{"width", 1024}, {"height", 768}}}}},
                {"render_debug_flags", {{"type", "array"}}},
                {"node_isolation_path", {{"type", "string"}}}
            }}
        };
        t.handler = [this](const json& args) { return handleCaptureViewport(args, m_ipcClient); };
        registerTool(t);

        // Alias
        t.name = "capture_viewport";
        t.handler = [this](const json& args) { return handleCaptureViewport(args, m_ipcClient); };
        registerTool(t);
    }
    {
        ToolDefinition t;
        t.name = "viewport_set_camera_transform";
        t.description = "Positions and rotates the editor or test camera to inspect specific coordinates.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"position", {{"type", "object"}, {"description", "Vector3 {x, y, z}"}}},
                {"rotation", {{"type", "object"}, {"description", "Vector3 {x, y, z} in degrees"}}},
                {"fov", {{"type", "number"}, {"default", 75.0}}}
            }},
            {"required", {"position"}}
        };
        t.handler = [this](const json& args) { return handleViewportSetCameraTransform(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "viewport_create_test_lab";
        t.description = "Generates an isolated test stage with neutral lighting, grid plane, and multi-angle cameras.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"target_resource_path", {{"type", "string"}}},
                {"environment", {{"type", "string"}, {"default", "studio_neutral"}}},
                {"orthographic", {{"type", "boolean"}, {"default", false}}},
                {"camera_rig", {{"type", "array"}, {"default", {"front", "back", "left", "right"}}}}
            }},
            {"required", {"target_resource_path"}}
        };
        t.handler = [this](const json& args) { return handleCreateVisualTestLab(args, m_ipcClient); };
        registerTool(t);

        // Alias
        t.name = "create_visual_test_lab";
        t.handler = [this](const json& args) { return handleCreateVisualTestLab(args, m_ipcClient); };
        registerTool(t);
    }
    {
        ToolDefinition t;
        t.name = "viewport_toggle_debug_draw";
        t.description = "Toggles collision wireframes, navigation meshes, normal vectors, and lighting modes.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"collision_shapes", {{"type", "boolean"}, {"default", true}}},
                {"navigation_mesh", {{"type", "boolean"}, {"default", false}}},
                {"wireframe", {{"type", "boolean"}, {"default", false}}}
            }}
        };
        t.handler = [this](const json& args) { return handleViewportToggleDebugDraw(args, m_ipcClient); };
        registerTool(std::move(t));
    }

    // ==========================================
    // Domain 5: Physics, Animation & Navigation
    // ==========================================
    {
        ToolDefinition t;
        t.name = "physics_raycast_query";
        t.description = "Fires a 2D/3D physics raycast to check line-of-sight, ray hits, and collision masks.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"from", {{"type", "object"}, {"description", "Ray start position"}}},
                {"to", {{"type", "object"}, {"description", "Ray end position"}}},
                {"collision_mask", {{"type", "integer"}, {"default", 1}}}
            }},
            {"required", {"from", "to"}}
        };
        t.handler = [this](const json& args) { return handlePhysicsRaycastQuery(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "physics_simulate_step";
        t.description = "Advances the physics engine by N ticks to test gravity, velocity, or collision response deterministically.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"steps", {{"type", "integer"}, {"default", 1}}},
                {"delta", {{"type", "number"}, {"default", 0.0166667}}}
            }}
        };
        t.handler = [this](const json& args) { return handlePhysicsSimulateStep(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "nav_bake_mesh";
        t.description = "Triggers runtime or editor navigation mesh baking (NavigationMesh / NavigationPolygon).";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"nav_node_path", {{"type", "string"}, {"description", "Path to NavigationRegion3D / NavigationMesh"}}}
            }}
        };
        t.handler = [this](const json& args) { return handleNavBakeMesh(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "nav_query_path";
        t.description = "Tests pathfinding between two points to verify walkable navmeshes.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"start_point", {{"type", "object"}, {"description", "Vector3 start"}}},
                {"end_point", {{"type", "object"}, {"description", "Vector3 target"}}}
            }},
            {"required", {"start_point", "end_point"}}
        };
        t.handler = [this](const json& args) { return handleNavQueryPath(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "anim_list_tracks";
        t.description = "Lists animations, keyframes, and blend trees in an AnimationPlayer or AnimationTree.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"animation_player_path", {{"type", "string"}, {"description", "Path to AnimationPlayer"}}}
            }},
            {"required", {"animation_player_path"}}
        };
        t.handler = [this](const json& args) { return handleAnimListTracks(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "anim_play_track";
        t.description = "Plays a specific animation keyframe sequence to verify transitions.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"animation_player_path", {{"type", "string"}}},
                {"animation_name", {{"type", "string"}}},
                {"custom_speed", {{"type", "number"}, {"default", 1.0}}}
            }},
            {"required", {"animation_player_path", "animation_name"}}
        };
        t.handler = [this](const json& args) { return handleAnimPlayTrack(args, m_ipcClient); };
        registerTool(std::move(t));
    }

    // ==========================================
    // Domain 6: Tilemaps, GridMaps & Procedural Generation
    // ==========================================
    {
        ToolDefinition t;
        t.name = "tilemap_set_cells";
        t.description = "Batch updates 2D TileMapLayer cells with source IDs, atlas coordinates, and alternate tiles.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"tilemap_path", {{"type", "string"}}},
                {"cells", {{"type", "array"}, {"description", "Array of {coords: [x, y], source_id: int, atlas_coords: [x, y]}"}}}
            }},
            {"required", {"tilemap_path", "cells"}}
        };
        t.handler = [this](const json& args) { return handleTilemapSetCells(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "tilemap_get_used_rect";
        t.description = "Returns used cell boundaries and layer structures.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"tilemap_path", {{"type", "string"}}}
            }},
            {"required", {"tilemap_path"}}
        };
        t.handler = [this](const json& args) { return handleTilemapGetUsedRect(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "gridmap_set_cells";
        t.description = "Places 3D mesh library tiles inside a GridMap with coordinate orientations.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"gridmap_path", {{"type", "string"}}},
                {"cells", {{"type", "array"}, {"description", "Array of {position: [x, y, z], item: int, orientation: int}"}}}
            }},
            {"required", {"gridmap_path", "cells"}}
        };
        t.handler = [this](const json& args) { return handleGridmapSetCells(args, m_ipcClient); };
        registerTool(std::move(t));
    }

    // ==========================================
    // Domain 7: Resources & Project File Management
    // ==========================================
    {
        ToolDefinition t;
        t.name = "resource_create";
        t.description = "Generates new resource instances (StandardMaterial3D, AudioStreamRandomizer, Curve3D, Shape3D).";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"resource_type", {{"type", "string"}, {"default", "StandardMaterial3D"}}},
                {"save_path", {{"type", "string"}, {"description", "Target res:// path"}}},
                {"properties", {{"type", "object"}}}
            }},
            {"required", {"save_path"}}
        };
        t.handler = [this](const json& args) { return handleResourceCreate(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "resource_inspect";
        t.description = "Reads inner resource properties and dependent UIDs.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"resource_path", {{"type", "string"}}}
            }},
            {"required", {"resource_path"}}
        };
        t.handler = [this](const json& args) { return handleResourceInspect(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "project_list_resources";
        t.description = "Scans res:// for assets filtered by type (e.g., .glb, .png, .tres).";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"search_path", {{"type", "string"}, {"default", "res://"}}},
                {"type_filter", {{"type", "string"}}},
                {"fuzzy_query", {{"type", "string"}}},
                {"include_uid", {{"type", "boolean"}, {"default", true}}}
            }}
        };
        t.handler = [this](const json& args) { return handleQueryProjectResources(args, m_ipcClient); };
        registerTool(t);

        // Alias
        t.name = "query_project_resources";
        t.handler = [this](const json& args) { return handleQueryProjectResources(args, m_ipcClient); };
        registerTool(t);
    }
    {
        ToolDefinition t;
        t.name = "project_get_uid_map";
        t.description = "Resolves uid:// references to local filesystem paths.";
        t.inputSchema = {{"type", "object"}};
        t.handler = [this](const json& args) { return handleProjectGetUidMap(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "instantiate_asset";
        t.description = "Creates an instance of a resource or scene and parents it with automatic collision assignment.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"asset_path", {{"type", "string"}}},
                {"parent_path", {{"type", "string"}, {"default", "/root"}}},
                {"transform", {{"type", "object"}}},
                {"collision_mode", {{"type", "string"}, {"default", "none"}}}
            }},
            {"required", {"asset_path"}}
        };
        t.handler = [this](const json& args) { return handleInstantiateAsset(args, m_ipcClient); };
        registerTool(std::move(t));
    }

    // ==========================================
    // Domain 8: Execution, Input Injection & Debugging
    // ==========================================
    {
        ToolDefinition t;
        t.name = "runtime_launch";
        t.description = "Starts a specific scene or test runner with custom CLI flags (--debug, --headless).";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"scene_path", {{"type", "string"}}},
                {"timeout_seconds", {{"type", "integer"}, {"default", 10}}},
                {"headless", {{"type", "boolean"}, {"default", true}}},
                {"break_on_error", {{"type", "boolean"}, {"default", true}}},
                {"extra_args", {{"type", "array"}}}
            }}
        };
        t.handler = [this](const json& args) { return handleExecuteTestSession(args, m_ipcClient); };
        registerTool(t);

        // Alias
        t.name = "execute_test_session";
        t.handler = [this](const json& args) { return handleExecuteTestSession(args, m_ipcClient); };
        registerTool(t);
    }
    {
        ToolDefinition t;
        t.name = "runtime_inject_input";
        t.description = "Synthesizes InputEventKey, InputEventMouseButton, or InputEventAction to simulate gameplay.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"event_type", {{"type", "string"}, {"default", "action"}}},
                {"action_name", {{"type", "string"}}},
                {"key_code", {{"type", "string"}}},
                {"pressed", {{"type", "boolean"}, {"default", true}}},
                {"strength", {{"type", "number"}, {"default", 1.0}}},
                {"duration_ms", {{"type", "integer"}, {"default", 100}}}
            }},
            {"required", {"event_type"}}
        };
        t.handler = [this](const json& args) { return handleInjectInputEvent(args, m_ipcClient); };
        registerTool(t);

        // Alias
        t.name = "inject_input_event";
        t.handler = [this](const json& args) { return handleInjectInputEvent(args, m_ipcClient); };
        registerTool(t);
    }
    {
        ToolDefinition t;
        t.name = "runtime_get_call_stack";
        t.description = "Fetches current debugger call stack and variable scopes on engine break/crash.";
        t.inputSchema = {{"type", "object"}};
        t.handler = [this](const json& args) { return handleRuntimeGetCallStack(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "runtime_read_profiler";
        t.description = "Pulls frame times, draw calls, draw passes, and physics tick metrics.";
        t.inputSchema = {{"type", "object"}};
        t.handler = [this](const json& args) { return handleRuntimeReadProfiler(args, m_ipcClient); };
        registerTool(std::move(t));
    }

    // ==========================================
    // Domain 9: Editor Lifecycle & Undo/Redo
    // ==========================================
    {
        ToolDefinition t;
        t.name = "editor_undo";
        t.description = "Reverts the last operation through Godot's EditorUndoRedoManager.";
        t.inputSchema = {{"type", "object"}};
        t.handler = [this](const json& args) { return handleEditorUndo(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "editor_redo";
        t.description = "Replays the previously reverted editor transaction.";
        t.inputSchema = {{"type", "object"}};
        t.handler = [this](const json& args) { return handleEditorRedo(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "editor_save_scene";
        t.description = "Saves the active scene to disk.";
        t.inputSchema = {{"type", "object"}};
        t.handler = [this](const json& args) { return handleEditorSaveScene(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "editor_reload_project";
        t.description = "Reloads all modified scripts and rescans project resources.";
        t.inputSchema = {{"type", "object"}};
        t.handler = [this](const json& args) { return handleEditorReloadProject(args, m_ipcClient); };
        registerTool(std::move(t));
    }
}

} // namespace mcp
} // namespace didi
