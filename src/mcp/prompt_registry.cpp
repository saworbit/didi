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
    debug_visual.description = "Directs the model to inspect node transforms, spawn a visual test lab, capture multi-angle screenshots, verify mesh/skeleton alignments, and suggest coordinate fixes.";
    debug_visual.arguments = {
        {"target_resource_path", "Path to target resource or scene (e.g. 'res://models/character.glb')", true},
        {"symptom_description", "Description of observed visual anomaly or glitch", false}
    };
    debug_visual.getHandler = [](const json& args) -> Result<json> {
        std::string res_path = args.value("target_resource_path", "res://");
        std::string symptom = args.value("symptom_description", "Inspect for visual or transform anomalies");

        std::string prompt_text =
            "You are tasked with diagnosing and fixing a visual or spatial anomaly in Godot 4.x for resource: " + res_path + "\n"
            "Reported Issue: " + symptom + "\n\n"
            "Recommended Diagnostic Workflow:\n"
            "1. Call `create_visual_test_lab` with target_resource_path='" + res_path + "' to spawn an isolated visual testbed.\n"
            "2. Call `capture_viewport` across camera angles ('lab_camera_front', 'lab_camera_left', 'lab_camera_top') to inspect alignment, normals, and lighting.\n"
            "3. Inspect node transforms and bone orientations using `get_scene_hierarchy`.\n"
            "4. Formulate precise coordinate, shader, or hierarchy fixes using `mutate_scene_tree` or `patch_script_symbols`.\n"
            "5. Re-verify the fixed state by capturing new viewport screenshots.";

        json result = {
            {"description", "Debug visual anomaly workflow for Godot 4.x"},
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
    generate_slice.description = "Directs the model to inspect existing project resources, generate a character/mechanic scene tree, attach typed GDScript, bind signals, and verify through an automated test run.";
    generate_slice.arguments = {
        {"feature_name", "Name of gameplay mechanic/slice (e.g. 'PlayerController', 'InventorySystem')", true},
        {"requirements", "Gameplay mechanics and technical requirements", true}
    };
    generate_slice.getHandler = [](const json& args) -> Result<json> {
        std::string feature = args.value("feature_name", "GameplayFeature");
        std::string reqs = args.value("requirements", "Implement core mechanics");

        std::string prompt_text =
            "You are creating a complete, production-ready Godot 4.x gameplay slice: " + feature + "\n"
            "Requirements:\n" + reqs + "\n\n"
            "Standard Construction Workflow:\n"
            "1. Call `query_project_resources` to index available meshes, textures, sounds, and scripts.\n"
            "2. Call `get_scene_hierarchy` to understand the existing scene structure.\n"
            "3. Use `mutate_scene_tree` to construct necessary nodes (CharacterBody2D/3D, CollisionShape, Area, etc.).\n"
            "4. Write typed, robust GDScript and validate compilation diagnostics via `analyze_script_diagnostics`.\n"
            "5. Run the feature in a headless test session using `execute_test_session` and check engine stdout/stderr.\n"
            "6. Use `inject_input_event` if needed to verify runtime response to input actions.";

        json result = {
            {"description", "Generate gameplay slice workflow for Godot 4.x"},
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
