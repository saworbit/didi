#include "bridge_internal.hpp"
#include "didi/gdextension/godot_bridge.hpp"
#include "didi/gdextension/godot_object.hpp"
#include "didi/runtime/godot_arguments.hpp"
#include <algorithm>
#include <map>
#include <utility>

namespace didi::godot {
using namespace bridge_internal;
namespace {
// A safety cap, not a usability budget, and the difference matters. The walk had
// no bound at all, so a large enough edited scene could exceed the IPC frame
// before the tool layer saw a byte of it. These numbers exist only to stop that,
// and are set far above any ordinary scene: the smoke fixture's edited scene is
// already about 1700 nodes and 225 KiB, so a tight cap here would silently cut
// scenes people actually work on.
//
// Callers who want a small answer ask for one, with max_nodes, class_filter or
// summary on scene_get_hierarchy. That is the lever for context cost. This is
// the lever for not losing the response entirely.
constexpr size_t kMaxHierarchyNodes = 100000;
constexpr size_t kMaxHierarchyResponseBytes = 8 * 1024 * 1024;
constexpr size_t kHierarchyEnvelopeReserveBytes = 256 * 1024;

struct HierarchyBudget {
    size_t node_count{0};
    size_t estimated_bytes{0};
    bool truncated{false};
};

// Counts everything beneath a node by class, so a branch the depth limit cut
// can report what went rather than looking like a leaf. The walk reads classes
// and nothing else, so it costs no JSON, and it carries its own stack rather
// than the call stack: max_depth bounds how deep buildHierarchy recurses, and
// this runs past that bound by definition.
Result<void> countDescendantsByType(GDExtensionObjectPtr root, std::map<std::string, size_t>& counts,
                                    size_t& total) {
    std::vector<GDExtensionObjectPtr> pending{root};
    bool at_root = true;
    while (!pending.empty()) {
        GDExtensionObjectPtr node = pending.back();
        pending.pop_back();
        if (!at_root) {
            auto type = nodeString(node, "get_class", 201670096LL);
            if (type.isErr()) return type.error();
            ++total;
            ++counts[type.value()];
        }
        at_root = false;
        auto include_internal = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, static_cast<GDExtensionBool>(0));
        if (include_internal.isErr()) return include_internal.error();
        auto children = callObject(node, "Node", "get_children", 873284517LL, {&include_internal.value()});
        if (children.isErr()) return children.error();
        auto size_result = callVariant(children.value(), "size");
        if (size_result.isErr()) return size_result.error();
        auto size = scalarFromVariant<int64_t>(size_result.value(), GDEXTENSION_VARIANT_TYPE_INT);
        if (size.isErr()) return size.error();
        for (int64_t i = 0; i < size.value(); ++i) {
            auto index = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, i);
            if (index.isErr()) return index.error();
            auto child_variant = callVariant(children.value(), "get", {&index.value()});
            if (child_variant.isErr()) return child_variant.error();
            auto child = objectFromVariant(child_variant.value());
            if (child.isErr() || !child.value()) return Error::internal("Godot returned an invalid child node");
            pending.push_back(child.value());
        }
    }
    return Result<void>::ok();
}

