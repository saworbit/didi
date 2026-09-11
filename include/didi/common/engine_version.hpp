#pragma once

#include <string>
#include "didi/common/types.hpp"

namespace didi {
namespace versions {

// "Godot Engine v4.7.stable.official" and "Godot v4.5.1.stable.official" are
// the two spellings in play, one from the shipped dump's header and one from
// the engine itself. Only the major and minor decide whether the API a caller
// is reading matches the engine in front of them, so only those are compared;
// a patch difference is not a mismatch worth shouting about.
inline std::string majorMinorOf(const std::string& version) {
    const auto v = version.find_first_of("0123456789");
    if (v == std::string::npos) return {};
    const auto dot = version.find('.', v);
    if (dot == std::string::npos) return {};
    const auto end = version.find_first_not_of("0123456789", dot + 1);
    return version.substr(v, (end == std::string::npos ? version.size() : end) - v);
}

// What a caller needs to weigh a verdict read out of the shipped API dump.
// `checked: true` reads as "verified against your engine", and CI covers three
// engine lines, so a gap between the pinned dump and the attached engine is the
// normal case rather than an edge one (#466). An unknown version is not a
// match, and saying nothing would read as one.
inline void annotateApiVersion(json& target,
                               const std::string& api_version,
                               const std::string& attached_engine_version) {
    target["attached_engine_version"] =
        attached_engine_version.empty() ? json(nullptr) : json(attached_engine_version);
    const auto pinned_line = majorMinorOf(api_version);
    const auto engine_line = majorMinorOf(attached_engine_version);
    if (pinned_line.empty() || engine_line.empty()) {
        target["api_version_matches_attached_engine"] = nullptr;
    } else {
        target["api_version_matches_attached_engine"] = pinned_line == engine_line;
    }
}

} // namespace versions
} // namespace didi
