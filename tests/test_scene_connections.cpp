#include "didi/mcp/tool_registry.hpp"
#include "didi/offline/resource_indexer.hpp"
#include "didi/offline/scene_connections.hpp"

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
using didi::offline::SceneConnectionRequest;
using didi::offline::connectSignalInScene;

const std::string kScene =
    "[gd_scene load_steps=5 format=3]\n\n"
    "[ext_resource type=\"Script\" path=\"res://main.gd\" id=\"1\"]\n"
    "[ext_resource type=\"Script\" path=\"res://pad.gd\" id=\"2\"]\n"
    "[ext_resource type=\"Script\" path=\"res://child.gd\" id=\"3\"]\n"
    "[ext_resource type=\"PackedScene\" path=\"res://other.tscn\" id=\"4\"]\n\n"
    "[sub_resource type=\"GDScript\" id=\"GDScript_1\"]\nscript/source = \"extends Node\"\n\n"
    "[node name=\"Main\" type=\"Node2D\"]\nscript = ExtResource(\"1\")\n\n"
    "[node name=\"LandingPad\" type=\"Area2D\" parent=\".\"]\nscript = ExtResource(\"2\")\n\n"
    "[node name=\"Clock\" type=\"Timer\" parent=\".\"]\n\n"
    "[node name=\"Group\" type=\"Node\" parent=\".\"]\n\n"
    "[node name=\"Deep\" type=\"Timer\" parent=\"Group\"]\n\n"
    "[node name=\"Heir\" type=\"Node\" parent=\".\"]\nscript = ExtResource(\"3\")\n\n"
    "[node name=\"Inst\" parent=\".\" instance=ExtResource(\"4\")]\n\n"
    "[node name=\"Built\" type=\"Node\" parent=\".\"]\nscript = SubResource(\"GDScript_1\")\n";

