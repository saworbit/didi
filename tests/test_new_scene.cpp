#include "didi/mcp/tool_registry.hpp"
#include "didi/offline/new_scene.hpp"
#include "didi/offline/resource_indexer.hpp"
#include "didi/offline/resource_uid.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

void registerTest(const std::string& name, std::function<void()> fn);

namespace {

using didi::json;
using didi::offline::NewSceneRequest;
using didi::offline::createSceneFile;

class ScopedNewSceneProject final {
public:
    explicit ScopedNewSceneProject(const std::string& project_godot = "config_version=5\n")
        : m_original(std::filesystem::current_path()),
          m_root(std::filesystem::temp_directory_path() /
                 ("didi-new-scene-" +
                  std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
        std::filesystem::create_directories(m_root);
        std::filesystem::current_path(m_root);
        write("project.godot", project_godot);
        didi::offline::ResourceIndexer::invalidateSharedIndex();
    }
    ~ScopedNewSceneProject() {
        std::error_code error;
        std::filesystem::current_path(m_original, error);
        std::filesystem::remove_all(m_root, error);
        didi::offline::ResourceIndexer::invalidateSharedIndex();
    }
    void write(const std::string& name, const std::string& text) const {
        std::ofstream(m_root / name, std::ios::binary) << text;
    }
    std::string read(const std::string& name) const {
        std::ifstream input(m_root / name, std::ios::binary);
        std::ostringstream buffer;
        buffer << input.rdbuf();
        return buffer.str();
    }
    bool exists(const std::string& name) const { return std::filesystem::exists(m_root / name); }
    const std::filesystem::path& root() const { return m_root; }

private:
    std::filesystem::path m_original;
    std::filesystem::path m_root;
};

std::string codeOf(const didi::Error& error) { return error.data.is_object() ? error.data.value("code", "") : ""; }
std::string fieldOf(const didi::Error& error) { return error.data.is_object() ? error.data.value("field", "") : ""; }

json payloadOf(const didi::mcp::CallToolResult& result) {
    for (const auto& item : result.content) {
        if (item.type == "text") return json::parse(item.text);
    }
    return json::object();
}

// What the engine's text saver writes for a root-only scene, measured on
// 4.5.1, 4.6.2 and 4.7.2, with the uid an editor gives it, and the rename
// Node.set_name makes.
void text_and_names() {
    ASSERT_EQ(didi::offline::newSceneText("Node2D", "Root", "uid://abc"),
              "[gd_scene format=3 uid=\"uid://abc\"]\n\n[node name=\"Root\" type=\"Node2D\"]\n");
    ASSERT_TRUE(didi::offline::newSceneText("Node", "tab\there 'q'\nx", "uid://abc")
                    .find("[node name=\"tab\\there \\'q\\'\\nx\" type=\"Node\"]") != std::string::npos);
    // The saver leaves ? alone. It writes \a and \v, which its own parser
    // reads back as letters, so those two go in as they are.
    ASSERT_TRUE(didi::offline::newSceneText("Node", "a?b\a\v", "uid://abc")
                    .find("[node name=\"a?b\a\v\" type=\"Node\"]") != std::string::npos);
    ASSERT_EQ(didi::offline::engineNodeName("a.b:c@d\"e%f"), "a_b_c_d_e_f");
    ASSERT_EQ(didi::offline::engineNodeName("a,b[c]=d;e$f#g"), "a,b[c]=d;e$f#g");
    ASSERT_EQ(didi::offline::engineNodeName(" lead and trail "), " lead and trail ");
}

// Break caught: with no editor attached scene_create answered 503, so a
// project's scenes could not be laid out before anything opened it (#1380).
void writes_a_scene() {
    ScopedNewSceneProject project;
    NewSceneRequest request{"res://scenes/level/player.tscn", "CharacterBody2D", "Player"};
    auto first = createSceneFile(project.root(), request);
    ASSERT_TRUE(first.isOk());
    ASSERT_EQ(first.value().scene_path, "res://scenes/level/player.tscn");
    ASSERT_EQ(first.value().root_name, "Player");
    ASSERT_TRUE(!first.value().replaced);
    ASSERT_TRUE(didi::offline::isResourceUidText(first.value().uid));
    ASSERT_EQ(project.read("scenes/level/player.tscn"),
              "[gd_scene format=3 uid=\"" + first.value().uid +
                  "\"]\n\n[node name=\"Player\" type=\"CharacterBody2D\"]\n");

    // A second call is refused as the editor's route refuses it, and the file
    // is unchanged.
    auto again = createSceneFile(project.root(), request);
    ASSERT_TRUE(again.isErr());
    ASSERT_EQ(again.error().code, 409);
    ASSERT_EQ(codeOf(again.error()), "already_exists");
    ASSERT_EQ(again.error().data["retry_with"]["overwrite"], true);
    ASSERT_EQ(project.read("scenes/level/player.tscn"), first.value().text);

    // Replacing it keeps its uid, so a scene that references it still does.
    request.root_type = "Area2D";
    request.overwrite = true;
    auto replaced = createSceneFile(project.root(), request);
    ASSERT_TRUE(replaced.isOk());
    ASSERT_TRUE(replaced.value().replaced);
    ASSERT_EQ(replaced.value().uid, first.value().uid);
    ASSERT_TRUE(project.read("scenes/level/player.tscn").find("type=\"Area2D\"") != std::string::npos);

    // Node itself, and the rename the engine makes.
    auto renamed = createSceneFile(project.root(), {"res://renamed.tscn", "Node", "My.Root:1"});
    ASSERT_TRUE(renamed.isOk());
    ASSERT_EQ(renamed.value().root_name, "My_Root_1");
    ASSERT_TRUE(project.read("renamed.tscn").find("[node name=\"My_Root_1\" type=\"Node\"]") != std::string::npos);
    ASSERT_TRUE(renamed.value().uid != first.value().uid);
}

void refuses_what_the_editor_refuses() {
    ScopedNewSceneProject project;
    const auto refused = [&](const NewSceneRequest& request, const std::string& field) {
        auto written = createSceneFile(project.root(), request);
        ASSERT_TRUE(written.isErr());
        ASSERT_EQ(fieldOf(written.error()), field);
        return written.error();
    };
    // A class the reference does not have, which is also a script's class_name.
    ASSERT_EQ(refused({"res://a.tscn", "NoSuchClass", "Root"}, "root_type").code, 400);
    ASSERT_EQ(refused({"res://a.tscn", "@GlobalScope", "Root"}, "root_type").code, 400);
    // Not a Node, and a Node class the engine cannot construct.
    ASSERT_TRUE(refused({"res://a.tscn", "Resource", "Root"}, "root_type").message.find("inherit Node") !=
                std::string::npos);
    ASSERT_TRUE(refused({"res://a.tscn", "CanvasItem", "Root"}, "root_type").message.find("abstract") !=
                std::string::npos);
    refused({"res://a.tscn", "CollisionObject2D", "Root"}, "root_type");
    refused({"res://a.tscn", "Node2D", ""}, "root_name");
    refused({"res://a.tscn", "Node2D", "a/b"}, "root_name");
    refused({"res://a.tscn", "Node2D", "a\\b"}, "root_name");
    refused({"res://a.scn", "Node2D", "Root"}, "scene_path");
    refused({"res://a.TSCN", "Node2D", "Root"}, "scene_path");
    refused({"res://../outside.tscn", "Node2D", "Root"}, "scene_path");
    ASSERT_TRUE(!project.exists("a.tscn"));
    ASSERT_TRUE(!project.exists("a.scn"));
    ASSERT_TRUE(!std::filesystem::exists(project.root().parent_path() / "outside.tscn"));
}

// The registry routes a call with no editor here, the answer says what was
// written and against which reference, and a dry run writes nothing.
void tool_offline_route() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    registry.setIpcClient(nullptr);
    const auto* tool = registry.getTool("scene_create");
    ASSERT_TRUE(tool != nullptr);
    const auto& modes = tool->capability.modes;
    ASSERT_TRUE(std::find(modes.begin(), modes.end(), "offline_fallback") != modes.end());
    ScopedNewSceneProject project(
        "config_version=5\n\n[application]\n\nconfig/features=PackedStringArray(\"4.5\", \"Forward Plus\")\n");

