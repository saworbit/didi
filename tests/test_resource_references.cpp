#include "didi/common/json.hpp"
#include "didi/common/types.hpp"
#include "didi/mcp/tool_registry.hpp"
#include "didi/offline/resource_indexer.hpp"

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

namespace didi {
namespace mcp {
CallToolResult handleResourceCreate(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
}
}

namespace {

using didi::json;

// A project on disk, because resource_create resolves paths and looks
// references up in the project index, both of which are rooted at the working
// directory.
class ProjectFixture {
public:
    explicit ProjectFixture(const std::string& suffix) {
        m_previous = std::filesystem::current_path();
        m_root = std::filesystem::temp_directory_path() /
                 ("didi-resource-refs-" + suffix + "-" +
                  std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(m_root / "art");
        std::ofstream(m_root / "project.godot", std::ios::binary) << "config_version=5\n";
        std::ofstream(m_root / "art" / "tiles.png", std::ios::binary) << "not really a png";
        std::filesystem::current_path(m_root);
        didi::offline::ResourceIndexer::invalidateSharedIndex();
    }

    ~ProjectFixture() {
        std::error_code ignored;
        std::filesystem::current_path(m_previous, ignored);
        std::filesystem::remove_all(m_root, ignored);
        didi::offline::ResourceIndexer::invalidateSharedIndex();
    }

    std::string read(const std::string& relative) const {
        std::ifstream input(m_root / std::filesystem::path(relative), std::ios::binary);
        std::ostringstream contents;
        contents << input.rdbuf();
        return contents.str();
    }

    bool exists(const std::string& relative) const {
        std::error_code ignored;
        return std::filesystem::exists(m_root / std::filesystem::path(relative), ignored);
    }

private:
    std::filesystem::path m_root;
    std::filesystem::path m_previous;
};

didi::mcp::CallToolResult create(const json& args) {
    return didi::mcp::handleResourceCreate(args, nullptr);
}

json payloadOf(const didi::mcp::CallToolResult& result) {
    for (const auto& item : result.content) {
        if (item.type == "text") {
            try {
                return json::parse(item.text);
            } catch (const json::parse_error&) {
                return json::object();
            }
        }
    }
    return json::object();
}

std::string textOf(const didi::mcp::CallToolResult& result) {
    for (const auto& item : result.content) {
        if (item.type == "text") return item.text;
    }
    return {};
}

// The case #380 reported as impossible: a TileSet needs an atlas source that
// points at a texture, which is one [sub_resource] and one [ext_resource] in
// the same file, and there was no way to say either.
void writes_a_tileset_with_an_atlas_source_and_a_texture() {
    ProjectFixture project("tileset");
    const auto result = create({
        {"save_path", "res://art/arena_tileset.tres"},
        {"resource_type", "TileSet"},
        {"sub_resources", json::array({
            {{"id", "TileSetAtlasSource_1"},
             {"resource_type", "TileSetAtlasSource"},
             {"properties", json::array({
                 {{"name", "texture"},
                  {"value", {{"type", "ExtResource"}, {"path", "res://art/tiles.png"}}}},
                 {{"name", "texture_region_size"},
                  {"value", {{"type", "Vector2i"}, {"x", 32}, {"y", 32}}}}
             })}}
        })},
        {"properties", json::array({
            {{"name", "tile_size"}, {"value", {{"type", "Vector2i"}, {"x", 32}, {"y", 32}}}},
            {{"name", "sources/0"},
             {"value", {{"type", "SubResource"}, {"id", "TileSetAtlasSource_1"}}}}
        })}
    });
    ASSERT_TRUE(!result.isError);

    const auto contents = project.read("art/arena_tileset.tres");
    // load_steps counts the externals, the sub-resources and the resource
    // itself. Godot uses it to size its load, and it had nowhere to come from.
    ASSERT_TRUE(contents.find("[gd_resource type=\"TileSet\" load_steps=3 format=3]") !=
                std::string::npos);
    ASSERT_TRUE(contents.find(
        "[ext_resource type=\"Texture2D\" path=\"res://art/tiles.png\" id=\"1_didi\"]") !=
                std::string::npos);
    ASSERT_TRUE(contents.find("[sub_resource type=\"TileSetAtlasSource\" id=\"TileSetAtlasSource_1\"]") !=
                std::string::npos);
    ASSERT_TRUE(contents.find("texture = ExtResource(\"1_didi\")") != std::string::npos);
    ASSERT_TRUE(contents.find("sources/0 = SubResource(\"TileSetAtlasSource_1\")") !=
                std::string::npos);
    // The sub-resource has to be declared before the [resource] block that
    // names it, or Godot resolves it against nothing.
    ASSERT_TRUE(contents.find("[sub_resource") < contents.find("[resource]"));
    ASSERT_TRUE(contents.find("[ext_resource") < contents.find("[sub_resource"));

    const auto payload = payloadOf(result);
    ASSERT_EQ(payload.value("load_steps", 0), 3);
    ASSERT_EQ(payload["external_references"].size(), size_t(1));
    ASSERT_EQ(payload["sub_resources_written"].size(), size_t(1));
}

// Break caught: one header entry per file, not per mention. Two entries for one
// path is a file Godot loads twice and a load_steps that does not match.
void gives_one_header_entry_to_a_path_named_twice() {
    ProjectFixture project("shared");
    const auto result = create({
        {"save_path", "res://art/two.tres"},
        {"resource_type", "Resource"},
        {"properties", json::array({
            {{"name", "a"}, {"value", {{"type", "ExtResource"}, {"path", "res://art/tiles.png"}}}},
            {{"name", "b"}, {"value", {{"type", "ExtResource"}, {"path", "res://art/tiles.png"}}}}
        })}
    });
    ASSERT_TRUE(!result.isError);
    const auto contents = project.read("art/two.tres");
    ASSERT_EQ(contents.find("[ext_resource"), contents.rfind("[ext_resource"));
    ASSERT_TRUE(contents.find("a = ExtResource(\"1_didi\")") != std::string::npos);
    ASSERT_TRUE(contents.find("b = ExtResource(\"1_didi\")") != std::string::npos);
    ASSERT_EQ(payloadOf(result).value("load_steps", 0), 2);
}

// A reference to a file that is not there produces a resource that loads with
// nothing in that slot, which is the silent wrong write this writer exists to
// refuse.
void refuses_a_reference_to_a_file_that_is_not_in_the_project() {
    ProjectFixture project("missing");
    const auto result = create({
        {"save_path", "res://art/broken.tres"},
        {"resource_type", "Resource"},
        {"properties", json::array({
            {{"name", "t"}, {"value", {{"type", "ExtResource"}, {"path", "res://art/absent.png"}}}}
        })}
    });
    ASSERT_TRUE(result.isError);
    ASSERT_TRUE(textOf(result).find("not in this project") != std::string::npos);
    // Nothing rendered means nothing written.
    ASSERT_TRUE(!project.exists("art/broken.tres"));
}

void refuses_a_reference_outside_the_project() {
    ProjectFixture project("outside");
    const auto result = create({
        {"save_path", "res://art/outside.tres"},
        {"resource_type", "Resource"},
        {"properties", json::array({
            {{"name", "t"}, {"value", {{"type", "ExtResource"}, {"path", "/etc/passwd"}}}}
        })}
    });
    ASSERT_TRUE(result.isError);
    ASSERT_TRUE(textOf(result).find("res://") != std::string::npos);
    ASSERT_TRUE(!project.exists("art/outside.tres"));
}

void refuses_a_sub_resource_id_that_was_never_declared() {
    ProjectFixture project("undeclared");
    const auto result = create({
        {"save_path", "res://art/undeclared.tres"},
        {"resource_type", "Resource"},
        {"properties", json::array({
            {{"name", "t"}, {"value", {{"type", "SubResource"}, {"id", "Nope"}}}}
        })}
    });
    ASSERT_TRUE(result.isError);
    ASSERT_TRUE(textOf(result).find("not declared above it") != std::string::npos);
    ASSERT_TRUE(!project.exists("art/undeclared.tres"));
}

// Break caught: Godot resolves a SubResource against the blocks it has already
// read, so a reference to one declared further down loads as null rather than
// failing. Accepting it would write a file that reports success and is wrong.
void refuses_a_sub_resource_naming_one_declared_below_it() {
    ProjectFixture project("forward");
    const auto result = create({
        {"save_path", "res://art/forward.tres"},
        {"resource_type", "Resource"},
        {"sub_resources", json::array({
            {{"id", "First"}, {"resource_type", "Resource"},
             {"properties", json::array({
                 {{"name", "x"}, {"value", {{"type", "SubResource"}, {"id", "Second"}}}}
             })}},
            {{"id", "Second"}, {"resource_type", "Resource"}}
        })},
        {"properties", json::array()}
    });
    ASSERT_TRUE(result.isError);
    ASSERT_TRUE(textOf(result).find("not declared above it") != std::string::npos);
    ASSERT_TRUE(!project.exists("art/forward.tres"));
}

// A sub-resource may name one declared before it, which is the ordering the
// argument's array shape exists to carry.
void lets_a_sub_resource_name_one_declared_above_it() {
    ProjectFixture project("backward");
    const auto result = create({
        {"save_path", "res://art/backward.tres"},
        {"resource_type", "Resource"},
        {"sub_resources", json::array({
            {{"id", "First"}, {"resource_type", "Resource"}},
            {{"id", "Second"}, {"resource_type", "Resource"},
             {"properties", json::array({
                 {{"name", "x"}, {"value", {{"type", "SubResource"}, {"id", "First"}}}}
             })}}
        })},
        {"properties", json::array()}
    });
    ASSERT_TRUE(!result.isError);
    const auto contents = project.read("art/backward.tres");
    ASSERT_TRUE(contents.find("x = SubResource(\"First\")") != std::string::npos);
    ASSERT_EQ(payloadOf(result).value("load_steps", 0), 3);
}

// A resource with no references is written exactly as it was before, header
// included. load_steps of 1 is what Godot omits, so this omits it too.
void leaves_a_plain_resource_exactly_as_it_was() {
    ProjectFixture project("plain");
    const auto result = create({
        {"save_path", "res://art/plain.tres"},
        {"resource_type", "CircleShape2D"},
        {"properties", json::array({{{"name", "radius"}, {"value", 8.0}}})}
    });
    ASSERT_TRUE(!result.isError);
    const auto contents = project.read("art/plain.tres");
    ASSERT_EQ(contents, std::string("[gd_resource type=\"CircleShape2D\" format=3]\n\n"
                                    "[resource]\nradius = 8.0\n"));
}

void refuses_a_malformed_sub_resources_argument() {
    ProjectFixture project("malformed");
    ASSERT_TRUE(create({{"save_path", "res://art/m1.tres"}, {"resource_type", "Resource"},
                        {"sub_resources", "not an array"}}).isError);
    ASSERT_TRUE(create({{"save_path", "res://art/m2.tres"}, {"resource_type", "Resource"},
                        {"sub_resources", json::array({json::object()})}}).isError);
    ASSERT_TRUE(create({{"save_path", "res://art/m3.tres"}, {"resource_type", "Resource"},
                        {"sub_resources", json::array({{{"id", "A"}}})}}).isError);
    // The same id twice would give two blocks a reference cannot choose between.
    ASSERT_TRUE(create({{"save_path", "res://art/m4.tres"}, {"resource_type", "Resource"},
                        {"sub_resources", json::array({
                            {{"id", "A"}, {"resource_type", "Resource"}},
                            {{"id", "A"}, {"resource_type", "Resource"}}})}}).isError);
    ASSERT_TRUE(!project.exists("art/m1.tres"));
    ASSERT_TRUE(!project.exists("art/m4.tres"));
}

struct Register {
    Register() {
        registerTest("resource_references.tileset_with_atlas_and_texture",
                     writes_a_tileset_with_an_atlas_source_and_a_texture);
        registerTest("resource_references.shared_path_gets_one_entry",
                     gives_one_header_entry_to_a_path_named_twice);
        registerTest("resource_references.refuses_missing_file",
                     refuses_a_reference_to_a_file_that_is_not_in_the_project);
        registerTest("resource_references.refuses_path_outside_project",
                     refuses_a_reference_outside_the_project);
        registerTest("resource_references.refuses_undeclared_id",
                     refuses_a_sub_resource_id_that_was_never_declared);
        registerTest("resource_references.refuses_forward_reference",
                     refuses_a_sub_resource_naming_one_declared_below_it);
        registerTest("resource_references.allows_backward_reference",
                     lets_a_sub_resource_name_one_declared_above_it);
        registerTest("resource_references.plain_resource_unchanged",
                     leaves_a_plain_resource_exactly_as_it_was);
        registerTest("resource_references.refuses_malformed_argument",
                     refuses_a_malformed_sub_resources_argument);
    }
} registrar;

} // namespace
