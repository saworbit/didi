#pragma once

#include "didi/common/types.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace didi::setup {

// One build's identity, as CMakeLists.txt composes it: the version, the commit
// and the moment the build tree was configured, `2.0.1+8e11c31e41e1.20260910T044028`.
//
// The server and the extension each carry exactly one of these, compiled in
// from the same configure. Two binaries are the same build only when the whole
// string matches; the version alone cannot tell a fresh build from a stale one,
// which is how a field trial spent an hour against a bridge six days older than
// its server (#326).
struct BuildId {
    std::string text;
    int major{0};
    int minor{0};
    int patch{0};
    std::string commit;  // twelve hex digits, or "nogit"
    std::string stamp;   // YYYYMMDDTHHMMSS, UTC
};

std::optional<BuildId> parseBuildId(std::string_view text);

// Which of two builds is newer, read as a person upgrading would read it: the
// higher version, and for one version the later configure. Two builds that
// share a version and a stamp but not a commit cannot be ordered, and saying
// so is the honest answer.
enum class BuildOrder { Same, Older, Newer, Unknown };
BuildOrder compareBuilds(const BuildId& subject, const BuildId& reference);
const char* buildOrderWord(BuildOrder order);

// The build id compiled into a binary, read out of its bytes.
//
// This is the fact the engine would publish once it had loaded the library, so
// a copy that was never loaded can be judged before anyone opens an editor.
// Absent when the binary carries none (a library older than the field), and an
// error when it carries two different ones or cannot be read.
Result<std::optional<BuildId>> readBuildIdFromBinary(const std::filesystem::path& binary);

// This process's own build.
BuildId ownBuildId();

}  // namespace didi::setup
