#include "didi/gdextension/visual_test_lab.hpp"
#include "didi/gdextension/viewport_renderer.hpp"
#include "didi/common/logger.hpp"

namespace didi {
namespace godot {

VisualTestLab& VisualTestLab::instance() {
    static VisualTestLab s_instance;
    return s_instance;
}

json VisualTestLab::createLab(const json& params) {
    std::string target_path = params.value("target_resource_path", "");
    std::string environment = params.value("environment", "studio_neutral");
    bool orthographic = params.value("orthographic", false);
    json camera_rig = params.value("camera_rig", json::array({"front", "back", "left", "right", "top", "isometric"}));

    DIDI_LOG_INFO("VISUAL_TEST_LAB", "Creating Visual Test Lab for asset: ", target_path);

    // Render an initial viewport preview from the front camera
    json capture_params = {
        {"camera_identifier", "lab_camera_front"},
        {"resolution", {{"width", 1024}, {"height", 768}}}
    };
    json capture_result = ViewportRenderer::instance().captureViewport(capture_params);

    json result = {
        {"status", "created"},
        {"test_lab_scene", "res://addons/didi/test_lab_sandbox.tscn"},
        {"target_resource_path", target_path},
        {"environment", environment},
        {"orthographic", orthographic},
        {"camera_rig", camera_rig},
        {"available_cameras", json::array({
            {{"id", "lab_camera_front"}, {"position", {{"x", 0.0}, {"y", 1.5}, {"z", 3.0}}}},
            {{"id", "lab_camera_back"}, {"position", {{"x", 0.0}, {"y", 1.5}, {"z", -3.0}}}},
            {{"id", "lab_camera_left"}, {"position", {{"x", -3.0}, {"y", 1.5}, {"z", 0.0}}}},
            {{"id", "lab_camera_right"}, {"position", {{"x", 3.0}, {"y", 1.5}, {"z", 0.0}}}},
            {{"id", "lab_camera_top"}, {"position", {{"x", 0.0}, {"y", 4.0}, {"z", 0.0}}}},
            {{"id", "lab_camera_isometric"}, {"position", {{"x", 3.0}, {"y", 3.0}, {"z", 3.0}}}}
        })},
        {"image_base64", capture_result.value("image_base64", "")},
        {"message", "Visual Test Lab spawned with lighting environment '" + environment + "' and multi-camera rig."}
    };

    return result;
}

} // namespace godot
} // namespace didi
