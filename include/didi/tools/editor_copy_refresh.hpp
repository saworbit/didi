#pragma once

#include "didi/common/ipc_channel.hpp"
#include "didi/common/types.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace didi::mcp {

// What an attached editor did with its copies of files this server wrote.
//
// An editor keeps every resource it has loaded and does not re-read a file
// that changed underneath it, so after a write every live reader answers from
// the old copy until something reloads it: not editor_reload_project, not a
// scan (#1047). A writer asks the editor to reload its copy of each file it
// wrote, and says what happened.
//
// A scene open in a tab is a copy too, and the one that matters most: the next
// save writes the tab's tree back over the file (#1068). Its tab is reloaded
// from the file, which drops the tab's unsaved changes, so that happens only
// for a tab the editor can show is clean or when the caller said to discard.
// On 4.5 and 4.6 a rebuild can leave the rebuilt tab current, and the scene
// the caller was editing is then opened again once the tabs are done (#1072).
struct EditorCopyRefresh {
    // An editor answered. When it did not -- none attached, a game selected,
    // or a bridge too old to know the request -- nothing is said, because
    // nothing is known.
    bool answered{false};
    // The files the editor held and now holds as written.
    std::vector<std::string> reloaded;
    // The scenes open in a tab whose tab was rebuilt from the written file.
    std::vector<std::string> scenes_reloaded;
    // The files the editor held and could not reload, each with the reason.
    json failed = json::array();
};

// discard_unsaved lets a tab that has unsaved changes, or one the engine
// cannot show has none, be reloaded and lose them.
EditorCopyRefresh refreshEditorCopies(const std::shared_ptr<ipc::IIpcClient>& ipc,
                                      const std::vector<std::string>& res_paths,
                                      bool discard_unsaved = false);

// Asked before a writer replaces several files. Refuses, with nothing written,
// when one of them is a scene open in the attached editor whose tab has
// unsaved changes, or on Godot 4.5 and 4.6 whose tab cannot be shown to have
// none, unless the caller passed discard_unsaved. Says nothing when no editor
// answers, the same as the refresh.
std::optional<Error> refuseUnsavedOpenScenes(const std::shared_ptr<ipc::IIpcClient>& ipc,
                                             const std::vector<std::string>& res_paths,
                                             bool discard_unsaved);

// A writer of one file: editor_copy_reloaded, editor_scene_reloaded when a tab
// held it and was rebuilt, and editor_copy_error when the editor held the file
// and could not reload it.
void reportEditorCopy(json& payload, const EditorCopyRefresh& refresh);

// A writer of several: editor_copies_reloaded, editor_scenes_reloaded, and
// editor_copy_errors naming each file the editor held and could not reload.
void reportEditorCopies(json& payload, const EditorCopyRefresh& refresh);

} // namespace didi::mcp