class ScopedSceneProject final {
public:
    ScopedSceneProject()
        : m_original(std::filesystem::current_path()),
          m_root(std::filesystem::temp_directory_path() /
                 ("didi-scene-connections-" +
                  std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
        std::filesystem::create_directories(m_root);
        std::filesystem::current_path(m_root);
        write("project.godot", "config_version=5\n");
        write("main.gd",
              "extends Node2D\n\n"
              "func _on_landing_pad_landed_safely(speed: float) -> void:\n\tpass\n\n"
              "func _on_timeout() -> void:\n\tpass\n\n"
              "func _on_two(a, b) -> void:\n\tpass\n\n"
              "func _on_optional(a = 1, b := Vector2(1, 2)) -> void:\n\tpass\n");
        write("pad.gd", "extends Area2D\n\nsignal landed_safely(speed: float)\n\nfunc _on_landed() -> void:\n\tpass\n");
        write("base.gd", "class_name BaseThing\nextends Node\n\nfunc inherited_method() -> void:\n\tpass\n");
        write("child.gd", "extends BaseThing\n");
        write("other.tscn", "[gd_scene format=3]\n\n[node name=\"Other\" type=\"Node\"]\n\n"
                            "[node name=\"Child\" type=\"Timer\" parent=\".\"]\n");
        write("main.tscn", kScene);
        didi::offline::ResourceIndexer::invalidateSharedIndex();
    }
    ~ScopedSceneProject() {
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
    const std::filesystem::path& root() const { return m_root; }

private:
    std::filesystem::path m_original;
    std::filesystem::path m_root;
};

SceneConnectionRequest connection(std::string emitter, std::string signal, std::string target, std::string method,
                                  int64_t flags = 2) {
    return {"res://main.tscn", std::move(emitter), std::move(signal), std::move(target), std::move(method), flags};
}

std::string codeOf(const didi::Error& error) { return error.data.is_object() ? error.data.value("code", "") : ""; }

// What the editor writes, measured on 4.5.1, 4.6.2 and 4.7.2: no flags for
// CONNECT_PERSIST alone, flags=N otherwise, paths from the scene root.
void connection_line() {
    ASSERT_EQ(didi::offline::connectionLine("landed_safely", "LandingPad", ".", "_on_landing_pad_landed_safely", 2),
              "[connection signal=\"landed_safely\" from=\"LandingPad\" to=\".\" method=\"_on_landing_pad_landed_safely\"]");
    ASSERT_EQ(didi::offline::connectionLine("timeout", "Group/Deep", ".", "_on_timeout", 6),
              "[connection signal=\"timeout\" from=\"Group/Deep\" to=\".\" method=\"_on_timeout\" flags=6]");
}

// Break caught: with no editor attached signal_connect answered 503, so a
// scene's signals could not be wired before anything opened it (#1387).
void writes_what_it_can_judge() {
    ScopedSceneProject project;
    // A script's signal to a script's method: both arities are known.
    auto first = connectSignalInScene(project.root(), connection("LandingPad", "landed_safely", ".",
                                                                 "_on_landing_pad_landed_safely"));
    ASSERT_TRUE(first.isOk());
    ASSERT_EQ(first.value().from, "LandingPad");
    ASSERT_EQ(first.value().to, ".");
    ASSERT_TRUE(first.value().arity_checked);
    ASSERT_EQ(project.read("main.tscn"),
              kScene + "\n[connection signal=\"landed_safely\" from=\"LandingPad\" to=\".\" "
                       "method=\"_on_landing_pad_landed_safely\"]\n");
    // An engine signal, named the ways the live tool takes, with flags; it
    // goes after the connection already there.
    auto second = connectSignalInScene(project.root(), connection("/root/Main/Group/Deep", "timeout", "Main",
                                                                  "_on_timeout", 3));
    ASSERT_TRUE(second.isOk());
    ASSERT_EQ(second.value().from, "Group/Deep");
    ASSERT_TRUE(!second.value().arity_checked);
    ASSERT_TRUE(project.read("main.tscn").find(
                    "method=\"_on_landing_pad_landed_safely\"]\n[connection signal=\"timeout\" "
                    "from=\"Group/Deep\" to=\".\" method=\"_on_timeout\" flags=3]\n") != std::string::npos);
    // An engine class's own method, and one inherited through class_name.
    ASSERT_TRUE(connectSignalInScene(project.root(), connection("Clock", "timeout", "Clock", "stop")).isOk());
    ASSERT_TRUE(connectSignalInScene(project.root(), connection("Clock", "timeout", "Heir", "inherited_method")).isOk());
    // Defaults make an argument optional, a bracket inside one included.
    ASSERT_TRUE(connectSignalInScene(project.root(), connection("Clock", "timeout", ".", "_on_optional")).isOk());
}

void refuses_what_it_cannot_judge() {
    ScopedSceneProject project;
    const auto refused = [&](const SceneConnectionRequest& request) {
        auto written = connectSignalInScene(project.root(), request);
        ASSERT_TRUE(written.isErr());
        ASSERT_EQ(project.read("main.tscn"), kScene);
        return written.error();
    };
    ASSERT_EQ(codeOf(refused(connection("Clock", "nope_signal", ".", "_on_timeout"))), "declared_signal_not_found");
    ASSERT_EQ(codeOf(refused(connection("Clock", "timeout", ".", "nope_method"))), "target_method_not_found");
    ASSERT_EQ(refused(connection("Missing", "timeout", ".", "_on_timeout")).code, 404);
    // One argument passed, two required, as the live route refuses it.
    ASSERT_EQ(codeOf(refused(connection("LandingPad", "landed_safely", ".", "_on_two"))),
              "signal_target_arity_incompatible");
    // What the file does not declare is not guessed at.
    ASSERT_EQ(codeOf(refused(connection("Inst/Child", "timeout", ".", "_on_timeout"))), "connection_not_judged");
    ASSERT_EQ(codeOf(refused(connection("Inst", "ready", ".", "_on_timeout"))), "connection_not_judged");
    ASSERT_EQ(codeOf(refused(connection("Clock", "timeout", "Built", "_ready"))), "connection_not_judged");
    ASSERT_TRUE(refused(connection("../Elsewhere", "timeout", ".", "_on_timeout")).code >= 400);
    auto not_a_scene = connection("Clock", "timeout", ".", "_on_timeout");
    not_a_scene.scene_path = "res://main.gd";
    ASSERT_EQ(refused(not_a_scene).code, 400);

    // A second identical connection, which the engine would make twice.
    ASSERT_TRUE(connectSignalInScene(project.root(), connection("Clock", "timeout", ".", "_on_timeout")).isOk());
    auto again = connectSignalInScene(project.root(), connection("Clock", "timeout", "./", "_on_timeout"));
    ASSERT_TRUE(again.isErr());
    ASSERT_EQ(codeOf(again.error()), "signal_connection_already_exists");
}

void keeps_the_file_layout() {
    ScopedSceneProject project;
    // CRLF stays CRLF, and a connection goes before an [editable] section.
    std::string crlf;
    for (const char c : kScene + "\n[editable path=\"Inst\"]\n") crlf += c == '\n' ? std::string("\r\n") : std::string(1, c);
    project.write("main.tscn", crlf);
    ASSERT_TRUE(connectSignalInScene(project.root(), connection("Clock", "timeout", ".", "_on_timeout")).isOk());
    const auto text = project.read("main.tscn");
    ASSERT_TRUE(text.find("\r\n\r\n[connection signal=\"timeout\" from=\"Clock\" to=\".\" method=\"_on_timeout\"]"
                          "\r\n\r\n[editable path=\"Inst\"]\r\n") != std::string::npos);
    for (size_t at = 0; (at = text.find('\n', at)) != std::string::npos; ++at) ASSERT_TRUE(at > 0 && text[at - 1] == '\r');
}

json payloadOf(const didi::mcp::CallToolResult& result) { return json::parse(result.content[0].text); }

// The tool with no editor attached: scene_path is required, the answer says
// what it wrote, and the modes it declares let the registry route it here.
void tool_offline_route() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    registry.setIpcClient(nullptr);
    const auto* tool = registry.getTool("signal_connect");
    ASSERT_TRUE(tool != nullptr);
    const auto& modes = tool->capability.modes;
    ASSERT_TRUE(std::find(modes.begin(), modes.end(), "offline_fallback") != modes.end());
    ScopedSceneProject project;
    const json call = {{"emitter_node", "LandingPad"}, {"signal_name", "landed_safely"},
                       {"target_node", "."}, {"target_method", "_on_landing_pad_landed_safely"}};
    const auto missing = registry.callTool("signal_connect", call);
    ASSERT_TRUE(missing.isError);
    ASSERT_EQ(payloadOf(missing)["error"]["data"]["field"], "scene_path");
    auto with_scene = call;
    with_scene["scene_path"] = "res://main.tscn";
    const auto written = registry.callTool("signal_connect", with_scene);
    ASSERT_TRUE(!written.isError);
    const auto answer = payloadOf(written);
    ASSERT_EQ(answer["connected"], true);
    ASSERT_EQ(answer["flags"], 2);
    ASSERT_EQ(answer["undo_redo_registered"], false);
    ASSERT_EQ(answer["rollback"], "none");
    ASSERT_EQ(answer["execution_mode"], "offline_fallback");
    ASSERT_EQ(answer["written_to"], "res://main.tscn");
    ASSERT_EQ(answer["connection"]["from"], "LandingPad");
    ASSERT_EQ(answer["arity_checked"], true);
    ASSERT_TRUE(project.read("main.tscn").find(answer["line_written"].get<std::string>()) != std::string::npos);
}

struct Register {
    Register() {
        registerTest("SceneConnections.Line", connection_line);
        registerTest("SceneConnections.WritesWhatItCanJudge", writes_what_it_can_judge);
        registerTest("SceneConnections.RefusesWhatItCannotJudge", refuses_what_it_cannot_judge);
        registerTest("SceneConnections.KeepsTheFileLayout", keeps_the_file_layout);
        registerTest("SceneConnections.ToolOfflineRoute", tool_offline_route);
    }
} registrar;

} // namespace
