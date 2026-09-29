#pragma once

#include "didi/common/ipc_channel.hpp"
#include "didi/common/types.hpp"

#include <memory>
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
struct EditorCopyRefresh {
    // An editor answered. When it did not -- none attached, a game selected,
    // or a bridge too old to know the request -- nothing is said, because
    // nothing is known.
    bool answered{false};
    // The files the editor held and now holds as written.
    std::vector<std::string> reloaded;
    // The files the editor held and could not reload, each with the reason.
    json failed = json::array();
};

EditorCopyRefresh refreshEditorCopies(const std::shared_ptr<ipc::IIpcClient>& ipc,
                                      const std::vector<std::string>& res_paths);

// A writer of one file: editor_copy_reloaded, and editor_copy_error when the
// editor held the file and could not reload it.
void reportEditorCopy(json& payload, const EditorCopyRefresh& refresh);

// A writer of several: editor_copies_reloaded, and editor_copy_errors naming
// each file the editor held and could not reload.
void reportEditorCopies(json& payload, const EditorCopyRefresh& refresh);

} // namespace didi::mcp
