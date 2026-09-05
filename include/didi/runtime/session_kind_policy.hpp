#pragma once

#include <string_view>

namespace didi::runtime {

enum class LiveSessionKindPolicy { editor_only, game_only, editor_or_game };
using SessionKindPolicy = LiveSessionKindPolicy;

enum class SessionKind { editor, game };

inline LiveSessionKindPolicy livePolicyForTool(std::string_view name) {
    if (name == "runtime_set_paused" || name == "runtime_step" || name == "runtime_stop" ||
        name == "physics_simulate_step" || name == "anim_play_track" ||
        name == "runtime_inject_input" || name == "inject_input_event" ||
        // Watching conditions while a game runs, and stopping it on the frame
        // that broke one. An editor has a SceneTree and monitors of its own,
        // but neither is a game running, so the answer would describe the
        // wrong thing.
        name == "runtime_watch_invariants") {
        return LiveSessionKindPolicy::game_only;
    }
    if (name == "runtime_read_logs" || name == "runtime_read_output" || name == "runtime_get_tree" ||
        name == "eval_gdscript" || name == "physics_raycast_query" ||
        name == "nav_query_path" || name == "spatial_query_raycast_batch" ||
        name == "spatial_query_clearance" || name == "spatial_query_frustum" ||
        name == "anim_list_tracks" ||
        name == "runtime_read_profiler" ||
        // A running game can be driven and read but could never be seen. The
        // frame is in the process Didi is attached to; the editor-only camera
        // identifiers stay editor-only.
        name == "viewport_capture_frame" || name == "capture_viewport" ||
        name == "viewport_diff_capture") {
        return LiveSessionKindPolicy::editor_or_game;
    }
    return LiveSessionKindPolicy::editor_only;
}

inline LiveSessionKindPolicy livePolicyForMethod(std::string_view method) {
    if (method == "runtime.setPaused" || method == "runtime.step" ||
        method == "runtime.stop" || method == "physics.simulateStep" ||
        method == "anim.playTrack" || method == "runtime.injectInput" ||
        method == "runtime.watchInvariants") {
        return LiveSessionKindPolicy::game_only;
    }
    if (method == "runtime.getLogs" || method == "runtime.getOutput" || method == "runtime.getTree" ||
        method == "runtime.evalGdscript" || method == "session.handshake" ||
        method == "physics.raycast" || method == "physics.raycastBatch" ||
        method == "physics.clearance" || method == "vision.frustumQuery" ||
        method == "nav.queryPath" ||
        method == "anim.listTracks" || method == "runtime.readProfiler" ||
        method == "profiler.sample" ||
        method == "vision.captureViewport" || method == "vision.diffViewport") {
        return LiveSessionKindPolicy::editor_or_game;
    }
    return LiveSessionKindPolicy::editor_only;
}

inline bool allowsSessionKind(LiveSessionKindPolicy policy, std::string_view kind) {
    switch (policy) {
        case LiveSessionKindPolicy::editor_only: return kind == "editor";
        case LiveSessionKindPolicy::game_only: return kind == "game";
        case LiveSessionKindPolicy::editor_or_game:
            return kind == "editor" || kind == "game";
    }
    return false;
}

} // namespace didi::runtime
