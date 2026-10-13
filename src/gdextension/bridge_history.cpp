#include "bridge_internal.hpp"
#include "didi/gdextension/godot_bridge.hpp"
#include "didi/gdextension/editor_hook.hpp"
#include "didi/common/godot_error.hpp"
#include "didi/common/project_path.hpp"
#include <filesystem>
#include <utility>

namespace didi::godot {
using namespace bridge_internal;

json GodotBridge::bridgeEditorUndoStatus(const std::string& /*method*/, const json& params, const std::string& /*session_kind*/, GDExtensionObjectPtr /*editor*/) {
    return undoStatusAnswer(params.is_object() ? params : json::object());
}


json GodotBridge::bridgeEditorUndoRedo(const std::string& method, const json& params, const std::string& /*session_kind*/, GDExtensionObjectPtr editor) {

    if (method == "editor.undo" && params.is_object() && params.contains("expect")) {
        return undoReferencedAction(params);
    }
    auto root = editedSceneRoot(editor);
    auto manager = undoManager(editor);
    if (root.isErr()) return errorJson(root.error());
    if (manager.isErr()) return errorJson(manager.error());
    auto root_value = makeObject(root.value());
    auto history_id = callObject(manager.value(), "EditorUndoRedoManager", "get_object_history_id", 1107568780LL, {&root_value.value()});
    if (history_id.isErr()) return errorJson(history_id.error());
    auto undo_redo = historyUndoRedo(manager.value(), history_id.value());
    if (undo_redo.isErr()) return errorJson(404, "No UndoRedo history exists for the edited scene");
    const bool is_undo = method == "editor.undo";
    // The editor's own command undoes the newer of the scene's history and
    // the global one, so either having an action is enough to run it.
    const auto has_action = [is_undo](GDExtensionObjectPtr history) -> Result<bool> {
        auto available = callObject(history, "UndoRedo", is_undo ? "has_undo" : "has_redo", 36873697LL);
        if (available.isErr()) return available.error();
        auto flag = scalarFromVariant<GDExtensionBool>(available.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
        if (flag.isErr()) return flag.error();
        return flag.value() != 0;
    };
    auto scene_has = has_action(undo_redo.value());
    if (scene_has.isErr()) return errorJson(scene_has.error());
    auto global_id = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, static_cast<int64_t>(0));
    auto global = global_id.isOk() ? historyUndoRedo(manager.value(), global_id.value())
                                   : Result<GDExtensionObjectPtr>(global_id.error());
    auto global_has = global.isOk() ? has_action(global.value()) : Result<bool>(false);
    if (!scene_has.value() && !(global_has.isOk() && global_has.value())) {
        return errorJson(409, is_undo ? "Nothing to undo" : "Nothing to redo",
                         {{"code", "nothing_to_undo"}});
    }
    auto moved = runEditorHistoryCommand(editor, manager.value(), undo_redo.value(), is_undo);
    if (moved.isErr()) return errorJson(moved.error());
    if (moved.value() == HistoryMoved::None) {
        return errorJson(409,
                         std::string("The editor ran its own ") + (is_undo ? "Undo" : "Redo") +
                             " and no history moved. Godot refuses to " +
                             (is_undo ? "undo" : "redo") +
                             " while a mouse button is held down in the editor.",
                         {{"code", "editor_declined"}, {"retryable", true}});
    }
    // A step can land on the saved version as well as move off it, so
    // the answer reads which (#1049).
    json stepped = {{"status", "success"},
                    {"action", is_undo ? "undo" : "redo"},
                    {"history", moved.value() == HistoryMoved::Scene ? "scene" : "global"}};
    reportEditedSceneSaved(stepped);
    return liveResult(stepped);
}


json GodotBridge::bridgeEditorSaveScene(const std::string& /*method*/, const json& /*params*/, const std::string& /*session_kind*/, GDExtensionObjectPtr editor) {
    // Where the engine's own output stands before the save, so anything it
    // prints during one can be attributed to this call.
    //
    // Against a headless editor every save produces `ERROR: Parameter "t"
    // is null` from the dummy rendering backend: the save path asks for a
    // scene thumbnail and there is no renderer to make one. The scene does
    // save, the tool reported `saved` and nothing else, and the error
    // landed only in the editor's log, which nothing in the response
    // pointed at (#683). The thumbnail step belongs to Godot's save and
    // cannot be switched off from here, so what is reported is that the
    // save completed with an engine diagnostic, and what it was.
    const auto before = EditorHook::instance().engineOutput().nextSequence();
    // The file, read around the save. save_scene answers OK for any open
    // scene with a path, including one the editor then refuses to write
    // behind a dialog (a cyclic instance, a missing dependency), so
    // "saved" came from a return code that says nothing about the file
    // (#1019). A write moves the modified time; one that did not is
    // refused, and what was written is read back.
    namespace fs = std::filesystem;
    auto saving_root = editedSceneRoot(editor);
    const std::string scene_path =
        saving_root.isOk() && saving_root.value() ? editedScenePath(saving_root.value()) : std::string();
    fs::path scene_file;
    if (strings::startsWith(scene_path, "res://")) {
        auto project_path = resolveGodotProjectPath();
        if (project_path.isOk()) {
            scene_file = didi::paths::projectPathFromUtf8(project_path.value()) /
                         didi::paths::projectPathFromUtf8(scene_path.substr(6));
        }
    }
    std::error_code before_error;
    const auto written_before = scene_file.empty() ? fs::file_time_type{}
                                                   : fs::last_write_time(scene_file, before_error);
    const bool existed_before = !scene_file.empty() && !before_error;
    auto saved = callObject(editor, "EditorInterface", "save_scene", 166280745LL);
    if (saved.isErr()) return errorJson(saved.error());
    auto code = scalarFromVariant<int64_t>(saved.value(), GDEXTENSION_VARIANT_TYPE_INT);
    if (code.isErr()) return errorJson(code.error());
    if (code.value() != 0) {
        return errorJson(::didi::godot::isGodotPathError(code.value()) ? 400 : 500,
                         "Godot save_scene failed with " +
                             ::didi::godot::describeGodotError(code.value()));
    }
    json result = {{"status", "saved"}, {"scene_path", scene_path}};
    if (!scene_file.empty()) {
        std::error_code after_error;
        const auto written_after = fs::last_write_time(scene_file, after_error);
        std::error_code size_error;
        const auto bytes = after_error ? std::uintmax_t{0} : fs::file_size(scene_file, size_error);
        if (after_error || size_error || (existed_before && written_after == written_before)) {
            return errorJson(409,
                             "The editor accepted the save and did not write " + scene_path +
                                 ". It refuses a scene it cannot save behind a dialog of its "
                                 "own, such as one that instances itself or names a missing "
                                 "dependency; its reason is in the editor's output.",
                             {{"code", "save_not_written"}, {"scene_path", scene_path}});
        }
        result["file_bytes"] = static_cast<uint64_t>(bytes);
    }
    reportEditedSceneSaved(result);
    constexpr size_t kMaxSaveDiagnostics = 8;
    auto during = EditorHook::instance().engineOutput().read(before, kMaxSaveDiagnostics,
                                                             "error");
    json diagnostics = json::array();
    if (during.isOk()) {
        const auto records = during.value().find("records");
        if (records != during.value().end() && records->is_array()) {
            for (const auto& record : *records) diagnostics.push_back(record);
        }
    }
    if (!diagnostics.empty()) {
        result["engine_diagnostics"] = std::move(diagnostics);
        result["engine_diagnostics_note"] =
            engineCanRender()
                ? "The scene saved. The engine printed these while saving it."
                : "The scene saved. This editor is headless, so Godot's save path could not "
                  "render the scene thumbnail and printed these; they are about the "
                  "thumbnail, not about the scene.";
    }
    return liveResult(result);
}

} // namespace didi::godot
