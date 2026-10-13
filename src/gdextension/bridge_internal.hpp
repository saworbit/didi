#pragma once

// Private bridge helpers shared by domain implementations. These retain the
// existing engine ownership, scene refusal and undo paths; domains do not copy
// those rules or add another write path.
#include "didi/gdextension/godot_values.hpp"
#include <optional>
#include <set>
#include <initializer_list>

namespace didi::godot::bridge_internal {

enum class SceneEdit {
    // A value, group or script on the node: an override the file holds for an
    // inherited node, nothing for a foreign one.
    Property,
    // Removing, moving or duplicating the node: nothing the file can hold for
    // a foreign or an inherited node.
    Structure,
    // A new node under this one: dropped with its parent when the parent is
    // foreign, kept under an inherited one.
    AddChild
};

json bridgeError(int code, const std::string& identifier, json data = json::object(),
                 const std::string& detail = {});
std::string nodeClassName(GDExtensionObjectPtr object);
Result<bool> objectIsClass(GDExtensionObjectPtr object, const char* class_name);
Result<std::string> stringFromVariant(VariantValue& value, GDExtensionVariantType type);
Result<void> requireMethodBind(const char* class_name, const char* method_name, int64_t hash);
Result<void> validateResPath(const std::string& path, const std::string& expected_suffix);
Result<std::string> projectDirectoryOnDisk();
Result<GDExtensionObjectPtr> editorInterface();
Result<GDExtensionObjectPtr> editedSceneRoot(GDExtensionObjectPtr editor);
Result<GDExtensionObjectPtr> resolveNode(GDExtensionObjectPtr root, const std::string& path);
std::optional<json> refuseUnsavableEdit(GDExtensionObjectPtr root, GDExtensionObjectPtr node,
                                     const std::string& described_path, SceneEdit edit);
Result<GDExtensionObjectPtr> undoManager(GDExtensionObjectPtr editor);
Result<void> preflightUndoManagerBindings();
Result<void> preflightUndoRollbackBindings();
Result<void> createAction(GDExtensionObjectPtr manager, const std::string& name,
                        GDExtensionObjectPtr context_object);
Result<void> managerMethod(GDExtensionObjectPtr manager, const char* operation,
                          GDExtensionObjectPtr object, const std::string& method,
                          const std::vector<const VariantValue*>& method_args);
Result<void> commitAction(GDExtensionObjectPtr manager);
Result<void> undoLastAction(GDExtensionObjectPtr manager, GDExtensionObjectPtr root);
void abandonAction(GDExtensionObjectPtr manager);
json liveResult(const json& fields);
json liveSceneMutation(json fields);

json animListTracks(const json& params, const std::string& session_kind);
json animPlayTrack(const json& params, const std::string& session_kind);
json animAddLibrary(const json& params, const std::string& session_kind);

Result<VariantValue> makeVector2(double x, double y);
Result<json> variantToJson(VariantValue& value, int depth = 0, bool lenient = false);
Result<json> vector2ToJson(VariantValue& value);
Result<json> rect2ToJson(VariantValue& value);
bool hasOnlyKeys(const json& value, std::initializer_list<const char*> allowed);
Result<std::string> nodeString(GDExtensionObjectPtr node, const char* method, int64_t hash);
Result<std::string> logicalPathFromEditedRoot(GDExtensionObjectPtr root, GDExtensionObjectPtr node);

json uiListControls(const json& params, const std::string& session_kind);
json uiHitTest(const json& params, const std::string& session_kind);

Result<VariantValue> makeVector3(double x, double y, double z);
Result<std::set<int64_t>> packedIntegerSet(VariantValue& packed);
Result<json> pointVariantToJson(VariantValue& value, int dimension);

json physicsClearance(const json& params, const std::string& session_kind);
json visionFrustumQuery(const json& params, const std::string& session_kind);
json physicsRaycastBatch(const json& params, const std::string& session_kind);
json physicsRaycast(const json& params, const std::string& session_kind);
json navQueryPath(const json& params, const std::string& session_kind);

std::string busLayoutPathFrom(const std::optional<std::string>& setting);
std::string busLayoutMovedSentence(const std::string& editor_path, const std::string& project_path);

} // namespace didi::godot::bridge_internal
