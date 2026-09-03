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

    // A board nobody has written is an empty object, not an array. Brace-initialising
    // a json from json::object() picks the initializer-list constructor on GCC and
    // yields an array holding one object, which MSVC never reproduces and which makes
    // every path write fail with a type error.
    BlackboardReadRequest empty;
    auto blank = blackboardRead(empty);
    ASSERT_TRUE(blank.isOk());
    ASSERT_TRUE(blank.value()["value"].is_object());

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
    for (size_t level = 0; level < kBlackboardMaxDepth + 4; ++level) {
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


std::string createTask(const std::string& title,
                       const std::vector<std::string>& dependencies = {},
                       int64_t priority = 0,
                       const std::vector<std::string>& tags = {},
                       int64_t* now_ms = nullptr) {
    BlackboardTaskCreateRequest request;
    request.title = title;
    request.dependencies = dependencies;
    request.priority = priority;
    request.tags = tags;
    auto result = now_ms ? blackboardTaskCreate(request, fixedClock(now_ms))
                         : blackboardTaskCreate(request);
    ASSERT_TRUE(result.isOk());
    return result.value()["task"]["task_id"].get<std::string>();
}

std::string taskStatus(const std::string& task_id, int64_t* now_ms = nullptr) {
    BlackboardTaskListRequest request;
    auto listed = now_ms ? blackboardTaskList(request, fixedClock(now_ms))
                         : blackboardTaskList(request);
    ASSERT_TRUE(listed.isOk());
    for (const auto& task : listed.value()["tasks"]) {
        if (task["task_id"].get<std::string>() == task_id) {
            return task["status"].get<std::string>();
        }
    }
    return "missing";
}

void test_tasks_claim_is_exclusive() {
    BoardFixture fixture("claim");
    const std::string task = createTask("write CharacterBase.gd");

    // Reading "unclaimed" and writing "mine" are two steps, and another agent
    // fits between them. Exactly one of these may come away with the task.
    //
    // The agents wait on a gate so they all attempt the claim at the same
    // instant. Without it they start in sequence, finish before the next one
    // begins, and the test passes whether the claim is atomic or not.
    constexpr int kAgents = 8;
    std::atomic<bool> go{false};
    std::atomic<int> ready{0};
    std::atomic<int> winners{0};
    std::atomic<int> refusals{0};
    std::atomic<int> failures{0};
    std::vector<std::thread> agents;
    for (int index = 0; index < kAgents; ++index) {
        agents.emplace_back([index, &go, &ready, &winners, &refusals, &failures] {
            BlackboardTaskClaimRequest request;
            request.agent_id = "agent-" + std::to_string(index);
            request.lease_seconds = 60;
            ++ready;
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            auto claimed = blackboardTaskClaim(request);
            if (claimed.isErr()) ++failures;
            else if (claimed.value()["claimed"].get<bool>()) ++winners;
            else ++refusals;
        });
    }
    while (ready.load() < kAgents) std::this_thread::yield();
    go.store(true, std::memory_order_release);
    for (auto& agent : agents) agent.join();

    // One winner, and every other agent told cleanly that it lost. An error is
    // not a refusal: it would mean the claim collided rather than serialised.
    ASSERT_EQ(winners.load(), 1);
    ASSERT_EQ(failures.load(), 0);
    ASSERT_EQ(refusals.load(), kAgents - 1);

    ASSERT_EQ(taskStatus(task), std::string("in_progress"));

    // A second claim finds nothing, and says why rather than returning empty.
    BlackboardTaskClaimRequest again;
    again.agent_id = "latecomer";
    auto nothing = blackboardTaskClaim(again);
    ASSERT_TRUE(nothing.isOk());
    ASSERT_TRUE(!nothing.value()["claimed"].get<bool>());
    ASSERT_TRUE(nothing.value().contains("reason"));
}

void test_tasks_lease_expiry_reclaims() {
    BoardFixture fixture("lease");
    int64_t now_ms = 5'000'000;
    const std::string task = createTask("import the sprites", {}, 0, {}, &now_ms);

    BlackboardTaskClaimRequest first;
    first.agent_id = "agent-a";
    first.lease_seconds = 30;
    auto claimed = blackboardTaskClaim(first, fixedClock(&now_ms));
    ASSERT_TRUE(claimed.isOk() && claimed.value()["claimed"].get<bool>());

    // While the lease is live nobody else can have it.
    BlackboardTaskClaimRequest contender;
    contender.agent_id = "agent-b";
    auto blocked = blackboardTaskClaim(contender, fixedClock(&now_ms));
    ASSERT_TRUE(blocked.isOk() && !blocked.value()["claimed"].get<bool>());

    // The agent dies. The lease lapses and the work returns to the pool.
    now_ms += 31 * 1000;
    ASSERT_EQ(taskStatus(task, &now_ms), std::string("pending"));
    auto reclaimed = blackboardTaskClaim(contender, fixedClock(&now_ms));
    ASSERT_TRUE(reclaimed.isOk() && reclaimed.value()["claimed"].get<bool>());
    ASSERT_EQ(reclaimed.value()["task"]["lease"]["owner"].get<std::string>(),
              std::string("agent-b"));

    // The original holder must not be able to finish work it no longer owns.
    BlackboardTaskCompleteRequest stale;
    stale.task_id = task;
    stale.agent_id = "agent-a";
    ASSERT_TRUE(blackboardTaskComplete(stale, fixedClock(&now_ms)).isErr());
}

void test_tasks_dependencies_gate_readiness() {
    BoardFixture fixture("deps");
    int64_t now_ms = 1'000'000;

    const std::string base = createTask("write CharacterBase.gd", {}, 0, {}, &now_ms);
    const std::string shapes = createTask("check collision shapes", {}, 0, {}, &now_ms);
    const std::string player = createTask("extend into Player.gd", {base, shapes}, 0, {}, &now_ms);

    ASSERT_EQ(taskStatus(player, &now_ms), std::string("blocked"));
    ASSERT_EQ(taskStatus(base, &now_ms), std::string("pending"));

    // Claiming must never hand out a blocked task.
    BlackboardTaskClaimRequest targeted;
    targeted.agent_id = "agent-b";
    targeted.task_id = player;
    auto refused = blackboardTaskClaim(targeted, fixedClock(&now_ms));
    ASSERT_TRUE(refused.isOk() && !refused.value()["claimed"].get<bool>());

    const auto finish = [&](const std::string& task_id, const std::string& agent) {
        BlackboardTaskClaimRequest claim;
        claim.agent_id = agent;
        claim.task_id = task_id;
        claim.lease_seconds = 600;
        auto got = blackboardTaskClaim(claim, fixedClock(&now_ms));
        ASSERT_TRUE(got.isOk() && got.value()["claimed"].get<bool>());
        BlackboardTaskCompleteRequest done;
        done.task_id = task_id;
        done.agent_id = agent;
        return blackboardTaskComplete(done, fixedClock(&now_ms));
    };

    auto first = finish(base, "agent-a");
    ASSERT_TRUE(first.isOk());
    // One prerequisite left, so nothing is released yet.
    ASSERT_EQ(first.value()["unblocked"].size(), size_t{0});
    ASSERT_EQ(taskStatus(player, &now_ms), std::string("blocked"));

    auto second = finish(shapes, "agent-c");
    ASSERT_TRUE(second.isOk());
    ASSERT_EQ(second.value()["unblocked"].size(), size_t{1});
    ASSERT_EQ(second.value()["unblocked"][0].get<std::string>(), player);
    ASSERT_EQ(taskStatus(player, &now_ms), std::string("pending"));
}

void test_tasks_cycle_is_refused() {
    BoardFixture fixture("cycle");
    const std::string first = createTask("first");

    // Depending on something that does not exist would sit blocked forever with
    // nothing to explain it.
    BlackboardTaskCreateRequest unknown;
    unknown.title = "depends on a ghost";
    unknown.dependencies = {"TASK-does-not-exist"};
    ASSERT_TRUE(blackboardTaskCreate(unknown).isErr());

    BlackboardTaskCreateRequest itself;
    itself.task_id = "TASK-self";
    itself.title = "depends on itself";
    itself.dependencies = {"TASK-self"};
    ASSERT_TRUE(blackboardTaskCreate(itself).isErr());

    BlackboardTaskCreateRequest duplicate;
    duplicate.task_id = first;
    duplicate.title = "same id again";
    ASSERT_TRUE(blackboardTaskCreate(duplicate).isErr());

    // The refusals left nothing behind.
    BlackboardTaskListRequest all;
    auto listed = blackboardTaskList(all);
    ASSERT_TRUE(listed.isOk());
    ASSERT_EQ(listed.value()["total"].get<size_t>(), size_t{1});
}

void test_tasks_only_the_holder_completes() {
    BoardFixture fixture("holder");
    int64_t now_ms = 2'000'000;
    const std::string task = createTask("rebalance the jump", {}, 0, {}, &now_ms);

    BlackboardTaskClaimRequest claim;
    claim.agent_id = "agent-a";
    claim.lease_seconds = 600;
    ASSERT_TRUE(blackboardTaskClaim(claim, fixedClock(&now_ms)).isOk());

    BlackboardTaskCompleteRequest stranger;
    stranger.task_id = task;
    stranger.agent_id = "agent-b";
    ASSERT_TRUE(blackboardTaskComplete(stranger, fixedClock(&now_ms)).isErr());

    BlackboardTaskUpdateRequest meddler;
    meddler.task_id = task;
    meddler.agent_id = "agent-b";
    meddler.progress = 90;
    ASSERT_TRUE(blackboardTaskUpdate(meddler, fixedClock(&now_ms)).isErr());
    ASSERT_EQ(taskStatus(task, &now_ms), std::string("in_progress"));

    BlackboardTaskUpdateRequest owner;
    owner.task_id = task;
    owner.agent_id = "agent-a";
    owner.progress = 50;
    owner.note = "jump height feels right, tuning the fall";
    auto updated = blackboardTaskUpdate(owner, fixedClock(&now_ms));
    ASSERT_TRUE(updated.isOk());
    ASSERT_EQ(updated.value()["task"]["progress"].get<int>(), 50);
    ASSERT_EQ(updated.value()["task"]["notes"].size(), size_t{1});

    BlackboardTaskCompleteRequest done;
    done.task_id = task;
    done.agent_id = "agent-a";
    done.artifacts = json{{"files", json::array({"res://Player.gd"})}};
    auto completed = blackboardTaskComplete(done, fixedClock(&now_ms));
    ASSERT_TRUE(completed.isOk());
    ASSERT_EQ(completed.value()["task"]["status"].get<std::string>(), std::string("completed"));
    ASSERT_EQ(completed.value()["task"]["artifacts"]["files"][0].get<std::string>(),
              std::string("res://Player.gd"));

    // Completing twice is refused rather than quietly repeated.
    ASSERT_TRUE(blackboardTaskComplete(done, fixedClock(&now_ms)).isErr());
}

void test_tasks_status_transitions() {
    BoardFixture fixture("transitions");
    int64_t now_ms = 3'000'000;
    const std::string task = createTask("tune the coyote time", {}, 0, {}, &now_ms);

    BlackboardTaskClaimRequest claim;
    claim.agent_id = "agent-a";
    claim.lease_seconds = 600;
    ASSERT_TRUE(blackboardTaskClaim(claim, fixedClock(&now_ms)).isOk());

    // A review hands the work back, so the lease goes with it.
    BlackboardTaskUpdateRequest review;
    review.task_id = task;
    review.agent_id = "agent-a";
    review.status = "needs_review";
    auto reviewed = blackboardTaskUpdate(review, fixedClock(&now_ms));
    ASSERT_TRUE(reviewed.isOk());
    ASSERT_TRUE(!reviewed.value()["task"].contains("lease"));
    ASSERT_EQ(taskStatus(task, &now_ms), std::string("needs_review"));

    // A review outcome is somebody else's call, so reopening needs no lease.
    BlackboardTaskUpdateRequest reopen;
    reopen.task_id = task;
    reopen.agent_id = "reviewer";
    reopen.status = "pending";
    ASSERT_TRUE(blackboardTaskUpdate(reopen, fixedClock(&now_ms)).isOk());
    ASSERT_EQ(taskStatus(task, &now_ms), std::string("pending"));

    // Anything else without a lease is refused.
    BlackboardTaskUpdateRequest unleased;
    unleased.task_id = task;
    unleased.agent_id = "agent-a";
    unleased.progress = 10;
    ASSERT_TRUE(blackboardTaskUpdate(unleased, fixedClock(&now_ms)).isErr());

    BlackboardTaskUpdateRequest illegal;
    illegal.task_id = task;
    illegal.agent_id = "agent-a";
    illegal.status = "completed";
    ASSERT_TRUE(blackboardTaskUpdate(illegal, fixedClock(&now_ms)).isErr());

    // A renewed lease pushes the expiry out rather than handing the task on.
    BlackboardTaskClaimRequest reclaim;
    reclaim.agent_id = "agent-a";
    reclaim.lease_seconds = 30;
    ASSERT_TRUE(blackboardTaskClaim(reclaim, fixedClock(&now_ms)).isOk());
    BlackboardTaskUpdateRequest renew;
    renew.task_id = task;
    renew.agent_id = "agent-a";
    renew.renew_lease_seconds = 600;
    auto renewed = blackboardTaskUpdate(renew, fixedClock(&now_ms));
    ASSERT_TRUE(renewed.isOk());
    now_ms += 60 * 1000;
    ASSERT_EQ(taskStatus(task, &now_ms), std::string("in_progress"));
}

void test_tasks_list_filters() {
    BoardFixture fixture("filters");
    int64_t now_ms = 4'000'000;
    const std::string art = createTask("import sprites", {}, 0, {"art"}, &now_ms);
    createTask("write the controller", {}, 5, {"code"}, &now_ms);
    createTask("balance the jump", {}, 1, {"code"}, &now_ms);

    BlackboardTaskListRequest by_tag;
    by_tag.tag = "code";
    auto tagged = blackboardTaskList(by_tag, fixedClock(&now_ms));
    ASSERT_TRUE(tagged.isOk());
    ASSERT_EQ(tagged.value()["total"].get<size_t>(), size_t{2});

    // Priority decides which ready task a tagged claim gets.
    BlackboardTaskClaimRequest claim;
    claim.agent_id = "coder";
    claim.tag = "code";
    claim.lease_seconds = 600;
    auto claimed = blackboardTaskClaim(claim, fixedClock(&now_ms));
    ASSERT_TRUE(claimed.isOk() && claimed.value()["claimed"].get<bool>());
    ASSERT_EQ(claimed.value()["task"]["title"].get<std::string>(),
              std::string("write the controller"));

    BlackboardTaskListRequest by_assignee;
    by_assignee.assigned_to = "coder";
    auto mine = blackboardTaskList(by_assignee, fixedClock(&now_ms));
    ASSERT_TRUE(mine.isOk());
    ASSERT_EQ(mine.value()["total"].get<size_t>(), size_t{1});

    BlackboardTaskListRequest by_status;
    by_status.status = "pending";
    auto pending = blackboardTaskList(by_status, fixedClock(&now_ms));
    ASSERT_TRUE(pending.isOk());
    ASSERT_EQ(pending.value()["total"].get<size_t>(), size_t{2});

    BlackboardTaskListRequest capped;
    capped.max_tasks = 1;
    auto limited = blackboardTaskList(capped, fixedClock(&now_ms));
    ASSERT_TRUE(limited.isOk());
    ASSERT_EQ(limited.value()["returned"].get<size_t>(), size_t{1});
    ASSERT_EQ(limited.value()["total"].get<size_t>(), size_t{3});
    ASSERT_TRUE(limited.value()["truncated"].get<bool>());

    // Tasks are not reachable through the state paths, so a write cannot corrupt
    // the queue by choosing a colliding name.
    BlackboardReadRequest state;
    auto board = blackboardRead(state, fixedClock(&now_ms));
    ASSERT_TRUE(board.isOk());
    ASSERT_TRUE(board.value()["value"].empty());
    ASSERT_TRUE(!art.empty());
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
        registerTest("BlackboardTasks.ClaimIsExclusive", test_tasks_claim_is_exclusive);
        registerTest("BlackboardTasks.LeaseExpiryReclaims", test_tasks_lease_expiry_reclaims);
        registerTest("BlackboardTasks.DependenciesGateReadiness",
                     test_tasks_dependencies_gate_readiness);
        registerTest("BlackboardTasks.CycleIsRefused", test_tasks_cycle_is_refused);
        registerTest("BlackboardTasks.OnlyTheHolderCompletes", test_tasks_only_the_holder_completes);
        registerTest("BlackboardTasks.StatusTransitions", test_tasks_status_transitions);
        registerTest("BlackboardTasks.ListFilters", test_tasks_list_filters);
    }
} g_registerBlackboardTests;

} // namespace
