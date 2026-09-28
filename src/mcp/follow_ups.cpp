// The work a successful mutation left undone (Q6 in docs/BUILD_QUEUE.md,
// principle P5). See follow_ups.hpp for the shape and why it exists.
//
// Each rule reads one fact an answer already publishes, and says in `reason`
// the same thing the answer's own prose says about it. A fact no rule reads is
// a step nobody gets, so the live harness fails an answer that carries one of
// these facts without the step.
//
// There is no rescan rule. Writing a file behind an attached editor leaves its
// cached copy stale, and a rescan does not refresh it: measured on all three
// engine lines, only a CACHE_MODE_REPLACE load does (#1047).

#include "didi/mcp/follow_ups.hpp"

#include <string>

namespace didi {
namespace mcp {

namespace {

bool isBoolean(const json& payload, const char* key, bool expected) {
    const auto found = payload.find(key);
    return found != payload.end() && found->is_boolean() && found->get<bool>() == expected;
}

std::string text(const json& payload, const char* key) {
    const auto found = payload.find(key);
    return found != payload.end() && found->is_string() ? found->get<std::string>() : std::string();
}

// Managed recovery saves the scene after a protected mutation and says so in
// its receipt. The bridge's scene_saved: false was true when the bridge
// answered, and is not by the time the caller reads it.
bool savedByRecovery(const json& payload) {
    const auto recovery = payload.find("recovery");
    if (recovery == payload.end() || !recovery->is_object()) return false;
    const auto operation = recovery->find("operation");
    return operation != recovery->end() && operation->is_object() &&
           operation->value("outcome", std::string()) == "completed_saved_files_checkpointed";
}

json step(const char* work, const char* tool, const char* reason) {
    json follow = {{"work", work}, {"reason", reason}};
    if (tool != nullptr) follow["tool"] = tool;
    return follow;
}

}  // namespace

json followUpsFor(const json& payload) {
    json steps = json::array();
    if (!payload.is_object()) return steps;

    // A live edit of the open scene, committed to its undo history (#557).
    if (isBoolean(payload, "scene_saved", false) && !savedByRecovery(payload)) {
        steps.push_back(step("save", "editor_save_scene",
                             "The change is in the editor's open scene and its undo history, not "
                             "on disk, and closing the editor without saving discards it."));
    }

    // The attached editor keeps what it started with: a new or removed
    // autoload, or a bus layout the project now names differently.
    if (isBoolean(payload, "requires_editor_restart", true)) {
        steps.push_back(step("restart", nullptr,
                             "The attached editor keeps the state it started with until it "
                             "restarts, and editor_reload_project does not change that. Nothing "
                             "Didi sends restarts an editor."));
    }

    // project.godot written with no editor attached. A live write goes through
    // the editor, which saves the file itself.
    const std::string written_to = text(payload, "written_to");
    const bool live = text(payload, "execution_mode") == "live";
    if (!live && written_to == "res://project.godot") {
        steps.push_back(step("restart", nullptr,
                             "project.godot was written directly. A Godot already running on this "
                             "project reads it only when it starts, and an editor open on it "
                             "without the Didi addon writes its own settings over the file when "
                             "it next saves them."));
    }

    // A preset no attached editor was made to read. An editor that starts
    // after this reads the file, so the step is for one already open.
    if (written_to == "res://export_presets.cfg" && isBoolean(payload, "editor_reloaded", false)) {
        steps.push_back(step("restart", nullptr,
                             "No attached editor read export_presets.cfg again. An editor open on "
                             "this project reads it only when it starts, and writes its own list "
                             "over the file when any preset changes in its Export dialog, so "
                             "restart it before using that dialog."));
    }
    return steps;
}

bool applyFollowUps(json& payload) {
    if (!payload.is_object() || payload.contains("follow_up")) return false;
    auto steps = followUpsFor(payload);
    if (steps.empty()) return false;
    payload["follow_up"] = std::move(steps);
    return true;
}

}  // namespace mcp
}  // namespace didi
