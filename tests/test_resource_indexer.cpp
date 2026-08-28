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
    std::ofstream(script.string() + ".uid") << "uid://didi_player_123\n";

    ASSERT_EQ(didi::offline::ResourceIndexer::extractUidFromFile(script.string()),
              "uid://didi_player_123");

    std::filesystem::remove_all(root);
}

struct RegisterResourceIndexerTests {
    RegisterResourceIndexerTests() {
        registerTest("ResourceIndexer.TypeDetection", test_resource_type_detection);
        registerTest("ResourceIndexer.UidSidecar", test_uid_sidecar_fallback);
    }
} g_registerResourceIndexerTests;
