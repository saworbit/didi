#include "didi/offline/resource_indexer.hpp"

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
        registerTest("ResourceIndexer.TypeDetection", test_resource_type_detection);
        registerTest("ResourceIndexer.UidSidecar", test_uid_sidecar_fallback);
        registerTest("ResourceIndexer.ExternalUidSidecar", test_uid_sidecars_are_indexed_for_external_resources);
        registerTest("ResourceIndexer.ImportedAssetUid",
                     test_imported_assets_take_their_uid_from_import_metadata);
    }
} g_registerResourceIndexerTests;