Result<json> buildHierarchy(GDExtensionObjectPtr node, int depth, int max_depth,
                            const std::string& logical_path,
                            HierarchyBudget& budget, GDExtensionObjectPtr root,
                            const InheritedSceneState& inherited) {
    auto name = nodeString(node, "get_name", 2002593661LL);
    auto type = nodeString(node, "get_class", 201670096LL);
    if (name.isErr()) return name.error();
    if (type.isErr()) return type.error();
    const std::string path = logical_path.empty() ? "/root/" + name.value() : logical_path;
    json result = {
        {"name", name.value()}, {"type", type.value()}, {"path", path},
        {"properties", json::object()}, {"children", json::array()}
    };

    // Where the node comes from, so a caller can see what the file will hold
    // before mutating it: an instance root names its scene, a node inside an
    // instance is not owned by the edited scene, and a node the scene inherits
    // says so. The editor shows the same three facts as the link icon, the
    // greyed rows and the inherited marker; the walk reported none of them, so
    // a caller had no way to tell an instance's internals from its own nodes
    // until the save dropped the change (#591).
    auto ownership = classifyNodeOwnership(root, node, inherited);
    if (ownership.isErr()) return ownership.error();
    result["owned_by_scene"] = ownership.value().owned_by_scene;
    if (ownership.value().inherited) result["inherited"] = true;
    if (node != root) {
        auto scene_path = callObject(node, "Node", "get_scene_file_path", 201670096LL);
        if (scene_path.isErr()) return scene_path.error();
        auto instance_of = stringFromVariant(scene_path.value(), GDEXTENSION_VARIANT_TYPE_STRING);
        if (instance_of.isErr()) return instance_of.error();
        if (!instance_of.value().empty()) {
            result["instance_of"] = instance_of.value();
            auto node_value = makeObject(node);
            if (node_value.isErr()) return node_value.error();
            auto editable_value = callObject(root, "Node", "is_editable_instance", 3093956946LL,
                                             {&node_value.value()});
            if (editable_value.isErr()) return editable_value.error();
            auto editable = scalarFromVariant<GDExtensionBool>(editable_value.value(),
                                                               GDEXTENSION_VARIANT_TYPE_BOOL);
            if (editable.isErr()) return editable.error();
            result["editable_instance"] = editable.value() != 0;
        }
    }

    // Charged before descending, so the node that crosses the limit is the one
    // that stops rather than the one after it.
    const size_t node_bytes = result.dump().size() + 2;
    const size_t tree_budget = kMaxHierarchyResponseBytes - kHierarchyEnvelopeReserveBytes;
    if (budget.node_count >= kMaxHierarchyNodes ||
        budget.estimated_bytes + node_bytes > tree_budget) {
        budget.truncated = true;
        return json(nullptr);
    }
    budget.estimated_bytes += node_bytes;
    ++budget.node_count;

    auto include_internal = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, static_cast<GDExtensionBool>(0));
    if (include_internal.isErr()) return include_internal.error();
    if (depth >= max_depth) {
        // A branch cut by depth used to be byte for byte a leaf, so "no
        // children" and "children not reported" read the same. It now says what
        // it stopped on, the way a branch cut by max_nodes does.
        std::map<std::string, size_t> omitted_counts;
        size_t omitted_total = 0;
        auto counted = countDescendantsByType(node, omitted_counts, omitted_total);
        if (counted.isErr()) return counted.error();
        if (omitted_total > 0) {
            json summary = json::object();
            for (const auto& entry : omitted_counts) summary[entry.first] = entry.second;
            result["children_omitted"] = omitted_total;
            result["children_summary"] = summary;
            budget.truncated = true;
        }
        return result;
    }
    auto children = callObject(node, "Node", "get_children", 873284517LL, {&include_internal.value()});
    if (children.isErr()) return children.error();
    auto size_result = callVariant(children.value(), "size");
    if (size_result.isErr()) return size_result.error();
    auto size = scalarFromVariant<int64_t>(size_result.value(), GDEXTENSION_VARIANT_TYPE_INT);
    if (size.isErr()) return size.error();
    for (int64_t i = 0; i < size.value(); ++i) {
        auto index = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, i);
        if (index.isErr()) return index.error();
        auto child_variant = callVariant(children.value(), "get", {&index.value()});
        if (child_variant.isErr()) return child_variant.error();
        auto child = objectFromVariant(child_variant.value());
        if (child.isErr() || !child.value()) return Error::internal("Godot returned an invalid child node");
        auto child_name = nodeString(child.value(), "get_name", 2002593661LL);
        if (child_name.isErr()) return child_name.error();
        auto child_json = buildHierarchy(child.value(), depth + 1, max_depth,
                                         path + "/" + child_name.value(), budget, root, inherited);
        if (child_json.isErr()) return child_json.error();
        if (child_json.value().is_null()) {
            result["children_truncated"] = true;
            break;
        }
        result["children"].push_back(child_json.value());
    }
    return result;
}

} // namespace

