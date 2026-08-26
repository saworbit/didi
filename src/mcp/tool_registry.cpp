#include "didi/mcp/tool_registry.hpp"
#include "didi/common/logger.hpp"

namespace didi {
namespace mcp {

// External declarations for tool implementation handlers
CallToolResult handleCaptureViewport(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleCreateVisualTestLab(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleGetSceneHierarchy(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleMutateSceneTree(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleAnalyzeScriptDiagnostics(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handlePatchScriptSymbols(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleExecuteTestSession(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleInjectInputEvent(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleQueryProjectResources(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleInstantiateAsset(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);

ToolRegistry& ToolRegistry::instance() {
    static ToolRegistry s_instance;
    return s_instance;
}

void ToolRegistry::registerTool(ToolDefinition tool) {
    m_tools[tool.name] = std::move(tool);
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
    for (const auto& [name, t] : m_tools) {
        list.push_back(t);
    }
    return list;
}

CallToolResult ToolRegistry::callTool(const std::string& name, const json& arguments) {
    auto tool = getTool(name);
    if (!tool) {
        return CallToolResult::error("Unknown tool: " + name);
    }
    try {
        return tool->handler(arguments);
    } catch (const std::exception& e) {
        return CallToolResult::error("Exception executing tool '" + name + "': " + e.what());
    }
}

void ToolRegistry::setIpcClient(std::shared_ptr<ipc::IIpcClient> ipc_client) {
    m_ipcClient = ipc_client;
}

std::shared_ptr<ipc::IIpcClient> ToolRegistry::getIpcClient() const {
    return m_ipcClient;
}

void ToolRegistry::registerAllDefaultTools() {
    // 1. capture_viewport
    ToolDefinition capture_viewport;
    capture_viewport.name = "capture_viewport";
    capture_viewport.description = "Renders a live editor/game viewport or isolated node to PNG base64 for spatial and visual verification.";
    capture_viewport.inputSchema = {
        {"type", "object"},
        {"properties", {
            {"camera_identifier", {
                {"type", "string"},
                {"description", "Path to camera node or preset identifier (e.g., 'active_editor_view', 'lab_camera_front', 'lab_camera_top')"},
                {"default", "active_editor_view"}
            }},
            {"resolution", {
                {"type", "object"},
                {"properties", {
                    {"width", {{"type", "integer"}, {"default", 1024}}},
                    {"height", {{"type", "integer"}, {"default", 768}}}
                }},
                {"default", {{"width", 1024}, {"height", 768}}}
            }},
            {"render_debug_flags", {
                {"type", "array"},
                {"items", {
                    {"type", "string"},
                    {"enum", {"wireframe", "collision_shapes", "normals", "lighting_only", "unshaded"}}
                }},
                {"description", "Optional debug visualization rendering modes"}
            }},
            {"node_isolation_path", {
                {"type", "string"},
                {"description", "Optional node path to isolate in rendering (hiding all other scene elements)"}
            }}
        }}
    };
    capture_viewport.handler = [this](const json& args) {
        return handleCaptureViewport(args, m_ipcClient);
    };
    registerTool(std::move(capture_viewport));

    // 2. create_visual_test_lab
    ToolDefinition visual_test_lab;
    visual_test_lab.name = "create_visual_test_lab";
    visual_test_lab.description = "Spawns a temporary, isolated 3D/2D sandbox scene with lighting, ground plane, and multi-angle test cameras for asset inspection.";
    visual_test_lab.inputSchema = {
        {"type", "object"},
        {"properties", {
            {"target_resource_path", {
                {"type", "string"},
                {"description", "Resource path to inspect, e.g., 'res://models/character.glb' or 'res://scenes/player.tscn'"}
            }},
            {"environment", {
                {"type", "string"},
                {"enum", {"studio_neutral", "dark_grid", "outdoor_sun"}},
                {"default", "studio_neutral"},
                {"description", "Lighting & backdrop environment preset"}
            }},
            {"orthographic", {
                {"type", "boolean"},
                {"default", false},
                {"description", "Whether camera rig should use orthographic projection"}
            }},
            {"camera_rig", {
                {"type", "array"},
                {"items", {
                    {"type", "string"},
                    {"enum", {"front", "back", "left", "right", "top", "isometric"}}
                }},
                {"default", {"front", "back", "left", "right"}},
                {"description", "List of camera angles to generate in the test lab"}
            }}
        }},
        {"required", {"target_resource_path"}}
    };
    visual_test_lab.handler = [this](const json& args) {
        return handleCreateVisualTestLab(args, m_ipcClient);
    };
    registerTool(std::move(visual_test_lab));

    // 3. get_scene_hierarchy
    ToolDefinition get_scene_hierarchy;
    get_scene_hierarchy.name = "get_scene_hierarchy";
    get_scene_hierarchy.description = "Returns the active scene tree with node types, transforms, script bindings, signals, and properties.";
    get_scene_hierarchy.inputSchema = {
        {"type", "object"},
        {"properties", {
            {"root_path", {
                {"type", "string"},
                {"default", "/root"},
                {"description", "Node path to start tree traversal from"}
            }},
            {"max_depth", {
                {"type", "integer"},
                {"default", 10},
                {"description", "Maximum recursion depth for scene tree traversal"}
            }},
            {"include_properties", {
                {"type", "boolean"},
                {"default", true},
                {"description", "Whether to include node properties and transform data"}
            }},
            {"include_signals", {
                {"type", "boolean"},
                {"default", true},
                {"description", "Whether to include signal connection definitions"}
            }},
            {"include_scripts", {
                {"type", "boolean"},
                {"default", true},
                {"description", "Whether to include attached script paths and methods"}
            }}
        }}
    };
    get_scene_hierarchy.handler = [this](const json& args) {
        return handleGetSceneHierarchy(args, m_ipcClient);
    };
    registerTool(std::move(get_scene_hierarchy));

    // 4. mutate_scene_tree
    ToolDefinition mutate_scene_tree;
    mutate_scene_tree.name = "mutate_scene_tree";
    mutate_scene_tree.description = "Adds, removes, reparents, duplicates, or edits nodes with UndoRedo transaction support.";
    mutate_scene_tree.inputSchema = {
        {"type", "object"},
        {"properties", {
            {"action", {
                {"type", "string"},
                {"enum", {"add", "remove", "modify", "reparent", "duplicate"}},
                {"description", "Type of scene mutation to execute"}
            }},
            {"target_node", {
                {"type", "string"},
                {"description", "Target node path to modify, remove, reparent, or parent a new node under"}
            }},
            {"payload", {
                {"type", "object"},
                {"description", "Parameters for action: node_type, name, properties, script_path, new_parent_path, transform"}
            }}
        }},
        {"required", {"action", "target_node"}}
    };
    mutate_scene_tree.handler = [this](const json& args) {
        return handleMutateSceneTree(args, m_ipcClient);
    };
    registerTool(std::move(mutate_scene_tree));

    // 5. analyze_script_diagnostics
    ToolDefinition analyze_script_diagnostics;
    analyze_script_diagnostics.name = "analyze_script_diagnostics";
    analyze_script_diagnostics.description = "Evaluates GDScript/C# files for compilation errors, warnings, unresolved symbols, and type mismatches.";
    analyze_script_diagnostics.inputSchema = {
        {"type", "object"},
        {"properties", {
            {"file_path", {
                {"type", "string"},
                {"description", "Path to script file (e.g. 'res://scripts/player.gd')"}
            }},
            {"source_text", {
                {"type", "string"},
                {"description", "Optional unsaved script content to analyze instead of disk file"}
            }}
        }},
        {"required", {"file_path"}}
    };
    analyze_script_diagnostics.handler = [this](const json& args) {
        return handleAnalyzeScriptDiagnostics(args, m_ipcClient);
    };
    registerTool(std::move(analyze_script_diagnostics));

    // 6. patch_script_symbols
    ToolDefinition patch_script_symbols;
    patch_script_symbols.name = "patch_script_symbols";
    patch_script_symbols.description = "Replaces or inserts specific functions, variables, or signal bindings cleanly without touching the rest of the file.";
    patch_script_symbols.inputSchema = {
        {"type", "object"},
        {"properties", {
            {"file_path", {
                {"type", "string"},
                {"description", "Path to script file, e.g. 'res://scripts/player.gd'"}
            }},
            {"symbol_name", {
                {"type", "string"},
                {"description", "Name of function, variable, or signal to replace"}
            }},
            {"new_definition", {
                {"type", "string"},
                {"description", "New code block for the symbol"}
            }},
            {"symbol_type", {
                {"type", "string"},
                {"enum", {"function", "variable", "signal", "enum", "class"}},
                {"default", "function"},
                {"description", "Type of symbol to patch"}
            }}
        }},
        {"required", {"file_path", "symbol_name", "new_definition"}}
    };
    patch_script_symbols.handler = [this](const json& args) {
        return handlePatchScriptSymbols(args, m_ipcClient);
    };
    registerTool(std::move(patch_script_symbols));

    // 7. execute_test_session
    ToolDefinition execute_test_session;
    execute_test_session.name = "execute_test_session";
    execute_test_session.description = "Boots the project or scene in headless or windowed mode with structured capture of engine stdout, warnings, and stack traces.";
    execute_test_session.inputSchema = {
        {"type", "object"},
        {"properties", {
            {"scene_path", {
                {"type", "string"},
                {"description", "Path to scene or script to execute (e.g. 'res://scenes/test_arena.tscn')"}
            }},
            {"timeout_seconds", {
                {"type", "integer"},
                {"default", 10},
                {"description", "Timeout limit before terminating test session"}
            }},
            {"headless", {
                {"type", "boolean"},
                {"default", true},
                {"description", "Run engine in --headless mode"}
            }},
            {"break_on_error", {
                {"type", "boolean"},
                {"default", true},
                {"description", "Abort session immediately if engine error/assertion is detected"}
            }},
            {"extra_args", {
                {"type", "array"},
                {"items", {{"type", "string"}}},
                {"description", "Additional command-line flags to pass to Godot"}
            }}
        }}
    };
    execute_test_session.handler = [this](const json& args) {
        return handleExecuteTestSession(args, m_ipcClient);
    };
    registerTool(std::move(execute_test_session));

    // 8. inject_input_event
    ToolDefinition inject_input_event;
    inject_input_event.name = "inject_input_event";
    inject_input_event.description = "Emulates mouse, keyboard, gamepad, or action events into the running game instance to test gameplay mechanics.";
    inject_input_event.inputSchema = {
        {"type", "object"},
        {"properties", {
            {"event_type", {
                {"type", "string"},
                {"enum", {"action", "key", "mouse_button", "mouse_motion", "gamepad"}},
                {"description", "Type of input event to emulate"}
            }},
            {"action_name", {
                {"type", "string"},
                {"description", "InputMap action name (e.g. 'ui_accept', 'move_left', 'jump')"}
            }},
            {"key_code", {
                {"type", "string"},
                {"description", "Key code string (e.g. 'KEY_SPACE', 'KEY_W')"}
            }},
            {"pressed", {
                {"type", "boolean"},
                {"default", true},
                {"description", "Whether key/action/button is pressed or released"}
            }},
            {"strength", {
                {"type", "number"},
                {"default", 1.0},
                {"description", "Input strength for analog/action triggers (0.0 to 1.0)"}
            }},
            {"position", {
                {"type", "object"},
                {"properties", {
                    {"x", {{"type", "number"}}},
                    {"y", {{"type", "number"}}}
                }},
                {"description", "Mouse coordinate position"}
            }},
            {"duration_ms", {
                {"type", "integer"},
                {"default", 100},
                {"description", "Hold duration in milliseconds"}
            }}
        }},
        {"required", {"event_type"}}
    };
    inject_input_event.handler = [this](const json& args) {
        return handleInjectInputEvent(args, m_ipcClient);
    };
    registerTool(std::move(inject_input_event));

    // 9. query_project_resources
    ToolDefinition query_project_resources;
    query_project_resources.name = "query_project_resources";
    query_project_resources.description = "Scans the res:// filesystem for textures, meshes, sounds, shaders, and metadata with UID resolution.";
    query_project_resources.inputSchema = {
        {"type", "object"},
        {"properties", {
            {"search_path", {
                {"type", "string"},
                {"default", "res://"},
                {"description", "Directory path to search"}
            }},
            {"type_filter", {
                {"type", "string"},
                {"description", "Resource type filter (e.g. 'PackedScene', 'Texture2D', 'StandardMaterial3D', 'AudioStream', 'Script')"}
            }},
            {"fuzzy_query", {
                {"type", "string"},
                {"description", "Search query keyword to filter resource names or paths"}
            }},
            {"include_uid", {
                {"type", "boolean"},
                {"default", true},
                {"description", "Whether to resolve and include Godot 4 UID strings"}
            }}
        }}
    };
    query_project_resources.handler = [this](const json& args) {
        return handleQueryProjectResources(args, m_ipcClient);
    };
    registerTool(std::move(query_project_resources));

    // 10. instantiate_asset
    ToolDefinition instantiate_asset;
    instantiate_asset.name = "instantiate_asset";
    instantiate_asset.description = "Creates an instance of a resource or scene and parents it with automatic collision and transform assignment.";
    instantiate_asset.inputSchema = {
        {"type", "object"},
        {"properties", {
            {"asset_path", {
                {"type", "string"},
                {"description", "Path to scene, mesh, or resource (e.g. 'res://models/tree.glb', 'res://scenes/enemy.tscn')"}
            }},
            {"parent_path", {
                {"type", "string"},
                {"default", "/root"},
                {"description", "Target parent node path to attach instance to"}
            }},
            {"transform", {
                {"type", "object"},
                {"properties", {
                    {"position", {{"type", "object"}, {"properties", {{"x", {{"type", "number"}}}, {"y", {{"type", "number"}}}, {"z", {{"type", "number"}}}}}}},
                    {"rotation", {{"type", "object"}, {"properties", {{"x", {{"type", "number"}}}, {"y", {{"type", "number"}}}, {"z", {{"type", "number"}}}}}}},
                    {"scale", {{"type", "object"}, {"properties", {{"x", {{"type", "number"}}}, {"y", {{"type", "number"}}}, {"z", {{"type", "number"}}}}}}}
                }},
                {"description", "Spatial transform to assign"}
            }},
            {"collision_mode", {
                {"type", "string"},
                {"enum", {"none", "auto_trimesh", "auto_convex", "box_bounds"}},
                {"default", "none"},
                {"description", "Automatic collision generation for 3D meshes"}
            }}
        }},
        {"required", {"asset_path"}}
    };
    instantiate_asset.handler = [this](const json& args) {
        return handleInstantiateAsset(args, m_ipcClient);
    };
    registerTool(std::move(instantiate_asset));
}

} // namespace mcp
} // namespace didi
