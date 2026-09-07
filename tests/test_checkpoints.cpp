#include "didi/runtime/checkpoint_store.hpp"
#include <chrono>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <thread>
void registerTest(const std::string&, std::function<void()>);
#define TEST(suite, name)                                                                          \
    void test_##suite##_##name();                                                                  \
    struct Register_##suite##_##name {                                                             \
        Register_##suite##_##name() { registerTest(#suite "." #name, test_##suite##_##name); }     \
    } g_register_##suite##_##name;                                                                 \
    void test_##suite##_##name()
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x))                                                                                  \
            throw std::runtime_error("Failed: " #x);                                               \
    } while (false)
namespace {
namespace fs = std::filesystem;
using didi::runtime::CheckpointStore;
struct Fixture {
    fs::path root = fs::temp_directory_path() /
                    ("didi-checkpoint-test-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() { fs::create_directories(root / "source"); }
    ~Fixture() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    void put(const fs::path& name, const std::string& data) {
        fs::create_directories((root / name).parent_path());
        std::ofstream f(root / name, std::ios::binary);
        f << data;
    }
    std::string get(const fs::path& name) {
        std::ifstream f(root / name, std::ios::binary);
        return {std::istreambuf_iterator<char>(f), {}};
    }
};
} // namespace
TEST(Checkpoints, InitializesIsolatedCopyAndRestoresOriginalBytes) {
    Fixture f;
    f.put("source/project.godot", "config_version=5\n");
    f.put("source/sub/file.txt", "abc");
    for (const auto* excluded : {".git", ".godot", ".didi", ".worktrees"})
        f.put(fs::path("source/sub") / excluded / "secret", "excluded");
    CHECK(CheckpointStore::initialize(f.root / "source", f.root / "managed").isOk());
    CHECK(f.get("managed/project/sub/file.txt") == "abc");
    CHECK(!fs::exists(f.root / "managed/project/sub/.godot"));
    CHECK(CheckpointStore::initialize(f.root / "source", f.root / "managed").isErr());
    CheckpointStore s(f.root / "managed/project", f.root / "managed/checkpoints");
    auto checkpoint = s.create("before");
    CHECK(checkpoint.isOk());
    CHECK(checkpoint.value().at("files") == 2);
    CHECK(checkpoint.value().at("bytes") == 20);
    CHECK(checkpoint.value().at("coverage") == "saved_project_files");
    f.put("managed/project/sub/file.txt", "changed");
    CHECK(s.stageRestore(checkpoint.value().at("id"), f.root / "restored").isOk());
    CHECK(f.get("restored/sub/file.txt") == "abc");
    CHECK(f.get("source/sub/file.txt") == "abc");
    CHECK(s.stageRestore(checkpoint.value().at("id"), f.root / "restored").isErr());
}
TEST(Checkpoints, RetainsFiveCompletedSnapshotsAndIgnoresPartial) {
    Fixture f;
    f.put("source/file", "abc");
    CheckpointStore s(f.root / "source", f.root / "snapshots");
    std::vector<std::string> ids;
    for (int i = 0; i < 7; ++i) {
        auto r = s.create(std::to_string(i));
        CHECK(r.isOk());
        ids.push_back(r.value().at("id"));
    }
    fs::create_directories(f.root / "snapshots/.partial-abandoned");
    auto listed = s.list();
    CHECK(listed.isOk());
    CHECK(listed.value().size() == 5);
    CHECK(listed.value()[0].at("id") == ids.back());
    CHECK(!fs::exists(f.root / "snapshots" / ids.front()));
    CHECK(s.stageRestore(ids.front(), f.root / "missing").isErr());
}
TEST(Checkpoints, RejectsCorruptContentManifestTraversalDuplicatesAndExtraFiles) {
    Fixture f;
    f.put("source/file", "abc");
    CheckpointStore s(f.root / "source", f.root / "snapshots");
    auto cp = s.create("baseline");
    CHECK(cp.isOk());
    std::string id = cp.value().at("id");
    auto manifestPath = fs::path("snapshots") / id / "manifest.json";
    auto manifest = didi::json::parse(f.get(manifestPath));
    CHECK(manifest["entries"][0]["sha256"] ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    f.put(fs::path("snapshots") / id / "files/file", "bad");
    CHECK(s.stageRestore(id, f.root / "bad").isErr());
    CHECK(!fs::exists(f.root / "bad"));
    f.put(fs::path("snapshots") / id / "files/file", "abc");
    auto bad = manifest;
    bad["entries"][0]["path"] = "../escape";
    f.put(manifestPath, bad.dump());
    CHECK(s.stageRestore(id, f.root / "bad").isErr());
    bad = manifest;
    bad["entries"].push_back(bad["entries"][0]);
    bad["files"] = 2;
    bad["bytes"] = 6;
    f.put(manifestPath, bad.dump());
    CHECK(s.stageRestore(id, f.root / "bad").isErr());
    bad = manifest;
    bad["entries"][0]["size"] = 4;
    f.put(manifestPath, bad.dump());
    CHECK(s.stageRestore(id, f.root / "bad").isErr());
    f.put(manifestPath, manifest.dump());
    f.put(fs::path("snapshots") / id / "files/unlisted", "hidden");
    CHECK(s.stageRestore(id, f.root / "bad").isErr());
    for (const auto* invalid : {"../escape", "", "C:/escape", "a/b", ".."})
        CHECK(s.stageRestore(invalid, f.root / "bad").isErr());
}
TEST(Checkpoints, RejectsOversizedFilesAndNestedContainerWithoutTouchingSource) {
    Fixture f;
    f.put("source/large", "");
    fs::resize_file(f.root / "source/large", 256ull * 1024 * 1024 + 1);
    CheckpointStore s(f.root / "source", f.root / "snapshots");
    CHECK(s.create("large").isErr());
    CHECK(s.list().isOk());
    CHECK(s.list().value().empty());
    CHECK(CheckpointStore::initialize(f.root / "source", f.root / "source/nested").isErr());
    CHECK(!fs::exists(f.root / "source/nested"));
    CHECK(fs::file_size(f.root / "source/large") == 256ull * 1024 * 1024 + 1);
}
TEST(Checkpoints, RejectsHardlinks) {
    Fixture f;
    f.put("source/file", "abc");
    fs::create_hard_link(f.root / "source/file", f.root / "source/alias");
    CheckpointStore s(f.root / "source", f.root / "snapshots");
    CHECK(s.create("linked").isErr());
    CHECK(CheckpointStore::initialize(f.root / "source", f.root / "managed").isErr());
}
TEST(Checkpoints, RejectsExcludedDirectoryInjectedIntoSnapshot) {
    Fixture f;
    f.put("source/file", "abc");
    CheckpointStore s(f.root / "source", f.root / "snapshots");
    auto cp = s.create("baseline");
    CHECK(cp.isOk());
    std::string id = cp.value().at("id");
    f.put(fs::path("snapshots") / id / "files/.godot/extra", "unlisted");
    CHECK(s.stageRestore(id, f.root / "restore").isErr());
    CHECK(!fs::exists(f.root / "restore"));
}
TEST(Checkpoints, PublicSummariesExcludePerFileManifestData) {
    Fixture f;
    f.put("source/file", "abc");
    CheckpointStore s(f.root / "source", f.root / "snapshots");
    auto cp = s.create("summary");
    CHECK(cp.isOk());
    CHECK(!cp.value().contains("entries"));
    CHECK(!cp.value().contains("directories"));
    auto listed = s.list();
    CHECK(listed.isOk());
    CHECK(listed.value().size() == 1);
    CHECK(!listed.value()[0].contains("entries"));
    CHECK(!listed.value()[0].contains("directories"));
}
TEST(Checkpoints, FileCountBoundaryRetainsMaximumSizedSnapshots) {
    Fixture f;
    for (int i = 0; i < 10000; ++i)
        f.put(fs::path("source") / std::to_string(i), "");
    CheckpointStore s(f.root / "source", f.root / "snapshots");
    auto cp = s.create("at-limit");
    CHECK(cp.isOk());
    CHECK(cp.value().at("files") == 10000);
    f.put("source/overflow", "");
    CHECK(s.create("too-many").isErr());
    for (int i = 0; i < 10000; ++i)
        fs::remove(f.root / "source" / std::to_string(i));
    // Retention must not incorrectly count the manifest as a project file.
    for (int i = 0; i < 5; ++i)
        CHECK(s.create("retention-at-limit").isOk());
    CHECK(s.list().isOk());
    CHECK(s.list().value().size() == 5);
}
namespace {
void checkConcurrentMutation(bool addFile) {
    // Removing the inventory check admits an added empty file. Removing the digest
    // check admits a same-length rewrite whose modification time was restored.
    Fixture f;
    f.put("source/a-first.txt", "abc");
    f.put("source/z-large.bin", std::string(32 * 1024 * 1024, 'x'));
    const auto originalTime = fs::last_write_time(f.root / "source/a-first.txt");
    CheckpointStore store(f.root / "source", f.root / "snapshots");
    bool changed = false;
    std::exception_ptr writerError;
    std::jthread writer([&](std::stop_token stop) {
        try {
            while (!stop.stop_requested()) {
                if (fs::exists(f.root / "snapshots")) {
                    for (const auto& dir : fs::directory_iterator(f.root / "snapshots")) {
                        if (!dir.path().filename().string().starts_with(".partial-"))
                            continue;
                        if (!fs::exists(dir.path() / "files/a-first.txt"))
                            continue;
                        if (addFile) {
                            f.put("source/new-empty-file", "");
                        } else {
                            f.put("source/a-first.txt", "def");
                            fs::last_write_time(f.root / "source/a-first.txt", originalTime);
                        }
                        changed = true;
                        return;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        } catch (...) {
            writerError = std::current_exception();
        }
    });
    auto result = store.create("concurrent-write");
    writer.request_stop();
    writer.join();
    if (writerError)
        std::rethrow_exception(writerError);
    CHECK(changed);
    CHECK(result.isErr());
    auto completed = store.list();
    CHECK(completed.isOk());
    CHECK(completed.value().empty());
}
} // namespace
TEST(Checkpoints, RejectsConcurrentRewriteWithRestoredMetadata) { checkConcurrentMutation(false); }
TEST(Checkpoints, RejectsConcurrentAddedFiles) { checkConcurrentMutation(true); }
TEST(Checkpoints, FullPartialStoreRejectsCreationWithoutGrowing) {
    Fixture f;
    f.put("source/file", "abc");
    for (int i = 0; i < 1000; ++i)
        fs::create_directories(f.root / "snapshots" / (".partial-abandoned-" + std::to_string(i)));
    CheckpointStore store(f.root / "source", f.root / "snapshots");
    for (int attempt = 0; attempt < 2; ++attempt) {
        CHECK(store.create("full-store").isErr());
        size_t count = 0;
        for (const auto& entry : fs::directory_iterator(f.root / "snapshots")) {
            (void)entry;
            ++count;
        }
        CHECK(count == 1000);
    }
}
