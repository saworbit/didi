#pragma once

#include "didi/common/types.hpp"

namespace didi {
namespace mcp {

// The work a successful mutation left undone, as steps a caller can act on
// (Q6 in docs/BUILD_QUEUE.md, principle P5).
//
// An answer used to say it in prose, where a person reads it: a live scene edit
// carried scene_saved: false and a limitation sentence, and a new autoload
// carried requires_editor_restart. An agent acts on what it can branch on, and
// a guide that asks for a follow-up without a call to attach it to changed
// nothing in trial 06. So the answer names it, under `follow_up`:
//
//   [{"work": "save",    "tool": "editor_save_scene", "reason": "..."},
//    {"work": "restart", "reason": "..."}]
//
// `work` is save, restart or rescan. `tool` is the call that does it, present
// when a tool can; nothing Didi sends restarts an editor, so a restart has
// none. `reason` says what is undone.
//
// Every step is derived from a fact the answer already carries, never from the
// tool's name alone, so a step is exactly as true as the fact behind it. The
// prose stays beside it. tests/follow_ups.json says, for every mutating tool,
// which work it can leave, and the live harness fails a step a tool did not
// declare and a fact that arrived without its step.
[[nodiscard]] json followUpsFor(const json& payload);

// Adds `follow_up` to a successful answer's payload when it leaves work and
// does not name it already. An answer that names its own is left alone.
// Returns whether it added one.
bool applyFollowUps(json& payload);

}  // namespace mcp
}  // namespace didi
