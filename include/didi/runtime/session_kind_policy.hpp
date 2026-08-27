#pragma once

#include <string_view>

namespace didi::runtime {

enum class LiveSessionKindPolicy { editor_only, game_only, editor_or_game };

inline LiveSessionKindPolicy livePolicyForTool(std::string_view name) {
    if (name == "runtime_set_paused" || name == "runtime_step" || name == "runtime_stop") {
        return LiveSessionKindPolicy::game_only;
    }
    if (name == "runtime_read_logs" || name == "runtime_get_tree" ||
        name == "eval_gdscript") {
        return LiveSessionKindPolicy::editor_or_game;
    }
    return LiveSessionKindPolicy::editor_only;
}

inline LiveSessionKindPolicy livePolicyForMethod(std::string_view method) {
    if (method == "runtime.setPaused" || method == "runtime.step" ||
        method == "runtime.stop") {
        return LiveSessionKindPolicy::game_only;
    }
    if (method == "runtime.getLogs" || method == "runtime.getTree" ||
        method == "runtime.evalGdscript" || method == "session.handshake") {
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
