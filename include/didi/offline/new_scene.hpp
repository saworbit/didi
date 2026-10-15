#pragma once

#include "didi/common/types.hpp"

#include <filesystem>
#include <string>

namespace didi::offline {

// scene_create with no editor attached (#1380): a .tscn holding one root node.
//
// Measured on 4.5.1, 4.6.2 and 4.7.2 for every Node class the engine can
// construct: a file of the header and one [node] line loads to the same stored
// state as the scene the engine packs and saves for that root. The engine writes
// more on some lines, unique_id from 4.6 and layout_mode for a Control root, and
// none of it changes what loads, so the editor adds it when it next saves.
struct NewSceneRequest {
    std::string scene_path;
    std::string root_type{"Node2D"};
    std::string root_name{"Root"};
    bool overwrite{false};
};

struct NewSceneWrite {
    // The readers' spelling of where the file landed.
    std::string scene_path;
    std::string root_type;
    // As the file holds it, after the rename Node.set_name makes.
    std::string root_name;
    std::string uid;
    std::string text;
    bool replaced{false};
};

// The name Node.set_name gives a node. The engine turns . : @ " and % into _
// and keeps every other character, while a loaded file keeps whatever name it
// holds, so the file has to hold the name the editor's route would have set.
std::string engineNodeName(const std::string& name);

// The file: the gd_scene header with its uid, and the root's [node] line with
// the name escaped the way the engine's text saver escapes it, except where the
// engine's parser would read the escape back as a different character.
std::string newSceneText(const std::string& root_type, const std::string& root_name,
                         const std::string& uid);

Result<NewSceneWrite> createSceneFile(const std::filesystem::path& project_root,
                                      const NewSceneRequest& request);

} // namespace didi::offline
