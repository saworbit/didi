#pragma once

#include "didi/common/types.hpp"

#include <string>

namespace didi::offline {

// Project-wide audit of what references what.
//
// A project accumulates textures from earlier iterations, scenes pointing at
// files that were renamed, and signals nobody listens to. None of that is
// visible from any single file, so an agent asked to change "the character
// sprite" cannot tell which of five candidates is the one actually wired up.
//
// Everything here is a static read of the project on disk. It follows the
// reference forms Godot writes and that people type:
//
//   [ext_resource path="res://..."]   and its uid="uid://..." form
//   preload("res://...") and load("res://...") in GDScript
//   GD.Load<T>("res://...") and ResourceLoader.Load("res://...") in C#
//
// What it cannot follow is a path a script builds at runtime, so the answers
// are evidence rather than verdicts. Nothing here says "safe to delete"; it
// says what it found and what it looked at.
struct ProjectAuditOptions {
    // Orphan detection covers asset types only: textures, audio, meshes, fonts
    // and shaders. Scenes and scripts are excluded on purpose. A scene that
    // nothing references is usually a level you open by hand, not rubbish, and
    // reporting it as an orphan would train people to ignore the tool.
    bool include_orphans{true};
    bool include_broken_references{true};
    bool include_dead_signals{true};
    bool include_import_health{true};
    // A cap so one call cannot return an unbounded list.
    size_t max_findings{500};
};

json auditProject(const std::string& root_dir, const ProjectAuditOptions& options);

} // namespace didi::offline
