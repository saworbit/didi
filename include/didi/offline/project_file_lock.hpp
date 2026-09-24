#pragma once

#include "didi/common/types.hpp"
#include "didi/runtime/session_lock.hpp"

#include <filesystem>
#include <memory>
#include <string>

namespace didi::offline {

// Holds the lock for one file at the project root, such as project.godot or
// export_presets.cfg, for as long as the returned handle lives.
//
// A tool that reads one of these files, changes it and writes it back holds
// this across the whole cycle. Two servers on one project are an ordinary
// shape: two agents, or an agent and a subagent. Without the lock their cycles
// overlap, and the second write replaces the first while both report success,
// or on Windows the replace meets the other process's open file and the call
// fails (#929). The blackboard holds its boards the same way.
//
// The lock file lives under .didi/locks, beside the blackboard, and not next to
// the file it guards, so the project tree gains nothing a person has to ignore.
// A holder that does not let go within the wait is refused with 409 and
// retryable: true, because the other cycle is a few milliseconds of work and
// calling again is the whole remedy.
Result<std::shared_ptr<runtime::RuntimeSessionLock>> lockProjectFile(
    const std::filesystem::path& project_root, const std::string& file_name);

} // namespace didi::offline
