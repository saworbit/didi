#include "didi/mcp/mcp_protocol.hpp"
#include "didi/tools/phase7_live_forward.hpp"
#include "didi/common/ipc_channel.hpp"
#include "didi/common/logger.hpp"
#include "didi/mcp/tool_registration.hpp"

namespace didi {
namespace mcp {

CallToolResult handlePhysicsRaycastQuery(const ResolvedToolBinding& binding, const json& args,
                         std::shared_ptr<ipc::IIpcClient> ipc) {
    return sendPhase7LiveRequest(binding, args, ipc);
}

CallToolResult handleSpatialQueryRaycastBatch(const ResolvedToolBinding& binding, const json& args,
                         std::shared_ptr<ipc::IIpcClient> ipc) {
    return sendPhase7LiveRequest(binding, args, ipc);
}

CallToolResult handleSpatialQueryClearance(const ResolvedToolBinding& binding, const json& args,
                         std::shared_ptr<ipc::IIpcClient> ipc) {
    return sendPhase7LiveRequest(binding, args, ipc);
}

CallToolResult handleSpatialQueryFrustum(const ResolvedToolBinding& binding, const json& args,
                         std::shared_ptr<ipc::IIpcClient> ipc) {
    return sendPhase7LiveRequest(binding, args, ipc);
}

CallToolResult handlePhysicsSimulateStep(const ResolvedToolBinding& binding, const json& args,
                         std::shared_ptr<ipc::IIpcClient> ipc) {
    return sendPhase7LiveRequest(binding, args, ipc);
}

CallToolResult handleNavBakeMesh(const ResolvedToolBinding& binding, const json& args,
                         std::shared_ptr<ipc::IIpcClient> ipc) {
    return sendPhase7LiveRequest(binding, args, ipc);
}

CallToolResult handleNavQueryPath(const ResolvedToolBinding& binding, const json& args,
                         std::shared_ptr<ipc::IIpcClient> ipc) {
    return sendPhase7LiveRequest(binding, args, ipc);
}

CallToolResult handleAnimListTracks(const ResolvedToolBinding& binding, const json& args,
                         std::shared_ptr<ipc::IIpcClient> ipc) {
    return sendPhase7LiveRequest(binding, args, ipc);
}

CallToolResult handleAnimPlayTrack(const ResolvedToolBinding& binding, const json& args,
                         std::shared_ptr<ipc::IIpcClient> ipc) {
    return sendPhase7LiveRequest(binding, args, ipc);
}

CallToolResult handleAnimAddLibrary(const ResolvedToolBinding& binding, const json& args,
                                    std::shared_ptr<ipc::IIpcClient> ipc) {
    return sendPhase7LiveRequest(binding, args, ipc);
}

// The tools whose handlers this file holds. registerAllDefaultTools calls
// each domain's in turn (#1256).
void ToolRegistry::registerPhysicsNavTools() {
    {
        ToolDefinition t;
        t.name = "spatial_query_clearance";
        t.description = "Sweeps a box, sphere, or capsule along a path in the attached session's physics world and reports how far it gets, which is the question a doorway or a spawn point asks and a ray cannot answer.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"shape", {
                    {"type", "object"},
                    {"description", "The body being fitted through. sphere is a circle in 2D."},
                    {"properties", {
                        {"kind", {{"type", "string"}, {"enum", json::array({"box", "sphere", "capsule"})}}},
                        {"size", {{"type", "object"}, {"description", "box only: {x,y} or {x,y,z}, every component greater than 0."}}},
                        {"radius", {{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 100000}, {"description", "sphere and capsule only."}}},
                        {"height", {{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 100000}, {"description", "capsule only."}}}
                    }},
                    {"required", json::array({"kind"})},
                    {"additionalProperties", false}
                }},
                {"from", {{"type", "object"}, {"description", "{x,y} for 2D or {x,y,z} for 3D."}}},
                {"to", {{"type", "object"}, {"description", "Where the shape is sweeping to. Equal to from asks whether it fits where it stands."}}},
                {"collision_mask", {{"type", "integer"}, {"minimum", 1}, {"maximum", 4294967295}, {"default", 1}}}
            }},
            {"required", json::array({"shape", "from", "to"})},
            {"additionalProperties", false}
        };
        t.boundHandler = [this](const ResolvedToolBinding& binding, const json& args) {
            return handleSpatialQueryClearance(binding, args, m_ipcClient);
        };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "spatial_query_frustum";
        t.description = "Lists the 3D nodes inside a camera frustum in the attached session, nearest first, and can sample whether anything with a collider stands between the camera and each one.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"camera_node", {{"type", "string"}, {"minLength", 1}, {"maxLength", 1024},
                                 {"description", "A Camera3D already in the scene, whose transform, projection, near and far planes are used. Exactly one of camera_node and camera is required."}}},
                {"camera", {
                    {"type", "object"},
                    {"description", "A frustum written out by hand. fov_degrees is vertical, matching a Godot camera's default."},
                    {"properties", {
                        {"position", {{"type", "object"}, {"description", "{x,y,z}. A frustum has no 2D form."}}},
                        {"look_at", {{"type", "object"}, {"description", "{x,y,z} the camera points at, which must differ from position."}}},
                        {"up", {{"type", "object"}, {"description", "{x,y,z}. Defaults to {0,1,0}, and the value used is reported back."}}},
                        {"fov_degrees", {{"type", "number"}, {"minimum", 1}, {"maximum", 179}}},
                        {"near", {{"type", "number"}, {"exclusiveMinimum", 0}}},
                        {"far", {{"type", "number"}, {"exclusiveMinimum", 0}}},
                        {"aspect", {{"type", "number"}, {"minimum", 0.01}, {"maximum", 100},
                                    {"description", "Width over height. Required, because the frustum is a different shape without it."}}}
                    }},
                    {"required", json::array({"position", "look_at", "fov_degrees", "near", "far", "aspect"})},
                    {"additionalProperties", false}
                }},
                {"sightline", {{"type", "boolean"}, {"default", false},
                               {"description", "Sample rays from the camera to each node. Rays see physics colliders only, so geometry without one does not block."}}},
                {"collision_mask", {{"type", "integer"}, {"minimum", 1}, {"maximum", 4294967295}, {"default", 1},
                                    {"description", "Applies to the sightline rays only."}}},
                {"max_results", {{"type", "integer"}, {"minimum", 1}, {"maximum", 256}, {"default", 64}}}
            }},
            {"additionalProperties", false}
        };
        t.boundHandler = [this](const ResolvedToolBinding& binding, const json& args) {
            return handleSpatialQueryFrustum(binding, args, m_ipcClient);
        };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "spatial_query_raycast_batch";
        t.description = "Casts many rays against the attached session's physics world in one dispatch, sharing one space state, and returns the same hit record per ray that physics_raycast_query returns.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"rays", {
                    {"type", "array"}, {"minItems", 1}, {"maxItems", 64},
                    {"description", "Rays to cast. Every ray shares one dimension, because a 2D and a 3D ray are answered by different space states."},
                    {"items", {
                        {"type", "object"},
                        {"properties", {
                            {"from", {{"type", "object"}, {"description", "{x,y} for 2D or {x,y,z} for 3D."}}},
                            {"to", {{"type", "object"}, {"description", "{x,y} for 2D or {x,y,z} for 3D."}}},
                            {"collision_mask", {{"type", "integer"}, {"minimum", 1}, {"maximum", 4294967295}, {"default", 1}}}
                        }},
                        {"required", json::array({"from", "to"})},
                        {"additionalProperties", false}
                    }}
                }}
            }},
            {"required", json::array({"rays"})},
            {"additionalProperties", false}
        };
        t.boundHandler = [this](const ResolvedToolBinding& binding, const json& args) {
            return handleSpatialQueryRaycastBatch(binding, args, m_ipcClient);
        };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "physics_raycast_query";
        t.description = "Fires a 2D/3D physics raycast to check line-of-sight, ray hits, and collision masks.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"from", {{"type", "object"}, {"description", "Ray start position"}}},
                {"to", {{"type", "object"}, {"description", "Ray end position"}}},
                {"collision_mask", {{"type", "integer"}, {"minimum", 1}, {"maximum", 4294967295}, {"default", 1}}}
            }},
            {"required", {"from", "to"}}
        };
        t.boundHandler = [this](const ResolvedToolBinding& binding, const json& args) {
            return handlePhysicsRaycastQuery(binding, args, m_ipcClient);
        };
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
        t.boundHandler = [this](const ResolvedToolBinding& binding, const json& args) {
            return handlePhysicsSimulateStep(binding, args, m_ipcClient);
        };
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
        t.boundHandler = [this](const ResolvedToolBinding& binding, const json& args) {
            return handleNavBakeMesh(binding, args, m_ipcClient);
        };
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
        t.boundHandler = [this](const ResolvedToolBinding& binding, const json& args) {
            return handleNavQueryPath(binding, args, m_ipcClient);
        };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "anim_list_tracks";
        t.description = "Lists the animations an AnimationPlayer holds, by the names anim_play_track takes, with each one's length, loop mode and tracks (type, node path and key times). Reads the edited scene in an editor and the running tree in a game. An AnimationTree is refused.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"animation_player_path", {{"type", "string"}, {"description", "Path to AnimationPlayer"}}}
            }},
            {"required", {"animation_player_path"}}
        };
        t.boundHandler = [this](const ResolvedToolBinding& binding, const json& args) {
            return handleAnimListTracks(binding, args, m_ipcClient);
        };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "anim_play_track";
        t.description = "Plays one animation on an AnimationPlayer in a running game and reports whether it is playing. Game sessions only. Name the animation the way anim_list_tracks or anim_add_library reports it: library/animation, or the bare name for the default library.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"animation_player_path", {{"type", "string"}}},
                {"animation_name", {{"type", "string"}}},
                {"custom_speed", {{"type", "number"}, {"default", 1.0}}}
            }},
            {"required", {"animation_player_path", "animation_name"}}
        };
        t.boundHandler = [this](const ResolvedToolBinding& binding, const json& args) {
            return handleAnimPlayTrack(binding, args, m_ipcClient);
        };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "anim_add_library";
        t.description = "Gives an AnimationPlayer in the edited scene an AnimationLibrary loaded from a res:// file, through the editor UndoRedo stack, and reports the animation names the player answers to afterwards. Adds only: a library name the player already uses is refused, not replaced. Save the scene before anim_play_track can play it in a running game.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"animation_player_path", {{"type", "string"}, {"minLength", 1}, {"maxLength", 1024}}},
                {"library_path", {{"type", "string"}, {"minLength", 1}, {"maxLength", 1024}}},
                {"library_name", {{"type", "string"}, {"maxLength", 256}, {"default", ""}}},
                {"reload_from_disk", {{"type", "boolean"}, {"default", false}}}
            }},
            {"required", json::array({"animation_player_path", "library_path"})},
            {"additionalProperties", false}
        };
        t.boundHandler = [this](const ResolvedToolBinding& binding, const json& args) {
            return handleAnimAddLibrary(binding, args, m_ipcClient);
        };
        registerTool(std::move(t));
    }
}

} // namespace mcp
} // namespace didi
