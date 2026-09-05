#pragma once

#include "didi/common/json.hpp"
#include "didi/common/types.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace didi {
namespace godot {

struct ViewportPixels {
    int width{0};
    int height{0};
    std::vector<uint8_t> rgba;
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
    Result<std::vector<std::string>> resolveReimportPaths(const std::vector<std::string>& paths);
    Result<void> startAssetReimport(const std::vector<std::string>& resolved_paths);
    Result<std::vector<std::string>> beginAssetReimport(const std::vector<std::string>& paths);
    Result<bool> isEditorFilesystemScanning();
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

} // namespace godot
} // namespace didi
