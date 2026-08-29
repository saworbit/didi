#include "didi/mcp/prompt_registry.hpp"

namespace didi {
namespace mcp {

PromptRegistry& PromptRegistry::instance() {
    static PromptRegistry s_instance;
    return s_instance;
}

void PromptRegistry::registerPrompt(PromptDefinition prompt) {
    m_prompts[prompt.name] = std::move(prompt);
}

const PromptDefinition* PromptRegistry::getPrompt(const std::string& name) const {
    auto it = m_prompts.find(name);
    if (it != m_prompts.end()) {
        return &it->second;
    }
    return nullptr;
}

std::vector<PromptDefinition> PromptRegistry::listPrompts() const {
    std::vector<PromptDefinition> list;
    list.reserve(m_prompts.size());
    for (const auto& [name, p] : m_prompts) {
        list.push_back(p);
    }
    return list;
}

Result<json> PromptRegistry::getPromptResult(const std::string& name, const json& args) {
    auto prompt = getPrompt(name);
    if (!prompt) {
        return Error::notFound("Prompt template not found: " + name);
    }
    return prompt->getHandler(args);
}

void PromptRegistry::registerAllDefaultPrompts() {
    // 1. godot_debug_visual_anomaly
    PromptDefinition debug_visual;
    debug_visual.name = "godot_debug_visual_anomaly";
    debug_visual.description = "Capability-aware visual inspection workflow using only currently implemented hierarchy, viewport, file, and focused scene/property tools.";
    debug_visual.arguments = {
        {"target_resource_path", "Path to target resource or scene (e.g. 'res://models/character.glb')", true},
        {"symptom_description", "Description of observed visual anomaly or glitch", false}
    };
    debug_visual.getHandler = [](const json& args) -> Result<json> {
        std::string res_path = args.value("target_resource_path", "res://");
        std::string symptom = args.value("symptom_description", "Inspect for visual or transform anomalies");

        std::string prompt_text =
            "You are diagnosing a visual or spatial anomaly in Godot 4.7+ for resource: " + res_path + "\n"
            "Reported Issue: " + symptom + "\n\n"
            "Capability-aware workflow:\n"
            "1. Inspect `tools/list` and do not call entries with `implemented: false`; require live mode for editor state.\n"
            "2. Use `project_list_resources` and `scene_get_hierarchy` to locate the asset and understand either live or parsed state.\n"
            "3. If useful, call `viewport_create_test_lab` to write a sandbox scene, then explicitly open or run that scene before drawing conclusions.\n"
            "4. Use `viewport_capture_frame` for the active editor viewport and verify `is_live_frame` before treating pixels as live evidence.\n"
            "5. Apply only supported focused `scene_*` scalar/node changes or `script_patch_method`, re-read the affected state, and report any unsupported camera, debug-draw, shader, or composite-property step.";

        json result = {
            {"description", "Capability-aware visual anomaly workflow for Godot 4.7+"},
            {"messages", json::array({
                {
                    {"role", "user"},
                    {"content", {{"type", "text"}, {"text", prompt_text}}}
                }
            })}
        };
        return result;
    };
    registerPrompt(std::move(debug_visual));

    // 2. godot_generate_gameplay_slice
    PromptDefinition generate_slice;
    generate_slice.name = "godot_generate_gameplay_slice";
    generate_slice.description = "Capability-aware gameplay-slice workflow using implemented file, focused scene, diagnostics, and separate-process test tools.";
    generate_slice.arguments = {
        {"feature_name", "Name of gameplay mechanic/slice (e.g. 'PlayerController', 'InventorySystem')", true},
        {"requirements", "Gameplay mechanics and technical requirements", true}
    };
    generate_slice.getHandler = [](const json& args) -> Result<json> {
        std::string feature = args.value("feature_name", "GameplayFeature");
        std::string reqs = args.value("requirements", "Implement core mechanics");

        std::string prompt_text =
            "You are implementing the supported portion of a Godot 4.7+ gameplay slice: " + feature + "\n"
            "Requirements:\n" + reqs + "\n\n"
            "Capability-aware workflow:\n"
            "1. Inspect `tools/list`; do not call entries with `implemented: false`, and require live mode for editor mutations.\n"
            "2. Use `project_list_resources` and `scene_get_hierarchy` to understand existing files and scene structure.\n"
            "3. When live, construct built-in nodes with focused `scene_instantiate_node`, `scene_set_property`, and other implemented `scene_*` tools.\n"
            "4. Edit project scripts through the normal workspace or `script_patch_method`, then run `script_check_syntax`.\n"
            "5. Use `runtime_launch` for a separate-process test and inspect captured stdout/stderr.\n"
            "6. Explicitly report unimplemented script attachment, signal wiring, input injection, or runtime introspection instead of claiming those steps succeeded.";

        json result = {
            {"description", "Capability-aware gameplay slice workflow for Godot 4.7+"},
            {"messages", json::array({
                {
                    {"role", "user"},
                    {"content", {{"type", "text"}, {"text", prompt_text}}}
                }
            })}
        };
        return result;
    };
    registerPrompt(std::move(generate_slice));
}

} // namespace mcp
} // namespace didi
