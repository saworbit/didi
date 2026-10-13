#pragma once

// Private bridge helpers shared by domain implementations. These retain the
// existing engine ownership, scene refusal and undo paths; domains do not copy
// those rules or add another write path.
#include "didi/gdextension/godot_values.hpp"
#include <optional>
#include <filesystem>
#include <set>
#include <initializer_list>
#include <string_view>

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

Result<void> validateScriptPath(const std::string& path);
Result<void> validateJsonForPropertyType(const std::string& property_name, const json& value,
                                       GDExtensionVariantType type);
Result<VariantValue> makeJsonVariant(const json& value, int depth = 0);
Result<VariantValue> makeJsonVariantForProperty(const json& value, GDExtensionVariantType type);
bool settingTakesTypedValue(GDExtensionVariantType held);

json wrongNodeTypeError(const std::string& identifier, const std::string& path,
                        GDExtensionObjectPtr object);
Result<VariantValue> makeVector2i(int64_t x, int64_t y);
Result<VariantValue> makeVector3i(int64_t x, int64_t y, int64_t z);
Result<json> integerVectorToJson(VariantValue& value, int dimensions);

Error bridgeRefusal(int code, const std::string& identifier);
#if defined(DIDI_PHASE7_SIGNAL_TEST_SEAMS)
bool takePhase7SignalTestSeam(std::string_view expected);
#endif

struct InheritedSceneState {
    // The scene the edited scene inherits, or empty when it inherits nothing.
    std::string base_scene;
    // Root-relative node paths the base chain declares, "." for the root.
    std::set<std::string> node_paths;
};


struct NodeOwnership {
    // The edited root owns it, or it is the root: what the file lists.
    bool owned_by_scene{false};
    // Owned by an instance the edited scene has not marked editable, or by
    // nothing: the packer discards it, and everything under it.
    bool foreign{false};
    // Declared by the scene this one inherits.
    bool inherited{false};
    std::string owner_scene;    // the instance's scene file, when foreign
    std::string instance_root;  // that instance's logical path, when foreign
};

Result<InheritedSceneState> inheritedSceneState(GDExtensionObjectPtr root);
Result<NodeOwnership> classifyNodeOwnership(GDExtensionObjectPtr root, GDExtensionObjectPtr node,
                                          const InheritedSceneState& inherited);
void addEditedSceneIdentity(json& payload, GDExtensionObjectPtr root);
Result<std::vector<std::string>> openScenePaths(GDExtensionObjectPtr editor);
json unsavedScenesReport(GDExtensionObjectPtr editor);
const std::string& displayServerName();
bool engineCanRender();

Result<std::string> relativePathWithinEditedRoot(GDExtensionObjectPtr root, GDExtensionObjectPtr node);
std::string sceneStatePathWithinEditedRoot(const std::string& state_path);

struct AttachableScript {
    VariantValue script;
    GDExtensionObjectPtr object{nullptr};
    std::string base_type;
};

Result<AttachableScript> loadAttachableScript(GDExtensionObjectPtr node, const std::string& script_path);
bool resourceFileExists(const std::string& path);
std::string resourcePathOf(GDExtensionObjectPtr resource);
Result<uint64_t> openSceneRootId(GDExtensionObjectPtr editor, const std::string& path);
std::string editedScenePath(GDExtensionObjectPtr root);

bool pathWithin(const std::filesystem::path& root, const std::filesystem::path& candidate);
bool engineHasClass(const std::string& name);
bool unsavedScenesBindAvailable();

} // namespace didi::godot::bridge_internal