json GodotBridge::bridgeExportReloadPresets(const std::string& /*method*/, const json& params, const std::string& session_kind) {
    // project_add_export_preset asks this after it has written
    // export_presets.cfg. The editor reads that file once, when it starts,
    // and writes its own list back over it 0.8 s after any preset changes,
    // so a preset written underneath it is lost with no error anywhere.
    // Registering or removing an export platform is the one public event
    // that makes it read the file again: EditorExport marks its presets
    // for reload and reloads them on its next process frame. A bare
    // EditorExportPlatformExtension, added and removed in one frame through
    // an EditorPlugin that is never in the tree, does that and prints
    // nothing. Measured on 4.5.1, 4.6.2 and 4.7.2 with
    // tools/vibe/probes/export_preset_engine.py; both binds are 3431312373
    // on all three.
    //
    // "request" does it. "confirm" does nothing: the server sends it only
    // once "request" has been answered, and this bridge serves requests
    // from its frame callback, so it is answered on a later frame, after
    // the editor has processed and read the file.
    if (session_kind != "editor") return bridgeError(409, "session_kind_rejected");
    const std::string step = params.value("step", "");
    if (step == "confirm") return liveResult({{"frame_passed", true}});
    if (step != "request") return errorJson(400, "step must be request or confirm");
    for (const char* bind : {"add_export_platform", "remove_export_platform"}) {
        if (requireMethodBind("EditorPlugin", bind, 3431312373LL).isErr()) {
            return bridgeError(501, "required_bind_unavailable");
        }
    }
    NativeName platform_class("EditorExportPlatformExtension");
    auto platform = constructObject(platform_class.ptr());
    if (!platform) return bridgeError(501, "export_platform_unavailable");
    // The Variant holds the platform's only reference, so it is freed when
    // this returns, after the editor has let go of it.
    auto platform_value = makeObject(platform);
    if (platform_value.isErr()) return errorJson(500, platform_value.error().message);
    NativeName plugin_class("EditorPlugin");
    auto plugin = constructObject(plugin_class.ptr());
    if (!plugin) return errorJson(500, "Godot ClassDB could not construct an EditorPlugin");
    auto added = callObject(plugin, "EditorPlugin", "add_export_platform", 3431312373LL,
                            {&platform_value.value()});
    auto removed = added.isOk()
                       ? callObject(plugin, "EditorPlugin", "remove_export_platform",
                                    3431312373LL, {&platform_value.value()})
                       : Result<VariantValue>(added.error());
    GodotApi::instance().object_destroy(plugin);
    if (added.isErr()) return errorJson(500, added.error().message);
    if (removed.isErr()) {
        // Added and not removed would leave a platform with no name in the
        // editor's list, so this is reported rather than passed over.
        return errorJson(500, "The export platform was registered and could not be removed: " +
                                  removed.error().message);
    }
    return liveResult({{"reload_requested", true}});
}


