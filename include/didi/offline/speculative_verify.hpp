#pragma once

#include "didi/common/json.hpp"
#include "didi/common/types.hpp"

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
};

struct SpeculativeScriptVerdict {
    std::string path;
    bool ok{false};
    // What the engine said, bounded. Empty when it said nothing.
    std::string detail;
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
    int written{0};
    bool all_ok{false};

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

} // namespace didi::offline
