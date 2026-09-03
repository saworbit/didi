#include "didi/offline/blackboard.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

void registerTest(const std::string& name, std::function<void()> fn);

namespace {

using namespace didi;
using namespace didi::offline;

// The board is resolved from the working directory, because that is the project
// root the server was started with. A test therefore has to become a project,
// and has to put the working directory back even when it throws.
class BoardFixture {
public:
    explicit BoardFixture(const std::string& suffix) {
        m_previous = std::filesystem::current_path();
        m_root = std::filesystem::temp_directory_path() /
                 ("didi-blackboard-" + suffix + "-" +
                  std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(m_root);
        std::filesystem::current_path(m_root);
    }

    ~BoardFixture() {
        std::error_code ignored;
        std::filesystem::current_path(m_previous, ignored);
        std::filesystem::remove_all(m_root, ignored);
    }

    std::string rawBoardFile(const std::string& board = "default") const {
        const auto file = m_root / ".didi" / "blackboard" / (board + ".json");
        std::ifstream input(file, std::ios::binary);
        if (!input) return {};
        std::ostringstream buffer;
        buffer << input.rdbuf();
        return buffer.str();
    }

private:
    std::filesystem::path m_previous;
    std::filesystem::path m_root;
};

BlackboardClock fixedClock(int64_t* now_ms) {
    return [now_ms] { return *now_ms; };
}

json writeValue(const std::string& path, const json& value, int64_t* now_ms = nullptr) {
    BlackboardWriteRequest request;
    request.path = path;
    request.value = value;
    auto result = now_ms ? blackboardWrite(request, fixedClock(now_ms))
                         : blackboardWrite(request);
    ASSERT_TRUE(result.isOk());
    return result.value();
}

void test_blackboard_write_read_round_trip() {
    BoardFixture fixture("round-trip");

    writeValue("architecture.inventory.slots", 12);
    writeValue("architecture.inventory.stacking", "by_type");
    writeValue("qa.notes", json::array({"jump feels floaty"}));

    BlackboardReadRequest deep;
    deep.path = "architecture.inventory";
    auto read = blackboardRead(deep);
    ASSERT_TRUE(read.isOk());
    ASSERT_TRUE(read.value()["found"].get<bool>());
    ASSERT_EQ(read.value()["value"]["slots"].get<int>(), 12);
    ASSERT_EQ(read.value()["value"]["stacking"].get<std::string>(), std::string("by_type"));

    // A shallow read marks a nested container rather than dropping it, so the
    // caller can see there is more without being handed the whole subtree.
    BlackboardReadRequest shallow;
    shallow.deep = false;
    auto top = blackboardRead(shallow);
    ASSERT_TRUE(top.isOk());
    ASSERT_EQ(top.value()["value"]["architecture"]["_truncated"].get<std::string>(),
              std::string("object"));

    BlackboardReadRequest missing;
    missing.path = "architecture.absent";
    auto absent = blackboardRead(missing);
    ASSERT_TRUE(absent.isOk());
    ASSERT_TRUE(!absent.value()["found"].get<bool>());

    // The second agent is a second process, so what matters is that the value is
    // on disk and not only in this one's memory.
    const std::string raw = fixture.rawBoardFile();
    ASSERT_TRUE(raw.find("by_type") != std::string::npos);

    BlackboardListKeysRequest keys;
    keys.prefix = "architecture";
    auto listed = blackboardListKeys(keys);
    ASSERT_TRUE(listed.isOk());
    ASSERT_EQ(listed.value()["total"].get<size_t>(), size_t{4});
    ASSERT_TRUE(!listed.value()["truncated"].get<bool>());
}

void test_blackboard_path_rejection() {
    BoardFixture fixture("paths");

    // Each of these would either escape the board or make it unreadable, and
    // none of them is normalised into something that looks like it worked.
    for (const char* path : {"", "..", "a..b", "a/../b", "a.", ".a", "a/", "/a"}) {
        BlackboardWriteRequest request;
        request.path = path;
        request.value = 1;
        ASSERT_TRUE(blackboardWrite(request).isErr());
    }

    BlackboardWriteRequest deep;
    deep.value = 1;
    for (int index = 0; index < 40; ++index) {
        if (!deep.path.empty()) deep.path += ".";
        deep.path += "s";
    }
    ASSERT_TRUE(blackboardWrite(deep).isErr());

    BlackboardWriteRequest long_path;
    long_path.value = 1;
    long_path.path = std::string(kBlackboardMaxPathBytes + 1, 'x');
    ASSERT_TRUE(blackboardWrite(long_path).isErr());

    // Writing through an existing value would silently turn another agent's
    // number into a container.
    writeValue("design.jumps", 2);
    BlackboardWriteRequest through;
    through.path = "design.jumps.count";
    through.value = 3;
    ASSERT_TRUE(blackboardWrite(through).isErr());
    BlackboardReadRequest check;
    check.path = "design.jumps";
    ASSERT_EQ(blackboardRead(check).value()["value"].get<int>(), 2);
}

void test_blackboard_patch_is_atomic() {
    BoardFixture fixture("patch");

    writeValue("design.max_jumps", 1);
    writeValue("design.reset_on", "floor");

    BlackboardPatchRequest good;
    good.operations = json::array({
        {{"op", "replace"}, {"path", "/design/max_jumps"}, {"value", 2}}
    });
    auto applied = blackboardPatch(good);
    ASSERT_TRUE(applied.isOk());
    BlackboardReadRequest read;
    read.path = "design.max_jumps";
    ASSERT_EQ(blackboardRead(read).value()["value"].get<int>(), 2);

    // The second operation cannot apply. The first must not survive it.
    BlackboardPatchRequest mixed;
    mixed.operations = json::array({
        {{"op", "replace"}, {"path", "/design/max_jumps"}, {"value", 99}},
        {{"op", "replace"}, {"path", "/design/does_not_exist"}, {"value", 1}}
    });
    ASSERT_TRUE(blackboardPatch(mixed).isErr());
    ASSERT_EQ(blackboardRead(read).value()["value"].get<int>(), 2);

    BlackboardReadRequest sibling;
    sibling.path = "design.reset_on";
    ASSERT_EQ(blackboardRead(sibling).value()["value"].get<std::string>(), std::string("floor"));

    // A dry run reports the result without keeping it.
    BlackboardPatchRequest preview;
    preview.dry_run = true;
    preview.operations = json::array({
        {{"op", "replace"}, {"path", "/design/max_jumps"}, {"value", 7}}
    });
    auto previewed = blackboardPatch(preview);
    ASSERT_TRUE(previewed.isOk());
    ASSERT_EQ(previewed.value()["resulting_value"]["design"]["max_jumps"].get<int>(), 7);
    ASSERT_EQ(blackboardRead(read).value()["value"].get<int>(), 2);
}

void test_blackboard_concurrent_writers_do_not_lose() {
    BoardFixture fixture("concurrent");

    constexpr int kWriters = 4;
    constexpr int kWritesEach = 6;
    std::atomic<int> failures{0};
    std::vector<std::thread> writers;
    for (int writer = 0; writer < kWriters; ++writer) {
        writers.emplace_back([writer, &failures] {
            for (int index = 0; index < kWritesEach; ++index) {
                BlackboardWriteRequest request;
                request.path = "worker" + std::to_string(writer) + ".item" + std::to_string(index);
                request.value = writer * 100 + index;
                if (blackboardWrite(request).isErr()) ++failures;
            }
        });
    }
    for (auto& thread : writers) thread.join();
    ASSERT_EQ(failures.load(), 0);

    // Read-modify-write without the lock loses whichever write landed first.
    // Every one of them has to be here.
    for (int writer = 0; writer < kWriters; ++writer) {
        for (int index = 0; index < kWritesEach; ++index) {
            BlackboardReadRequest read;
            read.path = "worker" + std::to_string(writer) + ".item" + std::to_string(index);
            auto value = blackboardRead(read);
            ASSERT_TRUE(value.isOk());
            ASSERT_TRUE(value.value()["found"].get<bool>());
            ASSERT_EQ(value.value()["value"].get<int>(), writer * 100 + index);
        }
    }
}

void test_blackboard_expiry_removes_entries() {
    BoardFixture fixture("expiry");
    int64_t now_ms = 1'000'000;

    BlackboardWriteRequest temporary;
    temporary.path = "scratch.hypothesis";
    temporary.value = "double jump may need coyote time";
    temporary.ttl_seconds = 60;
    ASSERT_TRUE(blackboardWrite(temporary, fixedClock(&now_ms)).isOk());

    writeValue("design.max_jumps", 2, &now_ms);

    BlackboardReadRequest read;
    read.path = "scratch.hypothesis";
    ASSERT_TRUE(blackboardRead(read, fixedClock(&now_ms)).value()["found"].get<bool>());

    now_ms += 61 * 1000;
    auto expired = blackboardRead(read, fixedClock(&now_ms));
    ASSERT_TRUE(expired.isOk());
    ASSERT_TRUE(!expired.value()["found"].get<bool>());

    // Gone from the listing and from the file, not merely hidden by the read.
    BlackboardListKeysRequest keys;
    auto listed = blackboardListKeys(keys, fixedClock(&now_ms));
    ASSERT_TRUE(listed.isOk());
    for (const auto& key : listed.value()["keys"]) {
        ASSERT_TRUE(key["path"].get<std::string>() != "scratch.hypothesis");
    }
    ASSERT_TRUE(fixture.rawBoardFile().find("coyote time") == std::string::npos);

    // The entry without a ttl is untouched.
    BlackboardReadRequest kept;
    kept.path = "design.max_jumps";
    ASSERT_EQ(blackboardRead(kept, fixedClock(&now_ms)).value()["value"].get<int>(), 2);
}

void test_blackboard_bounds_refuse_oversize_input() {
    BoardFixture fixture("bounds");

    BlackboardWriteRequest big;
    big.path = "scratch.blob";
    big.value = std::string(kBlackboardMaxValueBytes + 16, 'x');
    ASSERT_TRUE(blackboardWrite(big).isErr());

    json nested = "leaf";
    for (int level = 0; level < kBlackboardMaxDepth + 4; ++level) {
        nested = json{{"n", nested}};
    }
    BlackboardWriteRequest deep;
    deep.path = "scratch.deep";
    deep.value = nested;
    ASSERT_TRUE(blackboardWrite(deep).isErr());

    BlackboardWriteRequest bad_ttl;
    bad_ttl.path = "scratch.ttl";
    bad_ttl.value = 1;
    bad_ttl.ttl_seconds = 0;
    ASSERT_TRUE(blackboardWrite(bad_ttl).isErr());

    BlackboardWriteRequest bad_board;
    bad_board.board = "../escape";
    bad_board.path = "a";
    bad_board.value = 1;
    ASSERT_TRUE(blackboardWrite(bad_board).isErr());

    // A refusal must leave nothing behind that a later read would report.
    BlackboardListKeysRequest keys;
    auto listed = blackboardListKeys(keys);
    ASSERT_TRUE(listed.isOk());
    ASSERT_EQ(listed.value()["total"].get<size_t>(), size_t{0});
}

void test_blackboard_clear_scopes() {
    BoardFixture fixture("clear");

    writeValue("architecture.inventory.slots", 12);
    writeValue("architecture.combat.damage", 3);
    writeValue("qa.notes", "checked");

    BlackboardClearRequest preview;
    preview.path = "architecture.inventory";
    preview.dry_run = true;
    auto previewed = blackboardClear(preview);
    ASSERT_TRUE(previewed.isOk());
    ASSERT_EQ(previewed.value()["removed_keys"].get<size_t>(), size_t{2});

    BlackboardReadRequest still_there;
    still_there.path = "architecture.inventory.slots";
    ASSERT_TRUE(blackboardRead(still_there).value()["found"].get<bool>());

    BlackboardClearRequest scoped;
    scoped.path = "architecture.inventory";
    ASSERT_TRUE(blackboardClear(scoped).isOk());
    ASSERT_TRUE(!blackboardRead(still_there).value()["found"].get<bool>());

    BlackboardReadRequest sibling;
    sibling.path = "architecture.combat.damage";
    ASSERT_EQ(blackboardRead(sibling).value()["value"].get<int>(), 3);
    BlackboardReadRequest other;
    other.path = "qa.notes";
    ASSERT_EQ(blackboardRead(other).value()["value"].get<std::string>(), std::string("checked"));

    BlackboardClearRequest missing;
    missing.path = "architecture.absent";
    auto nothing = blackboardClear(missing);
    ASSERT_TRUE(nothing.isOk());
    ASSERT_TRUE(!nothing.value()["found"].get<bool>());

    BlackboardClearRequest whole;
    ASSERT_TRUE(blackboardClear(whole).isOk());
    BlackboardReadRequest root;
    auto after = blackboardRead(root);
    ASSERT_TRUE(after.isOk());
    ASSERT_TRUE(after.value()["found"].get<bool>());
    ASSERT_TRUE(after.value()["value"].empty());
}

struct Register {
    Register() {
        registerTest("Blackboard.WriteReadRoundTrip", test_blackboard_write_read_round_trip);
        registerTest("Blackboard.PathRejection", test_blackboard_path_rejection);
        registerTest("Blackboard.PatchIsAtomic", test_blackboard_patch_is_atomic);
        registerTest("Blackboard.ConcurrentWritersDoNotLose",
                     test_blackboard_concurrent_writers_do_not_lose);
        registerTest("Blackboard.ExpiryRemovesEntries", test_blackboard_expiry_removes_entries);
        registerTest("Blackboard.BoundsRefuseOversizeInput",
                     test_blackboard_bounds_refuse_oversize_input);
        registerTest("Blackboard.ClearScopes", test_blackboard_clear_scopes);
    }
} g_registerBlackboardTests;

} // namespace
