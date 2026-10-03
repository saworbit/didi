#include "didi/gdextension/undo_ledger.hpp"

#include <utility>

namespace didi {
namespace godot {

json UndoCommit::toJson() const {
    json out = {{"run", run},
                {"serial", serial},
                {"history_id", history_id},
                {"index", index},
                {"version", version},
                {"action", action}};
    if (!scene_path.empty()) out["scene_path"] = scene_path;
    return out;
}

std::optional<UndoCommit> UndoCommit::fromJson(const json& value, std::string* problem) {
    const auto refuse = [problem](const std::string& field) -> std::optional<UndoCommit> {
        if (problem) *problem = field;
        return std::nullopt;
    };
    if (!value.is_object()) return refuse("reference");
    UndoCommit commit;
    const auto run = value.find("run");
    if (run == value.end() || !run->is_string()) return refuse("run");
    commit.run = run->get<std::string>();
    const auto serial = value.find("serial");
    if (serial == value.end() || !serial->is_number_unsigned()) return refuse("serial");
    commit.serial = serial->get<uint64_t>();
    const auto integer = [&value](const char* key, int64_t& out) {
        const auto found = value.find(key);
        if (found == value.end() || !found->is_number_integer()) return false;
        out = found->get<int64_t>();
        return true;
    };
    if (!integer("history_id", commit.history_id)) return refuse("history_id");
    if (!integer("index", commit.index)) return refuse("index");
    if (!integer("version", commit.version)) return refuse("version");
    const auto action = value.find("action");
    if (action == value.end() || !action->is_string()) return refuse("action");
    commit.action = action->get<std::string>();
    if (const auto scene = value.find("scene_path"); scene != value.end()) {
        if (!scene->is_string()) return refuse("scene_path");
        commit.scene_path = scene->get<std::string>();
    }
    return commit;
}

const char* undoStateName(UndoState state) {
    switch (state) {
    case UndoState::available: return "available";
    case UndoState::blocked: return "blocked";
    case UndoState::later_history: return "later_history";
    case UndoState::undone: return "undone";
    case UndoState::gone: return "gone";
    }
    return "gone";
}

json UndoVerdict::toJson() const {
    json out = {{"state", undoStateName(state)}, {"reason", reason}};
    if (state == UndoState::later_history) out["later_actions"] = later_actions;
    if (!newer_action.empty() || !newer_history.empty()) {
        out["newer_action"] = newer_action;
        out["newer_history"] = newer_history;
    }
    if (needs_scene_tab) out["needs_scene_tab"] = true;
    return out;
}

UndoLedger::UndoLedger(std::string run, size_t capacity)
    : m_run(std::move(run)), m_capacity(capacity == 0 ? 1 : capacity) {}

std::vector<UndoCommit> UndoLedger::observe(const UndoHistoryState& before,
                                            const UndoHistoryState& after,
                                            const std::function<std::string(int64_t)>& names_at) {
    std::vector<UndoCommit> made;
    if (before.id != after.id || after.version <= before.version) return made;
    // A cleared history also has a higher version, and nothing to point at.
    if (after.current < 0 || after.count == 0) return made;
    const int64_t steps = after.version - before.version;
    for (int64_t k = 0; k < steps; ++k) {
        const int64_t index = after.current - (steps - 1 - k);
        if (index < 0) continue;
        UndoCommit commit;
        commit.run = m_run;
        commit.serial = m_next_serial++;
        commit.history_id = after.id;
        commit.scene_path = after.scene_path;
        commit.index = index;
        commit.version = before.version + 1 + k;
        commit.action = k == steps - 1 ? after.action : (names_at ? names_at(index) : std::string());
        m_commits.push_back(commit);
        while (m_commits.size() > m_capacity) m_commits.pop_front();
        made.push_back(std::move(commit));
    }
    return made;
}

std::optional<UndoCommit> UndoLedger::topOf(const UndoHistoryState& history) const {
    if (history.current < 0) return std::nullopt;
    for (auto it = m_commits.rbegin(); it != m_commits.rend(); ++it) {
        if (it->history_id == history.id && it->index == history.current &&
            it->version == history.version && it->action == history.action) {
            return *it;
        }
    }
    return std::nullopt;
}

bool UndoLedger::replacedLater(const UndoCommit& commit) const {
    for (auto it = m_commits.rbegin(); it != m_commits.rend(); ++it) {
        if (it->serial <= commit.serial) break;
        if (it->run == commit.run && it->history_id == commit.history_id &&
            it->version <= commit.version) {
            return true;
        }
    }
    return false;
}

UndoVerdict UndoLedger::evaluate(const UndoCommit& ref, const UndoHistoryState& own,
                                 bool own_is_candidate,
                                 const std::optional<UndoHistoryState>& other,
                                 const std::function<std::string(int64_t)>& name_at) const {
    UndoVerdict verdict;
    if (ref.run != m_run) {
        verdict.reason = "Recorded by an earlier editor run. The editor's undo history ends "
                         "with the editor, so there is nothing left to undo it with.";
        return verdict;
    }
    // Commits, undos and redos move the version and the current index
    // together, so their difference only changes when the history is cleared
    // or trimmed, and then an index no longer names the same action.
    if (ref.index < 0 || own.count <= ref.index ||
        own.version - own.current != ref.version - ref.index) {
        verdict.reason = "No longer in its history. Closing or reloading its scene clears the "
                         "history, and a change made after undoing it replaces it.";
        return verdict;
    }
    if ((name_at ? name_at(ref.index) : std::string()) != ref.action || replacedLater(ref)) {
        verdict.reason = "Replaced. It was undone and a later change took its place in the history.";
        return verdict;
    }
    if (own.current < ref.index) {
        verdict.state = UndoState::undone;
        verdict.reason = "Already undone. It is still in the history's redo entries until "
                         "something new is committed there.";
        return verdict;
    }
    if (own.current > ref.index) {
        verdict.state = UndoState::later_history;
        verdict.later_actions = own.current - ref.index;
        verdict.reason = "Still applied, under " + std::to_string(verdict.later_actions) +
                         (verdict.later_actions == 1 ? " later action" : " later actions") +
                         " in the same history, which depend on it. The editor undoes only "
                         "the newest action of a history.";
        return verdict;
    }
    if (!own_is_candidate) {
        verdict.state = UndoState::blocked;
        verdict.needs_scene_tab = true;
        verdict.reason = "On top of its scene's history, but that scene is not the current tab. "
                         "The editor's Undo reaches only the current scene's history and the "
                         "global one.";
        return verdict;
    }
    if (other.has_value() && other->current >= 0) {
        const auto top = topOf(*other);
        if (!top.has_value() || top->serial > ref.serial) {
            verdict.state = UndoState::blocked;
            verdict.newer_action = other->action;
            verdict.newer_history = other->id == kGlobalUndoHistory ? "global" : "scene";
            verdict.reason =
                top.has_value()
                    ? "The editor's Undo would take a newer change in the " +
                          verdict.newer_history + " history first (\"" + other->action + "\")."
                    : "The " + verdict.newer_history + " history's last action (\"" +
                          other->action +
                          "\") was not made by Didi in this editor run, so which of the two is "
                          "newer cannot be told, and the editor's Undo might take it instead.";
            return verdict;
        }
    }
    verdict.state = UndoState::available;
    verdict.reason = "On top of its history; the editor's Undo takes it next.";
    return verdict;
}

}  // namespace godot
}  // namespace didi
