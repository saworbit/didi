#pragma once

// The engine-independent half of the change journal's undo references (Q15 in
// docs/BUILD_QUEUE.md). The bridge reads the editor's histories around every
// command it runs and asks this file what the command committed, and later
// whether one of those commits can still be undone on its own. Everything here
// is decided from numbers and names, so it is tested without an engine; that
// the editor agrees is asserted by the live harness on every engine line.
//
// Three facts about Godot's UndoRedo shape it, measured on 4.5.1, 4.6.2 and
// 4.7.2:
//
// - get_version goes up on a commit and a redo and down on an undo, so after an
//   undo and a new commit the same version names a different action. A version
//   alone is not an identity.
// - The editor's own Undo, the only undo that keeps EditorUndoRedoManager's
//   stacks true (#913), takes whichever of the edited scene's history and the
//   global one has the newer action. Which one is newer is not readable, so the
//   commit numbers this ledger hands out stand in for the editor's timestamps.
// - Clearing a history, which closing or reloading its scene does, raises its
//   version and empties it.

#include "didi/common/json.hpp"
#include "didi/common/types.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace didi {
namespace godot {

// The global history's id in EditorUndoRedoManager.
inline constexpr int64_t kGlobalUndoHistory = 0;

// Commits remembered per editor run. A reference older than that is still
// judged against its own history; only the comparison with the other history
// loses its evidence, and is then refused rather than guessed.
inline constexpr size_t kUndoLedgerCapacity = 4096;

// References one undoStatus request may ask about.
inline constexpr size_t kMaxUndoStatusRefs = 256;

// One of the editor's histories as UndoRedo reports it.
struct UndoHistoryState {
    int64_t id = kGlobalUndoHistory;
    int64_t version = 0;
    // get_current_action: the index of the action an undo would take, -1 when
    // there is none.
    int64_t current = -1;
    // get_history_count, redo entries included.
    int64_t count = 0;
    // get_current_action_name, empty when current is -1.
    std::string action;
    // The scene a scene history belongs to, empty for the global one.
    std::string scene_path;
};

// One action a command committed, as the journal keeps it.
struct UndoCommit {
    std::string run;
    uint64_t serial = 0;
    int64_t history_id = kGlobalUndoHistory;
    std::string scene_path;
    int64_t index = -1;
    int64_t version = 0;
    std::string action;

    [[nodiscard]] json toJson() const;
    // Refuses a reference missing a field or carrying the wrong type, with the
    // field it lacks. A reference comes back from the journal file, which a
    // person can edit.
    static std::optional<UndoCommit> fromJson(const json& value, std::string* problem = nullptr);
};

// Whether one commit can be undone on its own now.
enum class UndoState {
    // On top of its history, and the editor's Undo would take it.
    available,
    // On top of its history, but the editor's Undo would take something else
    // first: a newer action in the other history, or its scene is not the
    // current tab.
    blocked,
    // Still applied, under later actions in the same history.
    later_history,
    // Undone, and still in the history's redo entries.
    undone,
    // Not in the history any more: cleared with its scene, replaced after an
    // undo, or recorded by an earlier editor run.
    gone,
};

[[nodiscard]] const char* undoStateName(UndoState state);

struct UndoVerdict {
    UndoState state = UndoState::gone;
    std::string reason;
    // later_history: how many actions are above it.
    int64_t later_actions = 0;
    // blocked: the newer action in the other history, and which history.
    std::string newer_action;
    std::string newer_history;
    // blocked by its scene not being the current tab.
    bool needs_scene_tab = false;

    [[nodiscard]] json toJson() const;
};

class UndoLedger {
public:
    explicit UndoLedger(std::string run, size_t capacity = kUndoLedgerCapacity);

    const std::string& run() const { return m_run; }

    // The commits a command made in one history, given that history before and
    // after it. A history whose version did not rise made none. names_at
    // answers get_action_name for an index; a command that committed more than
    // one action needs the names under the top one.
    std::vector<UndoCommit> observe(const UndoHistoryState& before,
                                    const UndoHistoryState& after,
                                    const std::function<std::string(int64_t)>& names_at);

    // The newest remembered commit that is a history's current action, if
    // Didi made it in this run.
    [[nodiscard]] std::optional<UndoCommit> topOf(const UndoHistoryState& history) const;

    // A later commit in the same history at a version no higher than this one
    // can only follow an undo of it, so this one was replaced.
    [[nodiscard]] bool replacedLater(const UndoCommit& commit) const;

    // Judges a reference against its own history and the other history the
    // editor's Undo would weigh it against. own_is_candidate says whether its
    // history is one the editor's Undo considers at all: the global one, or the
    // edited scene's. other is that other candidate, absent when there is none
    // (no scene open). name_at reads get_action_name in its own history.
    [[nodiscard]] UndoVerdict evaluate(const UndoCommit& ref, const UndoHistoryState& own,
                                       bool own_is_candidate,
                                       const std::optional<UndoHistoryState>& other,
                                       const std::function<std::string(int64_t)>& name_at) const;

private:
    std::string m_run;
    size_t m_capacity;
    uint64_t m_next_serial = 1;
    std::deque<UndoCommit> m_commits;
};

}  // namespace godot
}  // namespace didi
