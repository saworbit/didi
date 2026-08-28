#include "didi/offline/resource_indexer.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

void registerTest(const std::string& name, std::function<void()> fn);

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
    const auto shader = root / "water.gdshader";
    std::ofstream(shader) << "shader_type spatial;\n";
    std::ofstream(shader.string() + ".uid") << "uid://watershader123\n";
    const auto texture = root / "water.png";
    std::ofstream(texture, std::ios::binary) << "not-a-real-png";
    std::ofstream(texture.string() + ".uid") << "uid://watertexture456\n";

    didi::offline::ResourceIndexer indexer;
    indexer.scan(root.string());
    const auto shaders = indexer.query("res://", "Shader", "water.gdshader", true);
    const auto textures = indexer.query("res://", "Texture2D", "water.png", true);
    std::filesystem::remove_all(root);

    ASSERT_EQ(shaders.size(), 1u);
    ASSERT_EQ(shaders[0].uid, "uid://watershader123");
    ASSERT_EQ(textures.size(), 1u);
    ASSERT_EQ(textures[0].uid, "uid://watertexture456");
}

struct RegisterResourceIndexerTests {
    RegisterResourceIndexerTests() {
        registerTest("ResourceIndexer.TypeDetection", test_resource_type_detection);
        registerTest("ResourceIndexer.UidSidecar", test_uid_sidecar_fallback);
        registerTest("ResourceIndexer.ExternalUidSidecar", test_uid_sidecars_are_indexed_for_external_resources);
    }
} g_registerResourceIndexerTests;
