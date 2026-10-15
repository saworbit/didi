#include "didi/offline/resource_indexer.hpp"
#include "didi/offline/project_audit.hpp"
#include "didi/offline/project_text_scan.hpp"
#include "didi/offline/resource_uid.hpp"
#include "didi/mcp/mcp_protocol.hpp"
#include "didi/common/ipc_channel.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

void registerTest(const std::string& name, std::function<void()> fn);

static std::string utf8(const std::u8string& value) {
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

static void test_resource_type_detection() {
    ASSERT_EQ(didi::offline::ResourceIndexer::detectResourceType(".tscn"), "PackedScene");
    ASSERT_EQ(didi::offline::ResourceIndexer::detectResourceType(".tres"), "Resource");
    ASSERT_EQ(didi::offline::ResourceIndexer::detectResourceType(".gd"), "GDScript");
    ASSERT_EQ(didi::offline::ResourceIndexer::detectResourceType(".png"), "Texture2D");
    ASSERT_EQ(didi::offline::ResourceIndexer::detectResourceType(".glb"), "MeshResource");
    ASSERT_EQ(didi::offline::ResourceIndexer::detectResourceType(".gdextension"), "GDExtension");
}

static void test_uid_sidecar_fallback() {
    const auto root = std::filesystem::temp_directory_path() /
                      ("didi-resource-uid-" + std::to_string(
                          std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    const auto script = root / "player.gd";
    std::ofstream(script) << "extends Node\n";
    std::ofstream(script.string() + ".uid") << "  uid://d1d1player123  \n";

    ASSERT_EQ(didi::offline::ResourceIndexer::extractUidFromFile(script.string()),
              "uid://d1d1player123");

    std::ofstream(script.string() + ".uid", std::ios::trunc) << "uid://INVALID_uid\n";
    ASSERT_TRUE(didi::offline::ResourceIndexer::extractUidFromFile(script.string()).empty());

    std::ofstream(script.string() + ".uid", std::ios::trunc) << std::string(257, 'a');
    ASSERT_TRUE(didi::offline::ResourceIndexer::extractUidFromFile(script.string()).empty());

    const auto resource = root / "player.tres";
    std::ofstream(resource) << "[gd_resource type=\"Resource\" format=3 uid=\"uid://embedded123\"]\n";
    std::ofstream(resource.string() + ".uid") << "uid://sidecar456\n";
    ASSERT_EQ(didi::offline::ResourceIndexer::extractUidFromFile(resource.string()),
              "uid://embedded123");

    std::filesystem::remove_all(root);
}

static void test_uid_sidecars_are_indexed_for_external_resources() {
    const auto root = std::filesystem::temp_directory_path() /
                      ("didi-resource-uid-scan-" + std::to_string(
                          std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    const auto shader = root / std::filesystem::path(u8"水.gdshader");
    std::ofstream(shader) << "shader_type spatial;\n";
    auto shader_sidecar = shader;
    shader_sidecar += ".uid";
    std::ofstream(shader_sidecar) << "uid://watershader123\n";
    const auto texture = root / std::filesystem::path(u8"纹理.png");
    std::ofstream(texture, std::ios::binary) << "not-a-real-png";
    auto texture_sidecar = texture;
    texture_sidecar += ".uid";
    std::ofstream(texture_sidecar) << "uid://watertexture456\n";

    didi::offline::ResourceIndexer indexer;
    indexer.scan(root.string());
    const auto shaders = indexer.query("res://", "Shader", utf8(u8"水.gdshader"), true);
    const auto textures = indexer.query("res://", "Texture2D", utf8(u8"纹理.png"), true);
    std::filesystem::remove_all(root);

    ASSERT_EQ(shaders.size(), 1u);
    ASSERT_EQ(shaders[0].path, "res://" + utf8(u8"水.gdshader"));
    ASSERT_EQ(shaders[0].uid, "uid://watershader123");
    ASSERT_EQ(textures.size(), 1u);
    ASSERT_EQ(textures[0].path, "res://" + utf8(u8"纹理.png"));
    ASSERT_EQ(textures[0].uid, "uid://watertexture456");
}

namespace {

class IndexFixture {
public:
    IndexFixture() {
        m_root = std::filesystem::temp_directory_path() /
                 ("didi-resource-index-" + std::to_string(
                     std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(m_root);
    }
    ~IndexFixture() {
        std::error_code ignored;
        std::filesystem::remove_all(m_root, ignored);
    }
    void write(const std::string& relative, const std::string& contents) {
        const auto target = m_root / relative;
        std::filesystem::create_directories(target.parent_path());
        std::ofstream(target, std::ios::binary) << contents;
    }
    std::string root() const { return m_root.string(); }

private:
    std::filesystem::path m_root;
};

} // namespace

static void test_lookup_is_exact_and_listing_is_by_directory() {
    // Break caught: res://player.gd also matched res://player.gd.uid and
    // res://player.gdextension, and res://scenes matched res://scenes_v2.
    IndexFixture fixture;
    fixture.write("player.gd", "extends Node\n");
    fixture.write("player.gd.uid", "uid://d1d1player123\n");
    fixture.write("player.gdextension", "[configuration]\n");
    fixture.write("scenes/level.tscn", "[gd_scene format=3]\n");
    fixture.write("scenes_v2/level.tscn", "[gd_scene format=3]\n");

    didi::offline::ResourceIndexer indexer;
    indexer.scan(fixture.root());

    const auto* script = indexer.findExact("res://player.gd");
    ASSERT_TRUE(script != nullptr);
    ASSERT_EQ(script->path, "res://player.gd");
    ASSERT_EQ(script->type, "GDScript");
    ASSERT_TRUE(indexer.findExact("res://player") == nullptr);
    ASSERT_TRUE(indexer.findExact("res://missing.gd") == nullptr);

    const auto scenes = indexer.query("res://scenes");
    ASSERT_EQ(scenes.size(), 1u);
    ASSERT_EQ(scenes[0].path, "res://scenes/level.tscn");

    const auto trailing = indexer.query("res://scenes/");
    ASSERT_EQ(trailing.size(), 1u);
    ASSERT_EQ(trailing[0].path, "res://scenes/level.tscn");
}

static void test_the_index_is_in_path_order_whatever_the_filesystem() {
    // Break caught: the index kept the order the directory walk met files in,
    // which is name order on NTFS and not on ext4, so project_list_resources
    // answered the same project in a different order on Linux (#1121). An
    // upper-case name shows it on Windows too: NTFS sorts without case, a
    // byte comparison puts it first.
    IndexFixture fixture;
    fixture.write("b.gd", "extends Node\n");
    fixture.write("a.gd", "extends Node\n");
    fixture.write("C.gd", "extends Node\n");
    fixture.write("a/z.tres", "[gd_resource type=\"Resource\" format=3]\n");

    didi::offline::ResourceIndexer indexer;
    indexer.scan(fixture.root());

    std::vector<std::string> listed;
    for (const auto& resource : indexer.query()) listed.push_back(resource.path);
    const std::vector<std::string> expected = {"res://C.gd", "res://a.gd", "res://a/z.tres", "res://b.gd"};
    ASSERT_EQ(listed, expected);
}

static void test_text_resources_report_their_dependencies() {
    // Break caught: dependency extraction ran for PackedScene only, so every
    // .tres material, theme and tileset reported an empty dependency list.
    IndexFixture fixture;
    fixture.write("materials/wood.tres",
                  "[gd_resource type=\"StandardMaterial3D\" load_steps=3 format=3]\n\n"
                  "[ext_resource type=\"Texture2D\" path=\"res://textures/wood.png\" id=\"1\"]\n"
                  "[ext_resource type=\"Shader\" path=\"res://shaders/wood.gdshader\" id=\"2\"]\n\n"
                  "[resource]\n");
    fixture.write("textures/wood.png", "\x89PNG");
    fixture.write("shaders/wood.gdshader", "shader_type spatial;\n");

    didi::offline::ResourceIndexer indexer;
    indexer.scan(fixture.root());

    const auto* material = indexer.findExact("res://materials/wood.tres");
    ASSERT_TRUE(material != nullptr);
    ASSERT_EQ(material->type, "Resource");
    ASSERT_EQ(material->dependencies.size(), 2u);
    ASSERT_EQ(material->dependencies[0], "res://textures/wood.png");
    ASSERT_EQ(material->dependencies[1], "res://shaders/wood.gdshader");
}

static void test_shared_index_is_reused_and_dropped_on_invalidate() {
    // Break caught: every read tool built its own indexer and crawled the whole
    // project again.
    IndexFixture fixture;
    fixture.write("scenes/level.tscn", "[gd_scene format=3]\n");

    didi::offline::ResourceIndexer::invalidateSharedIndex();
    const auto first = didi::offline::ResourceIndexer::sharedIndex(fixture.root());
    const auto second = didi::offline::ResourceIndexer::sharedIndex(fixture.root());
    ASSERT_TRUE(first == second);
    ASSERT_EQ(first->query("res://").size(), 1u);

    didi::offline::ResourceIndexer::invalidateSharedIndex();
    const auto third = didi::offline::ResourceIndexer::sharedIndex(fixture.root());
    ASSERT_TRUE(third != first);
    ASSERT_EQ(third->query("res://").size(), 1u);
    didi::offline::ResourceIndexer::invalidateSharedIndex();
}

static void test_rescan_reuses_unchanged_files_and_notices_changed_ones() {
    // Rebuilding the index re-opened every scene, resource and script to pull
    // its uid and dependencies, whether or not anything had changed. On a few
    // thousand files that was most of the cost of a scan, repeated on every
    // rebuild.
    //
    // Skipping that read is only safe if a changed file is still noticed, so
    // that is what this checks: the same answer when nothing moved, and the new
    // answer when something did.
    IndexFixture fixture;
    fixture.write("scenes/level.tscn",
                  "[gd_scene format=3 uid=\"uid://before\"]\n"
                  "[ext_resource type=\"Texture2D\" path=\"res://art/first.png\" id=\"1\"]\n");

    didi::offline::ResourceIndexer::invalidateSharedIndex();
    didi::offline::ResourceIndexer first;
    first.scan(fixture.root());
    const auto* before = first.findExact("res://scenes/level.tscn");
    ASSERT_TRUE(before != nullptr);
    ASSERT_EQ(before->uid, "uid://before");
    ASSERT_EQ(before->dependencies.size(), 1u);
    ASSERT_EQ(before->dependencies[0], "res://art/first.png");

    // Nothing changed, so the memo answers and the facts must be identical.
    didi::offline::ResourceIndexer again;
    again.scan(fixture.root());
    const auto* unchanged = again.findExact("res://scenes/level.tscn");
    ASSERT_TRUE(unchanged != nullptr);
    ASSERT_EQ(unchanged->uid, "uid://before");
    ASSERT_EQ(unchanged->dependencies[0], "res://art/first.png");

    // A rewrite of a different length moves both the size and the timestamp,
    // which is what the memo keys on. A stale hit here would mean the index
    // reporting a uid the file no longer carries.
    fixture.write("scenes/level.tscn",
                  "[gd_scene format=3 uid=\"uid://afterwards\"]\n"
                  "[ext_resource type=\"Texture2D\" path=\"res://art/second.png\" id=\"1\"]\n"
                  "[ext_resource type=\"Texture2D\" path=\"res://art/third.png\" id=\"2\"]\n");

    didi::offline::ResourceIndexer third;
    third.scan(fixture.root());
    const auto* after = third.findExact("res://scenes/level.tscn");
    ASSERT_TRUE(after != nullptr);
    ASSERT_EQ(after->uid, "uid://afterwards");
    ASSERT_EQ(after->dependencies.size(), 2u);
    ASSERT_EQ(after->dependencies[0], "res://art/second.png");
    didi::offline::ResourceIndexer::invalidateSharedIndex();
}

static void test_invalidate_drops_the_per_file_memo_as_well() {
    // A mutating tool calls invalidateSharedIndex so its own write is never
    // served stale. That has to reach the per file memo too: a rewrite to the
    // same length inside the filesystem's timestamp resolution is exactly the
    // case the memo cannot see on its own, and it is the case Didi's own
    // writes create.
    IndexFixture fixture;
    fixture.write("a.tres", "[gd_resource type=\"Resource\" uid=\"uid://aaaaaa\"]\n");

    didi::offline::ResourceIndexer::invalidateSharedIndex();
    didi::offline::ResourceIndexer first;
    first.scan(fixture.root());
    ASSERT_EQ(first.findExact("res://a.tres")->uid, "uid://aaaaaa");

    // Same length and the timestamp put back, so the memo genuinely cannot see
    // the change and only the invalidation can. Writing and letting the clock
    // move leaves the memo missing anyway, and the test would pass without
    // testing anything.
    const auto target = std::filesystem::path(fixture.root()) / "a.tres";
    const auto original_time = std::filesystem::last_write_time(target);
    fixture.write("a.tres", "[gd_resource type=\"Resource\" uid=\"uid://bbbbbb\"]\n");
    std::filesystem::last_write_time(target, original_time);
    ASSERT_EQ(std::filesystem::last_write_time(target), original_time);
    didi::offline::ResourceIndexer::invalidateSharedIndex();

    didi::offline::ResourceIndexer second;
    second.scan(fixture.root());
    ASSERT_EQ(second.findExact("res://a.tres")->uid, "uid://bbbbbb");
    didi::offline::ResourceIndexer::invalidateSharedIndex();
}

static void test_out_of_source_build_trees_are_not_project_resources() {
    // Break caught: the skip list named build, build-clean and build-vs one at
    // a time, so build-ninja was indexed in full. .gitignore already gave up on
    // naming them and covers build*/. Every artifact under one counts against
    // kMaxIndexedResources, and a large enough build tree pushes real project
    // files out of the index.
    //
    // A prefix is not a glob on purpose. buildings/ is a project directory.
    IndexFixture fixture;
    fixture.write("scenes/level.tscn", "[gd_scene format=3]\n");
    fixture.write("buildings/tower.tscn", "[gd_scene format=3]\n");
    fixture.write("build-ninja/generated.tscn", "[gd_scene format=3]\n");
    fixture.write("build-debug/CMakeFiles/other.tres", "[gd_resource type=\"Resource\"]\n");
    fixture.write("build/old.tscn", "[gd_scene format=3]\n");

    didi::offline::ResourceIndexer::invalidateSharedIndex();
    didi::offline::ResourceIndexer indexer;
    indexer.scan(fixture.root());

    ASSERT_TRUE(indexer.findExact("res://scenes/level.tscn") != nullptr);
    ASSERT_TRUE(indexer.findExact("res://buildings/tower.tscn") != nullptr);
    ASSERT_TRUE(indexer.findExact("res://build-ninja/generated.tscn") == nullptr);
    ASSERT_TRUE(indexer.findExact("res://build-debug/CMakeFiles/other.tres") == nullptr);
    ASSERT_TRUE(indexer.findExact("res://build/old.tscn") == nullptr);
    didi::offline::ResourceIndexer::invalidateSharedIndex();
}

static void test_imported_assets_take_their_uid_from_import_metadata() {
    // Break caught: Godot writes no .uid sidecar for an imported asset, it
    // writes the uid into the .import file it generates beside it. Nothing read
    // that, so no texture, audio clip or mesh ever reached the uid map and
    // project_audit_assets called every uid:// reference to one unresolved.
    IndexFixture fixture;
    fixture.write("tiles/wall.png", "not-a-real-png");
    fixture.write("tiles/wall.png.import",
                  "[remap]\n\n"
                  "importer=\"texture\"\n"
                  "type=\"CompressedTexture2D\"\n"
                  "uid=\"uid://btgxunai7rduq\"\n"
                  "path=\"res://.godot/imported/wall.png-abc.ctex\"\n");
    // A .uid sidecar still wins where one exists, and a file with neither still
    // reports no uid rather than an invented one.
    fixture.write("audio/hit.wav", "RIFF");
    fixture.write("audio/hit.wav.uid", "uid://sidecarwins1\n");
    fixture.write("audio/hit.wav.import", "[remap]\n\nuid=\"uid://importloses1\"\n");
    fixture.write("tiles/plain.png", "not-a-real-png");

    didi::offline::ResourceIndexer indexer;
    indexer.scan(fixture.root());

    const auto* wall = indexer.findExact("res://tiles/wall.png");
    ASSERT_TRUE(wall != nullptr);
    ASSERT_EQ(wall->uid, "uid://btgxunai7rduq");

    const auto* audio = indexer.findExact("res://audio/hit.wav");
    ASSERT_TRUE(audio != nullptr);
    ASSERT_EQ(audio->uid, "uid://sidecarwins1");

    const auto* plain = indexer.findExact("res://tiles/plain.png");
    ASSERT_TRUE(plain != nullptr);
    ASSERT_TRUE(plain->uid.empty());

    // The .import file is metadata, not a resource that owns the uid, so it
    // must not claim it and shadow the asset in the map.
    const auto* metadata = indexer.findExact("res://tiles/wall.png.import");
    ASSERT_TRUE(metadata == nullptr || metadata->uid.empty());
}

namespace didi::mcp {
// Declared the way tool_registry.cpp declares it. Calling the handler directly
// puts the offline branch under test without standing up a registry whose ipc
// client an earlier test may have set.
CallToolResult handleEditorReloadProject(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
} // namespace didi::mcp

static void test_offline_reload_project_drops_the_shared_index() {
    // Break caught: the offline branch answered "Offline caches re-indexed."
    // and re-indexed nothing. Callers reach for editor_reload_project after
    // editing files outside Didi, and the cached index carried on answering
    // with what it read before.
    IndexFixture fixture;
    fixture.write("scenes/level.tscn", "[gd_scene format=3]\n");

    didi::offline::ResourceIndexer::invalidateSharedIndex();
    const auto before = didi::offline::ResourceIndexer::sharedIndex(fixture.root());
    ASSERT_EQ(before->query("res://").size(), 1u);

    fixture.write("scenes/menu.tscn", "[gd_scene format=3]\n");

    const auto result = didi::mcp::handleEditorReloadProject(didi::json::object(), nullptr);
    ASSERT_TRUE(!result.isError);

    const auto after = didi::offline::ResourceIndexer::sharedIndex(fixture.root());
    ASSERT_TRUE(after != before);
    ASSERT_EQ(after->query("res://").size(), 2u);
    didi::offline::ResourceIndexer::invalidateSharedIndex();
}

static void test_project_text_scan_shares_the_index_and_rename_does_not() {
    // The read paths reuse the cached list, so inspecting a resource and then
    // asking for its impact crawls the tree once. project_rename_references
    // cannot: it rewrites files, and a resource created outside Didi while the
    // cached list was alive would be missing from it, which is the
    // half-applied rename the truncation check refuses.
    IndexFixture fixture;
    fixture.write("scenes/level.tscn", "[gd_scene format=3]\n");

    didi::offline::ResourceIndexer::invalidateSharedIndex();
    const auto warm = didi::offline::ResourceIndexer::sharedIndex(fixture.root());
    ASSERT_EQ(warm->query("res://").size(), 1u);

    fixture.write("scenes/menu.tscn", "[gd_scene format=3]\n");

    const auto shared = didi::offline::scanProjectText(fixture.root());
    ASSERT_EQ(shared.resources.size(), 1u);

    const auto fresh = didi::offline::scanProjectText(fixture.root(),
                                                      didi::offline::ScanIndex::fresh);
    ASSERT_EQ(fresh.resources.size(), 2u);
    didi::offline::ResourceIndexer::invalidateSharedIndex();
}

static void test_uid_belongs_to_the_root_header_not_references_or_script_text() {
    IndexFixture fixture;
    fixture.write("texture.tres", "[gd_resource type=\"GradientTexture2D\" format=3 uid=\"uid://embedded123\"]\n[resource]\n");
    fixture.write("material.tres", "[gd_resource type=\"ShaderMaterial\" format=3]\n"
                                  "[ext_resource type=\"Texture2D\" uid=\"uid://embedded123\" path=\"res://texture.tres\" id=\"1\"]\n"
                                  "[resource]\n");
    fixture.write("scene.tscn", "[gd_scene format=3]\n"
                                "[ext_resource type=\"Texture2D\" uid=\"uid://embedded123\" path=\"res://texture.tres\" id=\"1\"]\n"
                                "[node name=\"Root\" type=\"Node\"]\n");
    fixture.write("player.gd", "# uid=\"uid://embedded123\"\nextends Node\n");
    fixture.write("player.gd.uid", "uid://d1d1player123\n");
    didi::offline::ResourceIndexer indexer;
    indexer.scan(fixture.root());
    ASSERT_TRUE(indexer.findExact("res://material.tres")->uid.empty());
    ASSERT_TRUE(indexer.findExact("res://scene.tscn")->uid.empty());
    ASSERT_EQ(indexer.findExact("res://texture.tres")->uid, "uid://embedded123");
    ASSERT_EQ(indexer.findExact("res://player.gd")->uid, "uid://d1d1player123");
}

static void test_imported_model_reports_the_type_its_import_records() {
    // Break caught: the index typed a .glb by its extension as MeshResource,
    // which is no Godot class, so type_filter "PackedScene" never found a model
    // Godot loads as one (#1461). The type has to come from the .import, and
    // the reads still from the extension: these bytes look like scene text, and
    // a model typed PackedScene that reached the scanners would grow a
    // dependency and a broken reference out of its own binary.
    IndexFixture fixture;
    const std::string model_bytes =
        "glTF binary [ext_resource type=\"Texture2D\" path=\"res://art/inside_the_model.png\" id=\"1\"]";
    fixture.write("models/probe.glb", model_bytes);
    fixture.write("models/probe.glb.import",
                  "[remap]\n\n"
                  "importer=\"scene\"\n"
                  "importer_version=1\n"
                  "type=\"PackedScene\"\n"
                  "uid=\"uid://bmodelprobe1\"\n"
                  "path=\"res://.godot/imported/probe.glb-abc.scn\"\n\n"
                  "[params]\n\n"
                  "nodes/root_type=\"\"\n");
    // Not imported yet, so the extension's guess is all there is.
    fixture.write("models/fresh.glb", "glTF");
    // A later section's key of the same name is not the asset's type.
    fixture.write("models/mesh.obj", "o mesh\n");
    fixture.write("models/mesh.obj.import",
                  "[remap]\n\nimporter=\"wavefront_obj\"\n\n[params]\n\ntype=\"Node3D\"\n");
    // A texture keeps Texture2D. Godot loads a CompressedTexture2D and writes a
    // scene's reference to it as Texture2D, measured on 4.5.1, 4.6.2 and 4.7.2,
    // and resource_create writes a reference with this type.
    fixture.write("art/wall.png", "not-a-real-png");
    fixture.write("art/wall.png.import",
                  "[remap]\n\nimporter=\"texture\"\ntype=\"CompressedTexture2D\"\n"
                  "uid=\"uid://bwalltexture1\"\n");
    // An extension the index does not know names only GenericResource.
    fixture.write("i18n/strings.csv", "keys,en\nHELLO,Hello\n");
    fixture.write("i18n/strings.csv.import",
                  "[remap]\n\nimporter=\"csv_translation\"\ntype=\"Translation\"\n");

    didi::offline::ResourceIndexer::invalidateSharedIndex();
    didi::offline::ResourceIndexer indexer;
    indexer.scan(fixture.root());

    const auto* model = indexer.findExact("res://models/probe.glb");
    ASSERT_TRUE(model != nullptr);
    ASSERT_EQ(model->type, "PackedScene");
    ASSERT_EQ(model->extension_type, "MeshResource");
    ASSERT_EQ(model->uid, "uid://bmodelprobe1");
    ASSERT_TRUE(model->dependencies.empty());
    ASSERT_EQ(model->toJson()["type"], "PackedScene");
    ASSERT_TRUE(!model->toJson().contains("extension_type"));

    ASSERT_EQ(indexer.findExact("res://models/fresh.glb")->type, "MeshResource");
    ASSERT_EQ(indexer.findExact("res://models/mesh.obj")->type, "MeshResource");
    ASSERT_EQ(indexer.findExact("res://art/wall.png")->type, "Texture2D");
    ASSERT_EQ(indexer.findExact("res://art/wall.png")->uid, "uid://bwalltexture1");
    ASSERT_EQ(indexer.findExact("res://i18n/strings.csv")->type, "Translation");

    const auto scenes = indexer.query("res://", "PackedScene");
    ASSERT_EQ(scenes.size(), 1u);
    ASSERT_EQ(scenes[0].path, "res://models/probe.glb");

    // The audit reads text through the same index. The model's bytes must not
    // be read as a scene, and an unused model is still an orphan.
    didi::offline::ProjectAuditOptions options;
    options.include_dead_signals = false;
    options.include_broken_connections = false;
    options.include_import_health = false;
    const auto report = didi::offline::auditProject(fixture.root(), options);
    ASSERT_TRUE(report["broken_references"].empty());
    bool model_orphaned = false;
    for (const auto& orphan : report["orphans"]) {
        if (orphan["path"] == "res://models/probe.glb") {
            model_orphaned = true;
            ASSERT_EQ(orphan["type"], "PackedScene");
        }
    }
    ASSERT_TRUE(model_orphaned);
    didi::offline::ResourceIndexer::invalidateSharedIndex();
}

static void test_rescan_notices_a_sidecar_written_after_its_file() {
    // Break caught: the per file memo was keyed on the file alone. Godot writes
    // an asset's .import when it first imports it and a script's .uid when it
    // first scans it, touching neither file, so both kept the facts read before
    // the sidecar existed: a model untyped and without a uid, a script without
    // its uid, until the file itself changed (#1461).
    IndexFixture fixture;
    fixture.write("models/probe.glb", "glTF");
    fixture.write("scripts/player.gd", "extends Node\n");

    didi::offline::ResourceIndexer::invalidateSharedIndex();
    didi::offline::ResourceIndexer before;
    before.scan(fixture.root());
    ASSERT_EQ(before.findExact("res://models/probe.glb")->type, "MeshResource");
    ASSERT_TRUE(before.findExact("res://models/probe.glb")->uid.empty());
    ASSERT_TRUE(before.findExact("res://scripts/player.gd")->uid.empty());

    fixture.write("models/probe.glb.import",
                  "[remap]\n\nimporter=\"scene\"\ntype=\"PackedScene\"\nuid=\"uid://bmodelprobe1\"\n");
    fixture.write("scripts/player.gd.uid", "uid://bplayerscript1\n");

    // No invalidation: only the editor wrote, and the memo must see it alone.
    didi::offline::ResourceIndexer after;
    after.scan(fixture.root());
    ASSERT_EQ(after.findExact("res://models/probe.glb")->type, "PackedScene");
    ASSERT_EQ(after.findExact("res://models/probe.glb")->uid, "uid://bmodelprobe1");
    ASSERT_EQ(after.findExact("res://scripts/player.gd")->uid, "uid://bplayerscript1");
    didi::offline::ResourceIndexer::invalidateSharedIndex();
}

static void test_one_rule_says_what_a_uid_is() {
    // The index, the audit and the impact analysis each had their own
    // definition, and the audit's took any text up to a quote (#1446). This is
    // the one they share, and it is ResourceUID::text_to_id's: uid:// and then
    // a-z and 0-9 only.
    using didi::offline::isResourceUidText;
    ASSERT_TRUE(isResourceUidText("uid://a"));
    ASSERT_TRUE(isResourceUidText("uid://btgxunai7rduq"));
    ASSERT_TRUE(isResourceUidText("uid://0123456789"));
    ASSERT_TRUE(!isResourceUidText(""));
    ASSERT_TRUE(!isResourceUidText("uid://"));
    ASSERT_TRUE(!isResourceUidText("uid:/abc"));
    ASSERT_TRUE(!isResourceUidText("UID://abc"));
    ASSERT_TRUE(!isResourceUidText("uid://ABC"));
    ASSERT_TRUE(!isResourceUidText("uid://abc-def"));
    ASSERT_TRUE(!isResourceUidText("uid://abc def"));
    ASSERT_TRUE(!isResourceUidText("uid://abc\n"));
    ASSERT_TRUE(!isResourceUidText("uid://<invalid>"));
    ASSERT_TRUE(!isResourceUidText(utf8(u8"uid://caf\u00e9")));
    ASSERT_TRUE(!isResourceUidText("res://abc"));
}

static void test_audit_follows_the_path_beside_a_uid_godot_cannot_read() {
    // Break caught: the audit took uid="uid://ABC" as a uid and called it
    // unresolved, while Godot warns "invalid UID", loads the path beside it and
    // the scene works, on 4.5.1, 4.6.2 and 4.7.2 (#1446). A uid it can read and
    // nothing carries is still reported.
    IndexFixture fixture;
    fixture.write("art/icon.png", "not-a-real-png");
    fixture.write("main.tscn",
                  "[gd_scene load_steps=3 format=3]\n\n"
                  "[ext_resource type=\"Texture2D\" uid=\"uid://ABC\" path=\"res://art/icon.png\" id=\"1\"]\n"
                  "[ext_resource type=\"Texture2D\" uid=\"uid://gonegone\" id=\"2\"]\n\n"
                  "[node name=\"Root\" type=\"Sprite2D\"]\ntexture = ExtResource(\"1\")\n");

    didi::offline::ResourceIndexer::invalidateSharedIndex();
    didi::offline::ProjectAuditOptions options;
    options.include_dead_signals = false;
    options.include_broken_connections = false;
    options.include_import_health = false;
    const auto report = didi::offline::auditProject(fixture.root(), options);
    ASSERT_EQ(report["broken_references"].size(), 1u);
    ASSERT_EQ(report["broken_references"][0]["target"], "uid://gonegone");
    ASSERT_EQ(report["broken_references"][0]["kind"], "unresolved_uid");
    for (const auto& orphan : report["orphans"]) ASSERT_TRUE(orphan["path"] != "res://art/icon.png");
    didi::offline::ResourceIndexer::invalidateSharedIndex();
}

struct RegisterResourceIndexerTests {
    RegisterResourceIndexerTests() {
        registerTest("ResourceIndexer.ExactLookupAndDirectoryListing",
                     test_lookup_is_exact_and_listing_is_by_directory);
        registerTest("ResourceIndexer.TextResourceDependencies",
                     test_text_resources_report_their_dependencies);
        registerTest("ResourceIndexer.SharedIndexReuseAndInvalidation",
                     test_shared_index_is_reused_and_dropped_on_invalidate);
        registerTest("ResourceIndexer.RescanReusesUnchangedFiles",
                     test_rescan_reuses_unchanged_files_and_notices_changed_ones);
        registerTest("ResourceIndexer.InvalidateDropsFileMemo",
                     test_invalidate_drops_the_per_file_memo_as_well);
        registerTest("ResourceIndexer.OutOfSourceBuildTreesAreSkipped",
                     test_out_of_source_build_trees_are_not_project_resources);
        registerTest("ResourceIndexer.ListedInPathOrder",
                     test_the_index_is_in_path_order_whatever_the_filesystem);
        registerTest("ResourceIndexer.TypeDetection", test_resource_type_detection);
        registerTest("ResourceIndexer.UidSidecar", test_uid_sidecar_fallback);
        registerTest("ResourceIndexer.UidBelongsToRootHeader",
                     test_uid_belongs_to_the_root_header_not_references_or_script_text);
        registerTest("ResourceIndexer.ExternalUidSidecar", test_uid_sidecars_are_indexed_for_external_resources);
        registerTest("ResourceIndexer.ImportedAssetUid",
                     test_imported_assets_take_their_uid_from_import_metadata);
        registerTest("ResourceIndexer.OfflineReloadDropsSharedIndex",
                     test_offline_reload_project_drops_the_shared_index);
        registerTest("ResourceIndexer.ProjectTextScanIndexSharing",
                     test_project_text_scan_shares_the_index_and_rename_does_not);
        registerTest("ResourceIndexer.ImportedModelType",
                     test_imported_model_reports_the_type_its_import_records);
        registerTest("ResourceIndexer.RescanNoticesLateSidecars",
                     test_rescan_notices_a_sidecar_written_after_its_file);
        registerTest("ResourceIndexer.UidTextRule", test_one_rule_says_what_a_uid_is);
        registerTest("ResourceIndexer.AuditFollowsPathBesideUnreadableUid",
                     test_audit_follows_the_path_beside_a_uid_godot_cannot_read);
    }
} g_registerResourceIndexerTests;
