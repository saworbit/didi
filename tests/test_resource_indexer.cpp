#include "didi/offline/resource_indexer.hpp"

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

struct RegisterResourceIndexerTests {
    RegisterResourceIndexerTests() {
        registerTest("ResourceIndexer.TypeDetection", test_resource_type_detection);
    }
} g_registerResourceIndexerTests;
