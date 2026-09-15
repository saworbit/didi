#pragma once

#include <cctype>
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

// What a caller needs to weigh a verdict a spawned Godot produced.
//
// script_check_syntax and shader_check_compile both launch an engine to answer
// "will the engine accept this?", and resolveGodotExecutable picks newest-first
// from a hardcoded list, so on a machine with 4.5, 4.6 and 4.7 installed both
// answered about a 4.5 project using 4.7 and neither said so (#617). The same
// shape #466 gave script_reflect_class: name the version, and say whether it is
// the engine in front of the caller. An unknown version is not a match, and
// saying nothing would read as one.
// What GODOT_BIN was set to and why it was not used, beside the executable that
// ran instead.
//
// A GODOT_BIN that cannot be used was discarded in silence and engine_executable
// named the fallback, so the one field that could have shown a user their
// variable was ignored named something they never set (#656). Absent when the
// variable is unset or was used, so a normal answer is unchanged.
inline void annotateConfiguredEngine(json& target, const std::string& configured,
                                     const std::string& rejected_because) {
    if (configured.empty() || rejected_because.empty()) return;
    target["engine_executable_configured"] = configured;
    target["engine_executable_configured_rejected"] = rejected_because;
}

inline void annotateCheckEngine(json& target,
                                const std::string& engine_version,
                                const std::string& engine_executable,
                                const std::string& attached_engine_version) {
    target["engine_version"] = engine_version.empty() ? json(nullptr) : json(engine_version);
    target["engine_executable"] =
        engine_executable.empty() ? json(nullptr) : json(engine_executable);
    target["attached_engine_version"] =
        attached_engine_version.empty() ? json(nullptr) : json(attached_engine_version);
    const auto ran_line = majorMinorOf(engine_version);
    const auto attached_line = majorMinorOf(attached_engine_version);
    if (ran_line.empty() || attached_line.empty()) {
        target["matches_attached_engine"] = nullptr;
    } else {
        target["matches_attached_engine"] = ran_line == attached_line;
    }
}

// The engine line a project declares for itself. project.godot carries
// config/features=PackedStringArray("4.5", "Forward Plus"), written by the
// editor that last saved the project, and the first quoted entry that reads as
// a version is that line. Empty when the literal has no such entry.
inline std::string featuresVersionOf(const std::string& features_literal) {
    size_t open = features_literal.find('"');
    while (open != std::string::npos) {
        const auto close = features_literal.find('"', open + 1);
        if (close == std::string::npos) break;
        const auto entry = features_literal.substr(open + 1, close - open - 1);
        if (!entry.empty() && std::isdigit(static_cast<unsigned char>(entry.front())) &&
            !majorMinorOf(entry).empty()) {
            return majorMinorOf(entry);
        }
        open = features_literal.find('"', close + 1);
    }
    return {};
}

// The offline half of annotateApiVersion. With no session selected there is
// no engine to compare the pinned dump against, but the project still says
// which line it was saved by, and a 4.5 project read against a 4.7 dump is
// the same skew whether or not an editor happens to be attached (#555).
inline void annotateProjectFeatures(json& target,
                                    const std::string& api_version,
                                    const std::string& features_literal) {
    const auto project_line = featuresVersionOf(features_literal);
    target["project_features_version"] =
        project_line.empty() ? json(nullptr) : json(project_line);
    const auto pinned_line = majorMinorOf(api_version);
    if (pinned_line.empty() || project_line.empty()) {
        target["api_version_matches_project_features"] = nullptr;
    } else {
        target["api_version_matches_project_features"] = pinned_line == project_line;
    }
}

} // namespace versions
} // namespace didi