json GodotBridge::bridgeEditorGetProtocolServers(const std::string& /*method*/, const json& params, const std::string& session_kind) {
    // Where this editor's GDScript language server and Debug Adapter
    // Protocol server listen, so the server can reach them (Q11). The
    // settings name a port; an editor started with --lsp-port or
    // --dap-port listens on that instead, and only the process's own
    // command line still says so. A server this engine was built without
    // has no setting and is answered null.
    if (session_kind != "editor") return bridgeError(409, "session_kind_rejected");
    if (!hasOnlyKeys(params, {})) {
        return errorJson(400, "editor.getProtocolServers takes no parameters");
    }
    auto editor = editorInterface();
    if (editor.isErr()) return errorJson(editor.error());
    // EditorInterface.get_editor_settings is 4086932459, and
    // EditorSettings.has_setting 3927539163 and get_setting 1868160156, on
    // 4.5.1, 4.6.2 and 4.7.2.
    auto settings_value =
        callObject(editor.value(), "EditorInterface", "get_editor_settings", 4086932459LL);
    if (settings_value.isErr()) {
        return errorJson(settings_value.error());
    }
    auto settings = objectFromVariant(settings_value.value());
    if (settings.isErr() || !settings.value()) {
        return errorJson(500, "The editor has no EditorSettings to read its servers' ports from");
    }
    const auto setting = [&](const std::string& name) -> std::optional<VariantValue> {
        auto setting_name = makeString(name);
        if (setting_name.isErr()) return std::nullopt;
        auto present = callObject(settings.value(), "EditorSettings", "has_setting", 3927539163LL,
                                  {&setting_name.value()});
        if (present.isErr()) return std::nullopt;
        auto has = scalarFromVariant<GDExtensionBool>(present.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
        if (has.isErr() || !has.value()) return std::nullopt;
        auto value = callObject(settings.value(), "EditorSettings", "get_setting", 1868160156LL,
                                {&setting_name.value()});
        if (value.isErr()) return std::nullopt;
        return std::move(value.value());
    };
    const auto setting_port = [&](const std::string& name) -> std::optional<int64_t> {
        auto value = setting(name);
        if (!value) return std::nullopt;
        const auto type = GodotApi::instance().variant_get_type(value->ptr());
        if (type != GDEXTENSION_VARIANT_TYPE_INT) return std::nullopt;
        auto port = scalarFromVariant<int64_t>(*value, type);
        if (port.isErr()) return std::nullopt;
        return port.value();
    };
    const auto overrides = runtime::protocolPortOverrides(runtime::processArguments());
    const auto describe = [&](const std::string& port_setting, const std::optional<int>& override,
                              const std::string& host) -> json {
        const auto configured = setting_port(port_setting);
        if (!configured.has_value()) return nullptr;
        json server = {{"host", host},
                       {"port", override.has_value() ? int64_t{*override} : *configured},
                       {"port_source", override.has_value() ? "command_line" : "editor_settings"}};
        if (override.has_value()) server["settings_port"] = *configured;
        return server;
    };
    std::string language_server_host = "127.0.0.1";
    if (auto host = setting("network/language_server/remote_host")) {
        const auto type = GodotApi::instance().variant_get_type(host->ptr());
        if (type == GDEXTENSION_VARIANT_TYPE_STRING) {
            auto text = stringFromVariant(*host, type);
            if (text.isOk()) language_server_host = text.value();
        }
    }
    // The debug adapter always binds 127.0.0.1; it has no host setting.
    return liveResult({{"language_server",
                        describe("network/language_server/remote_port",
                                 overrides.language_server, language_server_host)},
                       {"debug_adapter", describe("network/debug_adapter/remote_port",
                                                  overrides.debug_adapter, "127.0.0.1")}});
}


json GodotBridge::bridgeEditorOpenScenes(const std::string& /*method*/, const json& params, const std::string& session_kind) {
    // Asked before a writer replaces scene files, so a scene open in a tab
    // that may hold unsaved changes stops the write before anything on disk
    // changes (#1068). The unsaved list is the same report editor.getState
    // carries, null with readable false before 4.7.
    if (session_kind != "editor") return bridgeError(409, "session_kind_rejected");
    if (!hasOnlyKeys(params, {})) return errorJson(400, "editor.openScenes takes no parameters");
    if (requireMethodBind("EditorInterface", "get_open_scenes", 1139954409LL).isErr()) {
        return bridgeError(501, "required_bind_unavailable");
    }
    auto editor = editorInterface();
    if (editor.isErr()) return errorJson(editor.error());
    auto open = openScenePaths(editor.value());
    if (open.isErr()) return errorJson(open.error());
    json answer = {{"open_scenes", open.value()}};
    answer.update(unsavedScenesReport(editor.value()));
    return liveResult(answer);
}


json GodotBridge::bridgeEditorGetRecoveryState(const std::string& /*method*/, const json& /*params*/, const std::string& /*session_kind*/, GDExtensionObjectPtr /*editor*/) {
    auto scanning = isEditorFilesystemScanning();
    if (scanning.isErr()) return errorJson(scanning.error());
    return liveResult({{"filesystem_scanning", scanning.value()}});
}


json GodotBridge::bridgeEditorGetSelection(const std::string& /*method*/, const json& /*params*/, const std::string& /*session_kind*/, GDExtensionObjectPtr editor) {
    // What the person has selected is the referent of "this node". Nothing
    // else in the surface can answer it, and a second MCP client cannot ask
    // at all, because only one may hold the editor route.
    auto selection_value = callObject(editor, "EditorInterface", "get_selection", 2690272531LL);
    if (selection_value.isErr()) {
        return errorJson(selection_value.error());
    }
    auto selection = objectFromVariant(selection_value.value());
    if (selection.isErr()) return errorJson(selection.error());
    if (!selection.value()) {
        return errorJson(500, "Godot returned no EditorSelection");
    }

    auto root_result = editedSceneRoot(editor);
    if (root_result.isErr()) {
        return errorJson(root_result.error());
    }
    auto root = root_result.value();

    auto nodes_value = callObject(selection.value(), "EditorSelection", "get_selected_nodes",
                                  2915620761LL);
    if (nodes_value.isErr()) {
        return errorJson(nodes_value.error());
    }
    auto size_value = callVariant(nodes_value.value(), "size");
    if (size_value.isErr()) return errorJson(size_value.error());
    auto size = scalarFromVariant<int64_t>(size_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
    if (size.isErr()) return errorJson(size.error());

    // A selection is a handful of nodes, but it is engine-supplied and this
    // runs on the main loop, so it is bounded like every other list.
    constexpr int64_t kMaxSelected = 256;
    const int64_t total = size.value();
    const int64_t reported = std::min(total, kMaxSelected);

    json selected = json::array();
    for (int64_t index = 0; index < reported; ++index) {
        auto position = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, index);
        if (position.isErr()) return errorJson(position.error());
        auto node_value = callVariant(nodes_value.value(), "get", {&position.value()});
        if (node_value.isErr()) return errorJson(node_value.error());
        auto node = objectFromVariant(node_value.value());
        if (node.isErr()) return errorJson(node.error());
        // A selected node can be freed between the engine building the list
        // and this reading it. Skipping is right: reporting a null path
        // would be a node path that resolves to nothing.
        if (!node.value()) continue;

        auto path = logicalPathFromEditedRoot(root, node.value());
        auto node_class = nodeString(node.value(), "get_class", 201670096LL);
        auto node_name = nodeString(node.value(), "get_name", 2002593661LL);
        // A node selected in another open scene is not addressable from the
        // edited root, so it is counted and not named rather than reported
        // with a path that does not resolve.
        if (path.isErr()) continue;
        json entry = {{"node_path", path.value()}};
        if (node_class.isOk()) entry["class"] = node_class.value();
        if (node_name.isOk()) entry["name"] = node_name.value();
        selected.push_back(std::move(entry));
    }

    json selection_result = {{"status", "success"},
                             {"selected", selected},
                             {"count", selected.size()},
                             {"selected_total", total},
                             {"truncated", total > reported}};
    addEditedSceneIdentity(selection_result, root);
    return liveResult(selection_result);
}


json GodotBridge::bridgeEditorState(const std::string& method, const json& params, const std::string& /*session_kind*/, GDExtensionObjectPtr editor) {
    auto root_result = editedSceneRoot(editor);
    if (root_result.isErr()) return errorJson(root_result.error());
    auto root = root_result.value();
    if (method == "editor.getState") {
        // The path the rest of the surface speaks, not Node.get_path().
        //
        // This is the resource a client reads to find out which scene is
        // being edited, and it answered with the node's internal editor
        // path: 364 characters of @EditorNode@20438/@Panel@14/... down to
        // the SubViewport the editor parents an edited scene under. Every
        // scene_* tool refuses that, so a caller that did the obvious thing
        // and passed it on got a 404 blaming the node. Where Godot parents
        // an edited scene is an implementation detail of the editor, and
        // publishing it both leaked that and handed back a value that
        // cannot be used (#502).
        //
        // logicalPathFromEditedRoot is the same function every scene answer
        // already builds its paths with, so the resource and the tools now
        // describe the same tree in the same vocabulary.
        auto root_path = logicalPathFromEditedRoot(root, root);
        if (root_path.isErr()) return errorJson(root_path.error());
        // The editor's dirty state travels with its state, so the one
        // reader that asks before deciding anything, didi_control_room,
        // can show an amber light for a scene that is one "don't save"
        // away from losing its changes (#557).
        json state = {{"status", "online"}, {"editor_connected", true},
                      {"active_scene_root", root_path.value()}};
        // Whether this engine can draw. A headless editor is the only one a
        // build machine, a container or an ssh session can run, and every
        // tool whose answer is a picture fails on it for as long as it
        // lives. The control room already knows how to report an engine
        // limitation; it could not report this one because nothing told it
        // (#676).
        state["display_server"] = displayServerName().empty()
                                      ? json(nullptr)
                                      : json(displayServerName());
        state["renders"] = engineCanRender();
        state.update(unsavedScenesReport(editor));
        return liveResult(state);
    }
    const std::string requested_root = params.value("root_path", "/root");
    auto target = resolveNode(root, requested_root);
    if (target.isErr()) return errorJson(target.error());
    int max_depth = std::clamp(params.value("max_depth", 10), 0, 64);
    auto logical_root = logicalPathFromEditedRoot(root, target.value());
    if (logical_root.isErr()) return errorJson(logical_root.error());
    auto inherited = inheritedSceneState(root);
    if (inherited.isErr()) return errorJson(inherited.error());
    HierarchyBudget budget;
    auto hierarchy = buildHierarchy(target.value(), 0, max_depth, logical_root.value(), budget,
                                    root, inherited.value());
    if (hierarchy.isErr()) return errorJson(hierarchy.error());
    if (hierarchy.value().is_null()) {
        return errorJson(413, "The edited scene root alone exceeds the hierarchy response budget");
    }
    // The live walk reads names, classes and paths and nothing else, so the
    // list of what it left out is the same on every call. It used to be
    // derived from include_properties and friends, which meant asking for
    // properties added "bulk_properties" to the list of things omitted, and
    // declining them took it off while the properties stayed empty (#482).
    json omitted = json::array({"bulk_properties", "signals", "scripts"});
    json hierarchy_result = {{"root_path", params.value("root_path", "/root")},
                             {"source", "live_scene_tree"}, {"scene_tree", hierarchy.value()},
                             {"omitted_fields", omitted},
                             {"node_count", budget.node_count},
                             {"max_nodes", kMaxHierarchyNodes},
                             {"max_response_bytes", kMaxHierarchyResponseBytes},
                             {"message", "Use focused property/signal tools for fields omitted from hierarchy traversal."}};
    if (budget.truncated) hierarchy_result["truncated"] = true;
    // The base scene, once, at the top: the root is that scene's root
    // rather than an instance of it, so it does not carry instance_of.
    if (!inherited.value().base_scene.empty()) {
        hierarchy_result["inherits"] = inherited.value().base_scene;
    }
    addEditedSceneIdentity(hierarchy_result, root);
    return liveResult(hierarchy_result);
}


} // namespace didi::godot
