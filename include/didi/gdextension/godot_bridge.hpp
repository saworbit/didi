#pragma once

#include "didi/common/json.hpp"
#include "didi/common/types.hpp"
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace didi {
namespace godot {

// Did a property end up holding what the caller asked for?
//
// Compares by value rather than by JSON type, because Godot legitimately
// changes the type on the way in: an integer written to a float property reads
// back as a real, and reporting that as "not applied" would be a false alarm on
// a write that worked perfectly.
//
// Composites are compared member by member for the same reason. A Color or a
// Vector arrives as a JSON object and used to take an exact-equality branch, so
// a colour whose components are not exactly representable in the float32 the
// type is made of -- 0.1, say, which reads back as 0.10000000149011612 -- was
// reported as a write that did not land, and so was one sent without its
// optional alpha (#618). The tolerance is sized to that float32 round trip
// rather than to a double, because that is the narrowest storage the engine
// actually uses for these types; two values closer than that cannot be told
// apart once stored.
[[nodiscard]] bool jsonValuesEquivalent(const json& observed, const json& requested);

// What to say about a write that did not land.
//
// A property write can fail to take in two ways that look identical under
// `applied: false`: the property can still hold what it held, or the engine can
// store a third value of its own. The second used to be invisible, because the
// only other thing the caller got was `value`, and a substituted value reads as
// a plausible one.
//
// The report names which of those two happened, and relays the engine's own
// declared range or enum for the property, which is often the remedy: the
// minimum a Timer's wait_time takes, or the buses that actually exist. It does
// not name a cause. Three of the four measured failures are indistinguishable
// in everything the call can observe, and the one explanation the engine
// writes goes to its error stream, which a GDExtension cannot read. See the
// definition for the measurements.
[[nodiscard]] json notAppliedReport(const json& observed, const json& old_value, int hint,
                                    const std::string& hint_string);

// What the extension can see of the editor's own import pass.
//
// EditorFileSystem.reimport_files pumps the main loop from inside itself: its
// progress dialog calls Main::iteration on every step, so the frame callback
// runs in the middle of the pass, and so does anything it answers or starts.
// The pass clears its importing flag, then opens a second progress task, and
// only then emits resources_reimported. A reimport started in those frames
// opens a second "reimport" task on top of the first, and an answer sent from
// them says the work is done before the editor has reloaded the resources
// (#914). is_importing() already reads false there. The windowed-editor rows
// of tools/vibe/probes/import_config_engine.py measure all of this on 4.5.1,
// 4.6.2 and 4.7.2. A headless editor pumps nothing inside the pass, so only a
// windowed one shows it.
//
// resources_reimporting and resources_reimported bracket the whole pass, tail
// included. An extension cannot receive a signal, so the addon's
// didi_import_watch.gd counts them. is_importing() binds from 4.7 and covers
// the frames before resources_reimporting, which the counts cannot see. Each
// part is absent where the engine or the project's copy of the addon cannot
// supply it.
struct ImportPassObservation {
    // Didi's own reimport_files is on the stack.
    bool inside_own_call{false};
    // EditorFileSystem.is_importing, where the engine binds it.
    std::optional<bool> importing;
    // resources_reimporting and resources_reimported, as the addon counted them.
    std::optional<int64_t> started;
    std::optional<int64_t> finished;
};

// Open while any part says so. One function emits both signals with nothing
// between them that returns, so a pass is open exactly while started is ahead.
// A watch that began mid-pass sees finished ahead instead, which reads as
// closed rather than as a pass that never ends.
[[nodiscard]] inline bool importPassOpen(const ImportPassObservation& seen) {
    if (seen.inside_own_call) return true;
    if (seen.importing.value_or(false)) return true;
    return seen.started.has_value() && seen.finished.has_value() &&
           *seen.started > *seen.finished;
}

// Scan work the editor has yet to finish, waited out through the addon's count
// of sources_changed. A scan clears the editor's scanning flag on its own
// thread, and a later frame applies what it found: it swaps in a new file
// index, updates script classes and their documentation under progress tasks
// that run frames of their own, and only then emits sources_changed. A
// reimport started in between had the old index freed under it, and one
// answered in between sent the caller's next request into that work.
struct ScanSettle {
    // sources_changed as counted when the wait began. The wait is for a later
    // emission.
    int64_t after{0};
    // The file index a scan Didi started will replace, when it started one.
    // The editor applies a scan inside frames of its own, so a scan started in
    // those frames is applied after that scan's emission, and the emission
    // that ends the wait is one made with another index.
    std::optional<int64_t> replaced_index;
};

// sources_changed as the addon's watch has counted it.
struct ScanSettleObservation {
    std::optional<int64_t> settled;
    // The file index the editor held at the last emission.
    std::optional<int64_t> settled_index;
};

// Over once sources_changed has fired since the wait began, with another index
// when Didi's own scan is the one awaited. A watch the addon could not make
// has nothing to report, which reads as over: the scanning flag and the
// sidecars still bound the wait, as they did before the watch counted this.
[[nodiscard]] inline bool scanSettled(const ScanSettle& wait, const ScanSettleObservation& seen) {
    if (!seen.settled.has_value()) return true;
    if (*seen.settled <= wait.after) return false;
    if (!wait.replaced_index.has_value() || !seen.settled_index.has_value()) return true;
    return *seen.settled_index != *wait.replaced_index;
}

// What startAssetReimport did.
struct ReimportStart {
    // reimport_files was not called, and the frame loop calls it once the
    // editor can take it.
    bool held{false};
    // Scan work the reimport, and the answer, wait for.
    std::optional<ScanSettle> settle;
};

// A shader uniform's declared hint_range, as the engine spells it.
//
// Godot puts the range a shader author wrote in the uniform's PropertyInfo as
// PROPERTY_HINT_RANGE with a hint_string of "min,max" and an optional step,
// followed by flags. or_greater and or_less are the two that matter here: they
// say the author meant the range as a slider bound rather than as a limit.
struct ShaderHintRange {
    double minimum{0.0};
    double maximum{0.0};
    std::optional<double> step;
    bool or_greater{false};
    bool or_less{false};
};

[[nodiscard]] std::optional<ShaderHintRange> parseShaderHintRange(const std::string& hint_string);

// What a resource slot's declared type allows, and what it rules out.
//
// Godot puts a resource property's accepted types in its PropertyInfo as a
// PROPERTY_HINT_RESOURCE_TYPE hint_string, and get_property_list repeats that
// string verbatim under class_name. It is a list, not a name: MeshInstance3D
// declares material_override as "BaseMaterial3D,ShaderMaterial", and an entry
// written with a leading "-" names a class the slot excludes, as Decal does
// with "Texture2D,-AnimatedTexture,-AtlasTexture,...". Twelve such strings
// cover 34 properties in the pinned class reference, including every material
// on every node, so treating one as a single class name refused every write to
// all of them (#783).
struct ResourceTypeHint {
    std::vector<std::string> accepted;
    std::vector<std::string> excluded;
};

[[nodiscard]] ResourceTypeHint parseResourceTypeHint(const std::string& declared_type);

// Whether a resource of a given class may be written into a slot with this
// hint, by the rule the editor's own picker applies
// (EditorResourcePicker::_ensure_allowed_types): each accepted entry stands for
// itself and everything that inherits from it, and an excluded entry is then
// erased from that set by name, so an exclusion beats the entry that admitted
// it. `inherits` answers "is this class, or does it descend from, that one" --
// the engine's ClassDB question, passed in so the rule can be exercised without
// a running engine.
enum class ResourceTypeVerdict {
    Accepted,
    NotAccepted,
    Excluded,
};

[[nodiscard]] ResourceTypeVerdict resourceTypeVerdict(
    const ResourceTypeHint& hint, const std::string& resource_class,
    const std::function<bool(const std::string&)>& inherits);

// The sentence a caller reads when a resource does not suit the slot. Names
// every type the slot takes, because a list of one was the only case the old
// message could describe.
[[nodiscard]] std::string describeResourceTypeRefusal(const std::string& property_name,
                                                      const ResourceTypeHint& hint,
                                                      const std::string& resource_path,
                                                      const std::string& resource_class,
                                                      ResourceTypeVerdict verdict);

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
    // Paths Godot's import system already owns, which is exactly the ones
    // carrying a .import sidecar. reimport_files reads the importer name out of
    // that file.
    std::vector<std::string> reimported;
    // Paths with no sidecar. Two different things live here and the difference
    // only shows afterwards: a .gd or a .tscn never gets one and is simply
    // announced to the editor, while an asset the editor has never scanned --
    // a .png written into the project by something other than Godot -- needs
    // importing before anything can use it. Reported as refreshed and idle,
    // that second case read as "done, nothing was stale" for an asset that was
    // unusable (#731).
    std::vector<std::string> refreshed;
    // Whether a full EditorFileSystem.scan is needed to find them. Set when any
    // path has no sidecar: scan_sources looks at files the editor already knows
    // about, and a file it has never seen is not one of those.
    bool needs_scan{false};
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
    // Whether a .import sidecar exists for this res:// path right now.
    // Asked again after a scan, because whether an asset ended up
    // imported is an observation and not something to infer from the
    // call that was made.
    bool assetIsImported(const std::string& resource_path);
    // Whether the .import sidecar Godot wrote records a failed import.
    bool assetImportFailed(const std::string& resource_path);
    // Whether the editor still has work outstanding for this path.
    //
    // The scanning flag clears before the importer has written its sidecars, so
    // a wait that ends when the flag does reports a freshly imported asset as
    // unimported. EditorFileSystemDirectory.get_file_import_is_valid is the
    // editor's own answer for one file: false while an import is outstanding
    // and for one that failed, true once it is done and for a file that needs
    // no importing at all. Unknown when the editor has not indexed the path,
    // which is itself outstanding work during a scan.
    bool assetImportSettled(const std::string& resource_path);
    // Refreshes and scans what the batch needs, then reimports its imported
    // assets unless it cannot yet, and says which. reimport_files cannot find
    // a file while the editor scans, so an asset named then is skipped with
    // "Can't find file ... during file reimport" and nothing else to show for
    // it. It is held back while any scan runs, Didi's or the editor's own,
    // until that scan's results are applied, and while the editor does not
    // list one of the assets, and started by reimportIndexedAssets once that
    // is over.
    Result<ReimportStart> startAssetReimport(const ReimportBatch& batch);
    // The reimport_files half, for assets the editor lists, while it is not
    // scanning. A 409 editor_import_busy when the editor's own pass refused it.
    Result<void> reimportIndexedAssets(const std::vector<std::string>& reimported);
    // The paths the editor's filesystem does not list, in the order given.
    std::vector<std::string> unindexedAssets(const std::vector<std::string>& resource_paths);
    // Whether the editor has a modal progress task open, which it does for
    // the work that follows a scan that found new scripts (updating script
    // classes and their documentation). Frames run inside that work, and a
    // reimport started in one collides with it. False when the editor's
    // ProgressDialog cannot be found, with one warning.
    bool editorProgressOpen();
    // A wait for scan work already under way, begun now. Empty when the
    // addon's watch is unavailable, which leaves nothing to wait on.
    std::optional<ScanSettle> beginScanSettle();
    // Whether the scan work a wait began for is over.
    bool scanSettled(const ScanSettle& wait);
    Result<bool> isEditorFilesystemScanning();
    // The editor's own import pass as this session can see it. Makes the
    // addon's watch the first time an editor session asks.
    ImportPassObservation observeEditorImportPass();
    // Lets the watch go. Called as the extension deinitializes.
    void releaseImportWatch();

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
    // A string project setting, or nothing when the project does not set it
    // or the engine cannot answer yet. Read at initialization for the
    // extension's own log level (#601).
    std::optional<std::string> projectSettingString(const std::string& name);

    // audio/buses/default_bus_layout as it stood when the engine started. The
    // editor's Audio panel reads the setting once, when it is built, and saves
    // the layout to that file until the editor restarts, whatever the setting
    // says later: measured on 4.7.2 in vibe session nineteen, where a setting
    // moved underneath the editor left it writing the old file, and the next
    // start loaded the new one, empty. Remembered at SCENE initialization,
    // before anything can change it.
    void rememberStartupBusLayoutSetting();
    // The res:// file the editor writes the bus layout to, or nothing when the
    // setting was never remembered (a test, or a bridge that did not start).
    std::optional<std::string> startupBusLayoutPath();

    // Hands Input the events runtime.injectInput held while the tree was
    // paused, so they land in the first frame that processes. Called by
    // runtime.setPaused on the way to running, which the step also takes.
    // Returns how many were handed over.
    Result<size_t> releaseQueuedInput();

    // Performance.get_monitor support for runtime.readProfiler. Preflight is
    // the availability check the contract names: the pinned bind exists.
    Result<void> preflightPerformanceMonitors();
    Result<std::vector<double>> samplePerformanceMonitors(const std::vector<int64_t>& monitors);

private:
    GodotBridge() = default;
    bool m_startupBusLayoutRemembered = false;
    std::optional<std::string> m_startupBusLayoutSetting;
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