    const auto preview = registry.callTool(
        "scene_create", {{"scene_path", "res://world.tscn"}, {"root_type", "Node3D"}, {"dry_run", true}});
    ASSERT_TRUE(!preview.isError);
    ASSERT_TRUE(!project.exists("world.tscn"));

    const auto created = registry.callTool(
        "scene_create", {{"scene_path", "res://world.tscn"}, {"root_type", "Node3D"}, {"root_name", "World"}});
    ASSERT_TRUE(!created.isError);
    const auto answer = payloadOf(created);
    ASSERT_EQ(answer["saved"], true);
    ASSERT_EQ(answer["opened"], false);
    ASSERT_EQ(answer["execution_mode"], "offline_fallback");
    ASSERT_EQ(answer["scene_path"], "res://world.tscn");
    ASSERT_EQ(answer["root_type"], "Node3D");
    ASSERT_EQ(answer["replaced"], false);
    ASSERT_EQ(answer["uid_registered"], false);
    ASSERT_EQ(answer["type_check"]["source"], "class_reference");
    ASSERT_EQ(answer["type_check"]["project_features_version"], "4.5");
    ASSERT_EQ(answer["type_check"]["api_version_matches_project_features"], false);
    ASSERT_TRUE(answer["limitation"].get<std::string>().find("Godot 4.5") != std::string::npos);
    ASSERT_TRUE(!answer.contains("root_name_requested"));
    ASSERT_TRUE(project.read("world.tscn").find("uid=\"" + answer["uid"].get<std::string>() + "\"") !=
                std::string::npos);
    ASSERT_TRUE(project.read("world.tscn").find("[node name=\"World\" type=\"Node3D\"]") != std::string::npos);

    const auto renamed = registry.callTool("scene_create", {{"scene_path", "res://hud.tscn"},
                                                            {"root_type", "CanvasLayer"},
                                                            {"root_name", "H.U.D"}});
    ASSERT_TRUE(!renamed.isError);
    ASSERT_EQ(payloadOf(renamed)["root_name"], "H_U_D");
    ASSERT_EQ(payloadOf(renamed)["root_name_requested"], "H.U.D");
}

struct Register {
    Register() {
        registerTest("NewScene.TextAndNames", text_and_names);
        registerTest("NewScene.WritesAScene", writes_a_scene);
        registerTest("NewScene.RefusesWhatTheEditorRefuses", refuses_what_the_editor_refuses);
        registerTest("NewScene.ToolOfflineRoute", tool_offline_route);
    }
} registrar;

} // namespace
