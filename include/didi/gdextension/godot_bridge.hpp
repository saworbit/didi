#pragma once

#include "didi/common/json.hpp"
#include "didi/common/types.hpp"
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace didi {
namespace godot {

struct ViewportPixels {
    int width{0};
    int height{0};
    std::vector<uint8_t> rgba;
};

// One accepted asset_reimport batch, split by which engine call each path
// needs. Godot's import system owns only the files carrying a .import
// sidecar; reimport_files errors on anything else, and update_file is the
// documented way to tell the editor a plain project file changed on disk.
struct ReimportBatch {
    std::vector<std::string> paths;
    std::vector<std::string> reimported;
    std::vector<std::string> refreshed;
};

struct VisibilityRestorePoint {
    uint64_t instance_id{0};
    std::string class_name;
    bool visible{true};
};

struct ViewportIsolationState {
    std::string canonical_node_path;
    std::string isolation_background{"original"};
    std::vector<VisibilityRestorePoint> visibility;
    uint64_t viewport_instance_id{0};
    bool restore_transparent_background{false};
    bool original_transparent_background{false};
};

// One rendered pass, and what the bytes in it mean.
//
// A pass is the scene drawn again with every geometry node's material replaced,
// so the picture answers one question rather than showing one appearance. The
// shaders write the inverse of the sRGB curve the framebuffer applies, so the
// stored byte is the number the pass is about rather than a bent version of it.
struct PassFrame {
    std::string kind;
    ViewportPixels pixels;
};

// A node a segmentation pass painted, and the entry it was painted with.
struct SegmentedNode {
    std::string path;
    std::string class_name;
    size_t entry{0};
};

struct MultipassCapture {
    std::vector<PassFrame> frames;
    // Only when a segmentation pass ran. In palette order, which is the order
    // the legend reads the frame back in.
    std::vector<SegmentedNode> segmented;
    // Nodes there was no entry left for. Named rather than counted, because a
    // node missing from a legend is otherwise indistinguishable from a node
    // that was hidden behind another one.
    std::vector<std::string> unsegmented;
    // The distance mapped to white in a depth pass, so the grey is a length
    // again rather than a ratio to something unstated.
    double depth_far{0.0};
    int painted{0};
    int examined{0};
    bool scan_limit_reached{false};
};

class GodotBridge {
public:
    static GodotBridge& instance();

    json execute(const std::string& method, const json& params,
                 const std::string& session_kind = "editor");
    Result<ViewportPixels> captureEditorViewport(const std::string& camera_identifier);
    // A game has one root viewport and no camera selection to make. Both paths
    // share one capture body, so the size check, the RGBA8 conversion and the
    // byte-count postcondition exist once rather than twice.
    Result<ViewportPixels> captureGameViewport();
    // Split so the caller can publish its pending request before the reimport
    // starts. EditorFileSystem.reimport_files re-enters the main-loop callback,
    // and a nested frame that cannot see the request misses the scanning window.
    Result<ReimportBatch> resolveReimportPaths(const std::vector<std::string>& paths);
    Result<void> startAssetReimport(const ReimportBatch& batch);
    Result<ReimportBatch> beginAssetReimport(const std::vector<std::string>& paths);
    Result<bool> isEditorFilesystemScanning();

    // A coroutine started by scene_call_method and not finished yet.
    //
    // Object.callv on a GDScript function containing await returns a
    // GDScriptFunctionState rather than the value. Answering with that object
    // would report a bake that has not happened, so the call is parked until
    // the state's completed signal carries the real return value (#389).
    struct PendingScriptCall {
        uint64_t await_id{0};
        std::string target_node;
        std::string method_name;
    };

    // Runs one script-declared method on a node in the edited scene.
    //
    // Returns the finished payload when the method returned normally. When it
    // returned a coroutine, `pending` names the wait and the caller answers
    // later through collectScriptCall.
    json callScriptMethod(const json& params, std::optional<PendingScriptCall>& pending);

    // The value a parked coroutine produced, or nothing while it is still
    // running. Removes the wait once it answers.
    std::optional<json> collectScriptCall(uint64_t await_id);

    // Drops a wait whose caller has given up, so a coroutine that never
    // finishes does not hold its captured value forever.
    void abandonScriptCall(uint64_t await_id);

    // What the engine knows about a resource Didi just wrote.
    struct WrittenResourceUid {
        std::string uid;          // the uid:// in the file, empty when it has none
        bool registered{false};   // whether ResourceUID resolves it back to the path
        bool deferred{false};     // whether a retry was queued because a scan is running
    };

    // Teaches the engine about a file Didi wrote behind the editor's back.
    //
    // ResourceSaver.save writes the uid into the file, and in a settled editor
    // Godot's own save callback indexes it. While EditorFileSystem is scanning
    // that callback does nothing, the scan's directory snapshot predates the
    // file, and the uid ends up in the file but not in ResourceUID. Everything
    // outside the editor then warns on every load (#379).
    WrittenResourceUid registerWrittenResourceUid(const std::string& resource_path);

    // Re-indexes anything registerWrittenResourceUid could not, once the
    // editor filesystem is idle. Called once per frame by the editor hook.
    void processDeferredReindexFrame();

    // The editor main screen: the 2D/3D/Script/Game/AssetLib tab bar, plus any
    // main screen an addon adds.
    //
    // An editor viewport has no size unless its main screen is the selected
    // one, so a capture of it is impossible without this, and nothing in the
    // tool surface could select one. Switching main screens is the one thing a
    // person does with a mouse that an unattended agent could not do at all
    // (#381).
    //
    // Reading the current one is by class of the visible child, because Godot
    // exposes a setter and no getter. The built-in screens are all identifiable
    // that way; an addon's is not, and that is reported rather than guessed.
    std::optional<std::string> currentMainScreenName();
    Result<void> selectMainScreen(const std::string& name);
    Result<ViewportIsolationState> beginViewportIsolation(const std::string& node_path,
                                                          const std::string& camera_identifier,
                                                          const std::string& isolation_background);
    Result<void> restoreViewportIsolation(const ViewportIsolationState& state);
    // Draws the scene once per requested pass with replacement materials, and
    // puts every material_override back before returning, including on the
    // paths that fail. 3D only: a depth pass has no meaning on a canvas.
    Result<MultipassCapture> captureViewportPasses(const std::vector<std::string>& passes,
                                                   const std::string& camera_identifier,
                                                   const std::string& session_kind,
                                                   double requested_depth_far);
    Result<void> forceDraw();
    // Performance.get_monitor support for runtime.readProfiler. Preflight is
    // the availability check the contract names: the pinned bind exists.
    Result<void> preflightPerformanceMonitors();
    Result<std::vector<double>> samplePerformanceMonitors(const std::vector<int64_t>& monitors);

private:
    GodotBridge() = default;
};

Result<std::string> resolveGodotProjectPath();

// Which editor viewport a camera_identifier names.
//
// One table, read from every place that has to know. The answer used to be
// written out twice: #209 taught `editor_2d` to refuse a viewport with no size
// on screen, and its aliases `2d` and `canvas_item` were not in that branch, so
// they fell through to the 3D case and returned a picture of the 3D viewport
// described as the 2D one. An identifier in neither list is nothing, not 3D,
// because treating an unrecognised name as 3D is the same lie spelled
// differently.
enum class EditorViewport { TwoD, ThreeD };
std::optional<EditorViewport> selectEditorViewport(const std::string& camera_identifier);

// The identifiers above, for the refusal a caller reads.
std::string editorViewportIdentifierList();

// Whether a JSON value may be written to a property of a given Godot variant
// type. The type is the numeric GDEXTENSION_VARIANT_TYPE_* code, taken as an
// int so the decision, and the message built from it, can be exercised without
// a running engine.
enum class PropertyTypeMatch {
    Compatible,
    Incompatible,
    UnsupportedPropertyType,
};

PropertyTypeMatch matchJsonToPropertyType(const json& value, int godot_type);

// The name Godot gives a variant type. A caller reading a rejection has no way
// to turn a bare enum number back into a type, so nothing user-facing prints
// one.
std::string godotVariantTypeName(int godot_type);

// The name of the JSON type a value actually carries: "string", "number",
// "boolean", "null", "array", "object".
std::string jsonValueTypeName(const json& value);

// The rejection a caller reads when the JSON type of their value does not
// match the property's Godot type. It names the property, what arrived, and
// what to send instead, because the value they sent is the whole mistake and
// the response is the only thing they can see.
std::string describePropertyTypeMismatch(const std::string& property_name,
                                         const json& value, int godot_type);
// Whether a JSON value can stand for a value of a Godot property type, where
// variant_type is a GDExtensionVariantType. The answer comes from the JSON
// alone, so it holds with or without a running engine.
bool jsonValueFitsPropertyType(const json& value, int variant_type);

// A double in a form the wire can carry back. JSON has no spelling for inf or
// nan and nlohmann serialises both as `null`, so a property holding inf used to
// read back as null: not a number, not round-trippable, and identical on the
// wire to "unset" or "unknown". Non-finite values come back as "inf", "-inf" or
// "nan"; a finite one is unchanged.
json realToJson(double value);

// The refusal a caller reads when a number is outside what the property can
// hold, or nothing when it fits.
//
// A Godot float property is real_t, 32-bit in a standard build, and Vector2,
// Vector3 and Color are made of the same. A JSON number above about 3.4e38
// becomes inf the moment it lands there. Pure, so it holds with or without a
// running engine.
std::optional<std::string> describeRealRangeRefusal(const std::string& property_name,
                                                    const json& value, int godot_type);

// Every bridge failure identifier this build can answer with, and the sentence
// it says.
//
// These used to answer with the identifier as the whole message, which is the
// string a client shows a person: `target_method_not_found` does not say which
// method was looked for or on which node. Exported so a test can assert that
// no identifier reaches a caller without a sentence, without a running engine.
const std::map<std::string, std::string>& bridgeErrorSentenceTable();

} // namespace godot
} // namespace didi
