#pragma once

#include "didi/common/types.hpp"
#include <filesystem>
#include <string>
#include <string_view>

namespace didi::offline {

// Whether text is a UID as Godot's ResourceUID::text_to_id reads one: uid://
// and then at least one character, each a-z or 0-9. Every check of a uid's
// form goes through this, so the index, the audit and the impact analysis
// agree on what one is (#1446). A caller that only routes uid:// text to the
// engine, which answers validity itself, needs the prefix and not this.
bool isResourceUidText(std::string_view text);

struct ResourceUidHeader {
    bool matched{false};
    bool complete{false};
    std::string uid;
};

// Only the opening gd_resource/gd_scene tag owns the file's embedded UID.
// Leading whitespace, UTF-8 BOM, and semicolon comments are accepted.
ResourceUidHeader inspectTextResourceUidHeader(const std::string& text);

// Preserve an existing root UID, else allocate a Godot-format random 63-bit
// UID distinct from the indexed project UIDs. Existing root headers not fully
// read within the first 64 KiB are refused rather than replacing identity. The
// caller holds the file's project lock from this read through write/read-back.
Result<std::string> resourceUidForWrite(const std::filesystem::path& target);

} // namespace didi::offline
