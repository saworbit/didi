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
#include <vector>

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
        {"resource_type", "StandardMaterial3D"},
        {"properties", json::array({
            {{"name", "albedo_texture"},
             {"value", {{"type", "ExtResource"}, {"path", "res://art/tiles.png"}}}},
            {{"name", "normal_texture"},
             {"value", {{"type", "ExtResource"}, {"path", "res://art/tiles.png"}}}}
        })}
    });
    ASSERT_TRUE(!result.isError);
    const auto contents = project.read("art/two.tres");
    ASSERT_EQ(contents.find("[ext_resource"), contents.rfind("[ext_resource"));
    ASSERT_TRUE(contents.find("albedo_texture = ExtResource(\"1_didi\")") != std::string::npos);
    ASSERT_TRUE(contents.find("normal_texture = ExtResource(\"1_didi\")") != std::string::npos);
    ASSERT_EQ(payloadOf(result).value("load_steps", 0), 2);
}

// A reference to a file that is not there produces a resource that loads with
// nothing in that slot, which is the silent wrong write this writer exists to
// refuse.
void refuses_a_reference_to_a_file_that_is_not_in_the_project() {
    ProjectFixture project("missing");
    const auto result = create({
        {"save_path", "res://art/broken.tres"},
        {"resource_type", "StandardMaterial3D"},
        {"properties", json::array({
            {{"name", "albedo_texture"},
             {"value", {{"type", "ExtResource"}, {"path", "res://art/absent.png"}}}}
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
        {"resource_type", "StandardMaterial3D"},
        {"properties", json::array({
            {{"name", "albedo_texture"},
             {"value", {{"type", "ExtResource"}, {"path", "/etc/passwd"}}}}
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
        {"resource_type", "Material"},
        {"properties", json::array({
            {{"name", "next_pass"}, {"value", {{"type", "SubResource"}, {"id", "Nope"}}}}
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
            {{"id", "First"}, {"resource_type", "Material"},
             {"properties", json::array({
                 {{"name", "next_pass"}, {"value", {{"type", "SubResource"}, {"id", "Second"}}}}
             })}},
            {{"id", "Second"}, {"resource_type", "Material"}}
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
            {{"id", "First"}, {"resource_type", "Material"}},
            {{"id", "Second"}, {"resource_type", "Material"},
             {"properties", json::array({
                 {{"name", "next_pass"}, {"value", {{"type", "SubResource"}, {"id", "First"}}}}
             })}}
        })},
        {"properties", json::array()}
    });
    ASSERT_TRUE(!result.isError);
    const auto contents = project.read("art/backward.tres");
    ASSERT_TRUE(contents.find("next_pass = SubResource(\"First\")") != std::string::npos);
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

// Break caught: whatever names `properties` carried were written into the
// [resource] block and reported in properties_written. Godot drops a property
// the type does not have when it loads the file, silently, and no tool in the
// surface would show the loss: resource_inspect reports type, size, uid and
// dependencies, and no properties.
void refuses_properties_the_type_does_not_declare() {
    ProjectFixture project("undeclared-properties");
    const auto result = create({
        {"save_path", "res://art/r1.tres"},
        {"resource_type", "Resource"},
        {"properties", json::array({
            {{"name", "a"}, {"value", 1}},
            {{"name", "b"}, {"value", "two"}}
        })}
    });
    ASSERT_TRUE(result.isError);
    ASSERT_TRUE(textOf(result).find("'a'") != std::string::npos);
    ASSERT_TRUE(textOf(result).find("'b'") != std::string::npos);
    ASSERT_TRUE(textOf(result).find("Resource") != std::string::npos);
    // Checked before anything is rendered, so nothing is written.
    ASSERT_TRUE(!project.exists("art/r1.tres"));

    // A name the type does declare still goes through, and the result says the
    // check ran.
    const auto declared = create({
        {"save_path", "res://art/r2.tres"},
        {"resource_type", "Resource"},
        {"properties", json::array({{{"name", "resource_name"}, {"value", "arena"}}})}
    });
    ASSERT_TRUE(!declared.isError);
    ASSERT_EQ(payloadOf(declared)["property_check"]["checked"], true);

    // A sub-resource gets the same check, named by its id.
    const auto sub = create({
        {"save_path", "res://art/r3.tres"},
        {"resource_type", "Resource"},
        {"sub_resources", json::array({
            {{"id", "Inner_1"}, {"resource_type", "Material"},
             {"properties", json::array({{{"name", "nope"}, {"value", 1}}})}}
        })},
        {"properties", json::array()}
    });
    ASSERT_TRUE(sub.isError);
    ASSERT_TRUE(textOf(sub).find("Inner_1") != std::string::npos);
    ASSERT_TRUE(!project.exists("art/r3.tres"));
}

// The API dump lists only the inspector-visible set, so the names Godot stores
// without declaring -- sources/0 on a TileSet, _data on a Curve -- have to keep
// working. They are reported rather than refused, and a type the reference does
// not carry at all is not checked rather than refused.
void writes_storage_only_names_and_gates_an_unknown_type() {
    ProjectFixture project("storage-only");
    const auto stored = create({
        {"save_path", "res://art/curve.tres"},
        {"resource_type", "Curve"},
        {"properties", json::array({
            {{"name", "bake_resolution"}, {"value", 100}},
            {{"name", "_data"}, {"value", json::array()}}
        })}
    });
    ASSERT_TRUE(!stored.isError);
    const auto payload = payloadOf(stored);
    ASSERT_EQ(payload["property_check"]["checked"], true);
    ASSERT_EQ(payload["property_check"]["not_declared_but_written"].size(), size_t(1));
    ASSERT_EQ(payload["property_check"]["not_declared_but_written"][0], "_data");

    // Break caught: a type Godot does not know was written and reported as
    // created, and the file could not be loaded at all. The check that refuses
    // one undeclared property was skipped entirely for the larger mistake, and
    // the reason it was skipped came back as a nested field on a success
    // nothing forces a caller to read (#465).
    const json unknown_args = {
        {"save_path", "res://art/custom.tres"},
        {"resource_type", "MyCustomResourceClass"},
        {"properties", json::array({{{"name", "whatever"}, {"value", 1}}})}
    };
    const auto refused = create(unknown_args);
    ASSERT_TRUE(refused.isError);
    const auto envelope = json::parse(refused.content[0].text);
    ASSERT_EQ(envelope["error"]["code"], 400);
    ASSERT_TRUE(envelope["error"]["message"].get<std::string>().find("allow_unknown_type") !=
                std::string::npos);
    ASSERT_TRUE(!std::filesystem::exists("art/custom.tres"));

    // The escape hatch stays open, because a class_name script or a GDExtension
    // type is not in the shipped reference either.
    json allowed_args = unknown_args;
    allowed_args["allow_unknown_type"] = true;
    const auto allowed = create(allowed_args);
    ASSERT_TRUE(!allowed.isError);
    ASSERT_EQ(payloadOf(allowed)["property_check"]["checked"], false);
    ASSERT_EQ(payloadOf(allowed)["property_check"]["allowed_by"], "allow_unknown_type");
}

// JSON has one shape for a vector and Godot has two types for it, so the shape
// of the object used to pick the literal. Every integer-vector slot on the
// surface got a Vector2, and Godot drops one of those when it loads the file:
// resource_create reported created_offline, property_check reported checked,
// and the TileSet painted nothing (#730).
void writes_a_vector_as_the_type_the_property_declares() {
    ProjectFixture project("declared-types");

    // tile_size is Vector2i. The same {x, y} on a RectangleShape2D is a
    // Vector2, which is the control: the type decides, not the shape.
    const auto integer_vector = create({
        {"save_path", "res://art/tiles.tres"},
        {"resource_type", "TileSet"},
        {"properties", {{"tile_size", {{"x", 16}, {"y", 16}}}}}
    });
    ASSERT_TRUE(!integer_vector.isError);
    ASSERT_TRUE(project.read("art/tiles.tres").find("tile_size = Vector2i(16, 16)") !=
                std::string::npos);
    // The correction is reported, because a file that is not what the caller
    // described is worth one line of the answer.
    ASSERT_EQ(payloadOf(integer_vector)["property_check"]["written_as_declared_type"]["tile_size"],
              "Vector2i");

    const auto real_vector = create({
        {"save_path", "res://art/box.tres"},
        {"resource_type", "RectangleShape2D"},
        {"properties", {{"size", {{"x", 32}, {"y", 8}}}}}
    });
    ASSERT_TRUE(!real_vector.isError);
    ASSERT_TRUE(project.read("art/box.tres").find("size = Vector2(32, 8)") != std::string::npos);
    ASSERT_TRUE(!payloadOf(real_vector)["property_check"].contains("written_as_declared_type"));

    // A sub-resource is written by the same writer against its own type.
    const auto nested = create({
        {"save_path", "res://art/atlas.tres"},
        {"resource_type", "TileSet"},
        {"sub_resources", json::array({
            {{"id", "Atlas_1"},
             {"resource_type", "TileSetAtlasSource"},
             {"properties", json::array({
                 {{"name", "texture_region_size"}, {"value", {{"x", 16}, {"y", 16}}}}
             })}}
        })},
        {"properties", json::array({
            {{"name", "sources/0"}, {"value", {{"type", "SubResource"}, {"id", "Atlas_1"}}}}
        })}
    });
    ASSERT_TRUE(!nested.isError);
    ASSERT_TRUE(project.read("art/atlas.tres").find("texture_region_size = Vector2i(16, 16)") !=
                std::string::npos);
    ASSERT_EQ(payloadOf(nested)["sub_resource_property_checks"]["Atlas_1"]
                        ["written_as_declared_type"]["texture_region_size"],
              "Vector2i");
}

// The two ways a caller can still end up with a value the property cannot hold.
void refuses_a_value_the_declared_type_cannot_hold() {
    ProjectFixture project("declared-type-refusals");

    // Naming the wrong type explicitly. The refusal names the property, the
    // declared type and what to send instead.
    const auto contradiction = create({
        {"save_path", "res://art/wrong.tres"},
        {"resource_type", "TileSet"},
        {"properties", {{"tile_size", {{"type", "Vector2"}, {"x", 16}, {"y", 16}}}}}
    });
    ASSERT_TRUE(contradiction.isError);
    const auto message = textOf(contradiction);
    ASSERT_TRUE(message.find("tile_size") != std::string::npos);
    ASSERT_TRUE(message.find("Vector2i") != std::string::npos);
    ASSERT_TRUE(!project.exists("art/wrong.tres"));

    // A fraction in an integer vector. Truncating it would be a second silent
    // difference between what was asked for and what was written.
    const auto fractional = create({
        {"save_path", "res://art/fraction.tres"},
        {"resource_type", "TileSet"},
        {"properties", {{"tile_size", {{"x", 16.5}, {"y", 16}}}}}
    });
    ASSERT_TRUE(fractional.isError);
    ASSERT_TRUE(textOf(fractional).find("whole number") != std::string::npos);
    ASSERT_TRUE(!project.exists("art/fraction.tres"));
}

// The composite packed arrays are one flat run of components (#765).
//
// A nested constructor is not a near miss. Godot's text parser answers
// "Expected float in constructor" and the whole resource fails to load, so the
// property this writer reported and every other property in the file are gone
// together.
void writes_composite_packed_arrays_flat() {
    ProjectFixture project("packed-composites");

    const auto vectors = create({
        {"save_path", "res://art/nav.tres"},
        {"resource_type", "NavigationPolygon"},
        {"properties", {{"vertices", {{"type", "PackedVector2Array"},
                                      {"values", json::array({
                                          {{"x", 0}, {"y", 0}},
                                          {{"x", 512}, {"y", 0}}
                                      })}}}}}
    });
    ASSERT_TRUE(!vectors.isError);
    ASSERT_TRUE(project.read("art/nav.tres").find(
                    "vertices = PackedVector2Array(0, 0, 512, 0)") != std::string::npos);

    // Colour elements default their alpha, the way a lone Color does.
    const auto colours = create({
        {"save_path", "res://art/grad.tres"},
        {"resource_type", "Gradient"},
        {"properties", {{"colors", {{"type", "PackedColorArray"},
                                    {"values", json::array({
                                        {{"r", 1}, {"g", 0}, {"b", 0}}
                                    })}}}}}
    });
    ASSERT_TRUE(!colours.isError);
    ASSERT_TRUE(project.read("art/grad.tres").find("colors = PackedColorArray(1, 0, 0, 1.0)") !=
                std::string::npos);

    // The components already flattened, which is what the file looks like and
    // what a caller who copied one out of a .tres will send.
    const auto flat = create({
        {"save_path", "res://art/flat.tres"},
        {"resource_type", "NavigationPolygon"},
        {"properties", {{"vertices", {{"type", "PackedVector2Array"},
                                      {"values", json::array({0, 0, 512, 0})}}}}}
    });
    ASSERT_TRUE(!flat.isError);
    ASSERT_TRUE(project.read("art/flat.tres").find(
                    "vertices = PackedVector2Array(0, 0, 512, 0)") != std::string::npos);

    // Godot drops the trailing part-element and reports nothing, so a count
    // that is not a whole number of elements is refused rather than written.
    const auto ragged = create({
        {"save_path", "res://art/ragged.tres"},
        {"resource_type", "NavigationPolygon"},
        {"properties", {{"vertices", {{"type", "PackedVector2Array"},
                                      {"values", json::array({0, 0, 512})}}}}}
    });
    ASSERT_TRUE(ragged.isError);
    ASSERT_TRUE(textOf(ragged).find("whole number of Vector2s") != std::string::npos);
    ASSERT_TRUE(!project.exists("art/ragged.tres"));

    const auto mixed = create({
        {"save_path", "res://art/mixed.tres"},
        {"resource_type", "NavigationPolygon"},
        {"properties", {{"vertices", {{"type", "PackedVector2Array"},
                                      {"values", json::array({0, 0, {{"x", 1}, {"y", 2}}})}}}}}
    });
    ASSERT_TRUE(mixed.isError);
    ASSERT_TRUE(textOf(mixed).find("Send one or the other") != std::string::npos);

    // The packed arrays whose element is already a scalar are unchanged.
    const auto scalars = create({
        {"save_path", "res://art/offsets.tres"},
        {"resource_type", "Gradient"},
        {"properties", {{"offsets", {{"type", "PackedFloat32Array"},
                                     {"values", json::array({0.0, 0.5, 1.0})}}}}}
    });
    ASSERT_TRUE(!scalars.isError);
    ASSERT_TRUE(project.read("art/offsets.tres").find(
                    "offsets = PackedFloat32Array(0.0, 0.5, 1.0)") != std::string::npos);
}

// A scalar or an array into a slot that cannot hold it (#764).
//
// The #730 guard compared the shape of an object against the declared type, so
// it never saw a string, a number, a boolean or an array. Godot keeps the
// property's default for all of them and says nothing, which is the silent
// wrong write this writer exists to refuse.
void refuses_a_scalar_the_declared_type_cannot_hold() {
    ProjectFixture project("declared-scalar-refusals");

    struct Row {
        const char* resource_type;
        const char* property;
        json value;
        const char* expected_in_message;
    };
    const std::vector<Row> refused = {
        {"CircleShape2D", "radius", "big", "Send a number"},
        {"StyleBoxFlat", "expand_margin_top", true, "Send a number"},
        {"StyleBoxFlat", "corner_detail", "many", "Send a whole number"},
        {"StyleBoxFlat", "corner_detail", 4.7, "truncates a fraction"},
        {"StyleBoxFlat", "anti_aliasing", "yes please", "Send true or false"},
        // The message is serialised into the text item, so its own quotes come
        // back escaped. Match the part that carries no quotes.
        {"RectangleShape2D", "size", 7, "declared Vector2 by"},
        {"StyleBoxFlat", "shadow_offset", "over there", "Send an object with"},
        {"StyleBoxFlat", "bg_color", 3, "colour string"},
        {"StyleBoxFlat", "bg_color", json::array({1.0, 0.5, 0.0, 1.0}), "colour string"},
        // No constructor spells a float, so an object is wrong there too.
        {"CircleShape2D", "radius", {{"x", 1}, {"y", 2}}, "Send a number"},
    };
    for (const auto& row : refused) {
        const auto result = create({
            {"save_path", "res://art/refused.tres"},
            {"resource_type", row.resource_type},
            {"properties", {{row.property, row.value}}}
        });
        ASSERT_TRUE(result.isError);
        const auto message = textOf(result);
        ASSERT_TRUE(message.find(row.property) != std::string::npos);
        ASSERT_TRUE(message.find(row.expected_in_message) != std::string::npos);
        ASSERT_TRUE(!project.exists("art/refused.tres"));
    }

    // The conversions Godot does anyway, and the spellings the surface
    // documents, all still write. An integer into a float slot arrives as a
    // float, and a string into a Color goes through Godot's own #rrggbbaa
    // parsing, which is what scene_set_property documents for the same slot.
    const std::vector<Row> written = {
        {"CircleShape2D", "radius", 7, "radius = 7"},
        {"StyleBoxFlat", "corner_detail", 4, "corner_detail = 4"},
        // JSON does not separate 4 from 4.0, and a client that serialises
        // every number as a double must not be refused for the spelling.
        {"StyleBoxFlat", "corner_detail", 4.0, "corner_detail = 4.0"},
        {"StyleBoxFlat", "anti_aliasing", true, "anti_aliasing = true"},
        {"StyleBoxFlat", "bg_color", "#ff8800ff", "bg_color = \"#ff8800ff\""},
        {"StyleBoxFlat", "resource_name", "panel", "resource_name = \"panel\""},
        {"SpriteFrames", "animations", json::array(), "animations = []"},
    };
    for (const auto& row : written) {
        const auto result = create({
            {"save_path", "res://art/written.tres"},
            {"resource_type", row.resource_type},
            {"overwrite", true},
            {"properties", {{row.property, row.value}}}
        });
        ASSERT_TRUE(!result.isError);
        ASSERT_TRUE(project.read("art/written.tres").find(row.expected_in_message) !=
                    std::string::npos);
    }

    // A name the class reference does not carry a type for is unchecked, and
    // has to stay that way: `_data` on a Curve and `tracks/0/keys` on an
    // Animation are legitimate and absent from the dump.
    const auto storage_only = create({
        {"save_path", "res://art/curve.tres"},
        {"resource_type", "Curve"},
        {"properties", {{"_data", json::array({0.0, 0.0, 0.0, 0.0, 0})}}}
    });
    ASSERT_TRUE(!storage_only.isError);
}

struct Register {
    Register() {
        registerTest("resource_references.writes_declared_vector_type",
                     writes_a_vector_as_the_type_the_property_declares);
        registerTest("resource_references.packed_composites_written_flat",
                     writes_composite_packed_arrays_flat);
        registerTest("resource_references.refuses_wrong_declared_scalar",
                     refuses_a_scalar_the_declared_type_cannot_hold);
        registerTest("resource_references.refuses_wrong_declared_type",
                     refuses_a_value_the_declared_type_cannot_hold);
        registerTest("resource_references.refuses_undeclared_properties",
                     refuses_properties_the_type_does_not_declare);
        registerTest("resource_references.storage_only_names_still_write",
                     writes_storage_only_names_and_gates_an_unknown_type);
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
