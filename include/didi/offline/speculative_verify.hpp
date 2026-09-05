#pragma once

#include "didi/common/json.hpp"
#include "didi/common/types.hpp"

#include <optional>
#include <string>
#include <vector>

namespace didi::offline {

// Checking a change before the working tree ever sees it.
//
// The point is not that one file parses. `script_check_syntax` already answers
// that from source text alone, without writing anything. What it cannot answer
// is whether a set of files is consistent with each other, because a script
// that preloads a sibling is only correct when that sibling is the proposed one
// rather than the one still on disk. That needs the whole set present together,
// somewhere that is not the project a person is working in.
//
// So this builds an isolated copy of the project, writes the proposal into it,
// checks it there, and takes the copy away again. Nothing it does can be seen
// from the working tree, which is what makes it safe to run on a proposal that
// turns out to be wrong.
struct SpeculativeChange {
    // As given, for reporting back.
    std::string path;
    // Project-relative and already checked for traversal and containment.
    std::string relative;
    std::string content;
};

constexpr size_t kMaxSpeculativeChanges = 64;
constexpr size_t kMaxSpeculativeContentBytes = 1024u * 1024u;

struct SpeculativeVerifyRequest {
    std::vector<SpeculativeChange> changes;
    int timeout_seconds{120};
    // A scene to open in the copy once the proposal is written, so the check is
    // more than a parse. Parsing says a file is well formed; it says nothing
    // about a scene that fails to load, an @onready path that resolves to
    // nothing, or a _ready() that divides by zero. Empty when no run was asked
    // for, because a run costs an engine start and not every proposal needs one.
    std::string run_scene;
    // Project-relative and already checked for traversal and containment.
    std::string run_scene_relative;
    // Iterations to let the scene run before Godot quits by itself, so a game
    // that would never exit still ends.
    int run_frames{120};
};

struct SpeculativeScriptVerdict {
    std::string path;
    bool ok{false};
    // What the engine said, bounded. Empty when it said nothing.
    std::string detail;
};

// What happened when the scene was opened in the copy.
//
// The exit code is not the whole answer. Godot leaves a runtime script error on
// its error stream and still exits 0, so a run judged on the exit code alone
// would call a broken scene fine. The error lines are read as well, which is
// what execute_test_session already does for a scene in the working tree.
struct SpeculativeSceneRun {
    std::string path;
    // False when a proposed script did not parse. Running then costs an engine
    // start to learn what the parse already said, and the load failure it
    // produces reads as a runtime fault when it is not one. Reported rather
    // than left out, so an absent run and a skipped run cannot be confused.
    bool ran{false};
    bool ok{false};
    int exit_code{0};
    int frames{0};
    bool timed_out{false};
    // Error-level lines the engine printed, bounded and in order.
    std::vector<std::string> errors;
};

struct SpeculativeVerifyResult {
    // The commit the isolated copy was built from, so a caller knows what the
    // proposal was checked against rather than assuming it was checked against
    // whatever is on screen.
    std::string base_commit;
    // Whether uncommitted work was carried across. A check run without it is
    // answering a question about a project nobody has.
    bool carried_uncommitted{false};
    // Untracked files are not carried, and a proposal that depends on one would
    // be checked against a project missing it. Named rather than counted.
    std::vector<std::string> untracked_excluded;
    std::vector<SpeculativeScriptVerdict> scripts;
    // Present only when a run was asked for.
    std::optional<SpeculativeSceneRun> scene_run;
    int written{0};
    bool all_ok{false};

    json toJson() const;
};

// A proposal that was checked and then, only if it passed, put in place.
//
// The verification is run here rather than taken on trust from an earlier call.
// A caller that verified a minute ago is describing a project that may have
// moved since, and the point of this is that what reaches the working tree is
// the thing that was just proved, not the thing that was proved earlier.
struct SpeculativeApplyResult {
    SpeculativeVerifyResult verification;
    bool applied{false};
    // The paths that reached the working tree, as the caller wrote them.
    std::vector<std::string> written;

    json toJson() const;
};

// Rejects anything the sandbox could not honestly check: a path outside the
// project, a traversal, an empty set, or a file this cannot parse.
Result<SpeculativeVerifyRequest> parseSpeculativeVerifyRequest(const json& params);

// Requires the project to sit inside a git work tree, because that is what
// makes an isolated copy cheap enough to build per call. Refuses rather than
// falling back to copying a whole project directory, which for a Godot project
// means its imported assets too.
Result<SpeculativeVerifyResult> verifyChangesInSandbox(const SpeculativeVerifyRequest& request);

// Checks the proposal in the sandbox and, if it passed, writes it into the
// working tree. Every file is staged before any is replaced, so the change
// cannot stop half applied because the last file was the one that could not be
// written. A verification that did not pass writes nothing and is reported as
// it stands.
Result<SpeculativeApplyResult> applyVerifiedChanges(const SpeculativeVerifyRequest& request);

} // namespace didi::offline
