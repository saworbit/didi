#pragma once

// What a domain file needs to register its tools (#1256): the handlers and
// the helpers the registrations share.
// Only the registry and src/tools include it.

#include "didi/mcp/tool_registry.hpp"
#include "didi/tools/visual_test_lab_path.hpp"
#include "didi/offline/gdscript_diagnostics.hpp"
#include "didi/mcp/change_journal.hpp"
#include "didi/mcp/error_data.hpp"
#include "didi/mcp/follow_ups.hpp"
#include "didi/mcp/parameter_descriptions.hpp"
#include "didi/mcp/control_room.hpp"
#include "didi/mcp/project_tools.hpp"
#include "didi/common/logger.hpp"
#include "didi/runtime/audio_requests.hpp"
#include "didi/runtime/session_kind_policy.hpp"
#include "didi/runtime/undo_capture.hpp"
#include "didi/common/project_path.hpp"
#include "didi/common/scene_node_path.hpp"
#include "didi/tools/resolved_tool_binding.hpp"
#include "didi/mcp/phase7_schemas.hpp"
#include "didi/mcp/schema_validation.hpp"
#include "didi/mcp/response_economy.hpp"
#include "didi/offline/deep_domain_support.hpp"
#include "didi/offline/project_impact.hpp"
#include "didi/offline/project_settings_file.hpp"
#include "didi/offline/speculative_verify.hpp"
#include "didi/tools/editor_copy_refresh.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace didi {
namespace mcp {

// Every JSON type, for an argument that takes any value (#1000).
json anyJsonType();

// Output schemas for tools whose real result shape has been observed.
//
// A declared outputSchema is a promise about structuredContent, so a schema is
// added only after the tool's actual output has been seen. Tools that cannot be
// exercised here, and every unimplemented name, declare nothing rather than
// asserting a shape nobody has verified.
//
// `required` lists only fields guaranteed in every execution mode. Live results
// carry extra members that offline results do not -- capture identifiers,
// omitted-field lists, session envelopes -- and additional properties are
// permitted so those never invalidate a result.
namespace output_schema {

inline const json string_type = {{"type", "string"}};
inline const json integer_type = {{"type", "integer"}};
inline const json boolean_type = {{"type", "boolean"}};

inline json object_schema(json properties, std::vector<std::string> required) {
    return json{{"type", "object"},
                {"properties", std::move(properties)},
                {"required", std::move(required)}};
}
inline json array_of(json items) {
    return json{{"type", "array"}, {"items", std::move(items)}};
}

}  // namespace output_schema

// The handlers, each defined in the src/tools file that registers it.
CallToolResult handleCaptureViewport(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleViewportCapturePasses(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleEditorRenderGhostPreview(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleEditorClearGhostPreviews(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleViewportDiffCapture(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleViewportSetCameraTransform(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleCreateVisualTestLab(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleViewportToggleDebugDraw(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);

CallToolResult handleGetSceneHierarchy(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleSceneInstantiateNode(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleSceneRemoveNode(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleSceneReparentNode(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleSceneSetProperty(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleSceneCallMethod(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleSceneGetProperty(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleSceneDuplicateNode(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleMutateSceneTree(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleSceneListGroups(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleSceneAddToGroup(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleSceneRemoveFromGroup(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleSceneGetGroupMembers(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleSceneCreate(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleSceneOpen(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleSceneClose(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleScenePackBranch(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);

CallToolResult handleSignalListConnections(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleSignalConnect(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleSignalDisconnect(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleSignalEmit(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
std::optional<Error> refuseUnusableSignalArguments(const ResolvedToolBinding& binding, const json& arguments);

CallToolResult handleScriptCheckSyntax(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleProjectVerifyChanges(const json& args);
CallToolResult handleProjectApplyChanges(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleScriptReflectClass(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleScriptGetSymbols(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleScriptPatchMethod(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleScriptCreate(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleScriptAttachToNode(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleScriptDetachFromNode(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);

CallToolResult handlePhysicsRaycastQuery(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleSpatialQueryRaycastBatch(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleSpatialQueryClearance(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleSpatialQueryFrustum(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleShaderListUniforms(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleShaderSetUniform(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleShaderGetVisualGraph(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handlePhysicsSimulateStep(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleNavBakeMesh(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleNavQueryPath(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleAnimListTracks(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleAnimPlayTrack(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleAnimAddLibrary(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);

CallToolResult handleTilemapSetCells(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleTilemapGetUsedRect(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleGridmapSetCells(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);

CallToolResult handleQueryProjectResources(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleResourceCreate(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleResourceInspect(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleProjectGetUidMap(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleProjectAuditAssets(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleBlackboardWrite(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleBlackboardRead(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleBlackboardPatch(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleBlackboardListKeys(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleBlackboardClear(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleBlackboardTaskCreate(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleBlackboardTaskClaim(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleBlackboardTaskUpdate(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleBlackboardTaskComplete(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleBlackboardTaskList(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleSceneGetSelection(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleProjectAnalyzeImpact(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleProjectRenameReferences(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleAudioListBuses(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleAudioConfigureBus(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleAudioAddBus(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleInstantiateAsset(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleAssetReimport(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleAssetConfigureImport(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
Result<json> previewAssetConfigureImport(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleCSharpCheckBuild(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleShaderCheckCompile(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleProjectRunTests(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleProjectListExportPresets(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleProjectExport(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleProjectAddExportPreset(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleGridmapExportMeshLibrary(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleUiHitTest(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleUiListControls(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);

CallToolResult handleExecuteTestSession(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleRuntimeRunScenario(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleInjectInputEvent(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleRuntimeGetCallStack(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleRuntimeReadProfiler(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleRuntimeWatchInvariants(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleRuntimeExploreScene(const ResolvedToolBinding& binding, const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleRuntimeListSessions(const json& args, std::shared_ptr<runtime::IRuntimeSessionClient> sessions);
CallToolResult handleRuntimeAttachSession(const json& args, std::shared_ptr<runtime::IRuntimeSessionClient> sessions);
CallToolResult handleRuntimeDetachSession(const json& args, std::shared_ptr<runtime::IRuntimeSessionClient> sessions);
CallToolResult handleRuntimeGetSession(const json& args, std::shared_ptr<runtime::IRuntimeSessionClient> sessions,
                                       std::vector<std::string> available_without_engine = {});
CallToolResult handleRuntimeReadLogs(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleRuntimeReadOutput(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleRuntimeSetPaused(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleRuntimeStep(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleRuntimeStop(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleRuntimeGetTree(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleEvalGdscript(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);

CallToolResult handleEditorUndo(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleEditorRedo(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleEditorSaveScene(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleEditorReloadProject(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);

} // namespace mcp
} // namespace didi
