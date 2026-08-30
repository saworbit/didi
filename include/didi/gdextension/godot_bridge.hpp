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

class GodotBridge {
public:
    static GodotBridge& instance();

    json execute(const std::string& method, const json& params,
                 const std::string& session_kind = "editor");
    Result<ViewportPixels> captureEditorViewport(const std::string& camera_identifier);
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
    Result<void> forceDraw();

private:
    GodotBridge() = default;
};

Result<std::string> resolveGodotProjectPath();

} // namespace godot
} // namespace didi
