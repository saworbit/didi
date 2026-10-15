#include "didi/offline/new_scene.hpp"

#include "didi/common/atomic_write.hpp"
#include "didi/common/engine_version.hpp"
#include "didi/common/project_path.hpp"
#include "didi/offline/class_reference.hpp"
#include "didi/offline/project_file_lock.hpp"
#include "didi/offline/resource_uid.hpp"

#include <fstream>
#include <optional>
#include <sstream>

namespace didi::offline {
namespace {

Error invalid(const std::string& field, const std::string& message) {
    return Error(400, message,
                 json{{"code", "invalid_arguments"}, {"field", field}, {"retryable", false}});
}

// The escapes the engine's text saver writes in a node's name, measured on
// 4.5.1, 4.6.2 and 4.7.2. It also writes \a and \v, and its parser reads those
// back as the letters a and v, so the engine's own file loses the character.
// They are written as they are instead, which the parser keeps.
std::string escaped(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char c : text) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\'': out += "\\'"; break;
            case '"': out += "\\\""; break;
            default: out += c;
        }
    }
    return out;
}

// Why root_type cannot root a scene, judged from the class reference the way
// the editor's route judges it from ClassDB: the class exists, it inherits
// Node, and it can be constructed. The reference has no record of a script's
// class_name, and ClassDB has none either, so both routes refuse one.
std::optional<Error> rootTypeProblem(const std::string& root_type) {
    const auto& reference = ClassReference::instance();
    if (!reference.loaded()) {
        return Error(422,
                     "root_type could not be checked, because the class reference this server "
                     "reads is missing. With an editor attached, scene_create checks it against "
                     "the engine instead.",
                     json{{"code", "root_type_not_judged"}, {"retryable", false}});
    }
    const auto line = versions::majorMinorOf(reference.apiVersion());
    const auto* record = root_type == "@GlobalScope" ? nullptr : reference.find(root_type);
    if (!record) {
        return invalid("root_type", "root_type must be a Godot class that inherits Node, and the Godot " +
                                        line + " class reference has no class named " + root_type + ".");
    }
    bool node = false;
    std::string name = root_type;
    for (int depth = 0; depth < 64 && !name.empty(); ++depth) {
        if (name == "Node") {
            node = true;
            break;
        }
        const auto* current = reference.find(name);
        if (!current) break;
        name = current->value("inherits", std::string());
    }
    if (!node) {
        return invalid("root_type", "root_type must inherit Node, and " + root_type +
                                        " does not, so no scene can be rooted in it.");
    }
    if (!record->value("is_instantiable", false)) {
        return invalid("root_type", "root_type " + root_type +
                                        " cannot be constructed, because the engine declares it "
                                        "abstract. Use a class that inherits it.");
    }
    return std::nullopt;
}

} // namespace

std::string engineNodeName(const std::string& name) {
    std::string out = name;
    for (auto& c : out) {
        if (c == '.' || c == ':' || c == '@' || c == '"' || c == '%') c = '_';
    }
    return out;
}

std::string newSceneText(const std::string& root_type, const std::string& root_name,
                         const std::string& uid) {
    return "[gd_scene format=3 uid=\"" + uid + "\"]\n\n[node name=\"" + escaped(root_name) +
           "\" type=\"" + root_type + "\"]\n";
}

Result<NewSceneWrite> createSceneFile(const std::filesystem::path& project_root,
                                      const NewSceneRequest& request) {
    namespace fs = std::filesystem;
    // The editor's route takes a res:// path ending in .tscn, in that case, and
    // writes it with the text saver.
    if (!strings::endsWith(request.scene_path, ".tscn")) {
        return invalid("scene_path", "scene_path must be a res:// path ending in .tscn, the text "
                                     "scene format.");
    }
    auto resolved = paths::resolveProjectFileForWrite(request.scene_path);
    if (resolved.isErr()) {
        auto error = resolved.error();
        if (!error.data.is_object()) error.data = json::object();
        error.data["field"] = "scene_path";
        return error;
    }
    const fs::path target = resolved.value();
    const auto scene_path = paths::resourcePathOf(target);

    // The same refusals, in the same words, as the editor's route.
    if (request.root_type.empty() || request.root_type.size() > 128) {
        return invalid("root_type", "root_type must be a Godot class name that inherits Node");
    }
    if (request.root_name.empty() || request.root_name.find('/') != std::string::npos ||
        request.root_name.find('\\') != std::string::npos) {
        return invalid("root_name", "root_name must be a non-empty node name without path separators");
    }
    if (auto problem = rootTypeProblem(request.root_type)) return *problem;

    // Held from the existence check through the read-back, so a second creator
    // either sees this file or keeps its uid.
    auto lock = lockProjectFile(project_root, scene_path.substr(6));
    if (lock.isErr()) return lock.error();
    std::error_code exists_error;
    const bool exists = fs::exists(target, exists_error);
    if (exists_error) {
        return Error(500, "Whether " + scene_path + " exists could not be read: " + exists_error.message());
    }
    if (exists && !request.overwrite) {
        return Error(409, "Scene target already exists; pass overwrite: true to replace it",
                     json{{"code", "already_exists"}, {"retry_with", {{"overwrite", true}}}});
    }
    // A replaced scene keeps its uid, as it does when the editor saves over it,
    // so every scene that references it by uid still finds it.
    auto uid = resourceUidForWrite(target);
    if (uid.isErr()) return uid.error();

    NewSceneWrite write;
    write.scene_path = scene_path;
    write.root_type = request.root_type;
    write.root_name = engineNodeName(request.root_name);
    write.uid = uid.value();
    write.text = newSceneText(write.root_type, write.root_name, write.uid);
    write.replaced = exists;

    // The editor's route creates the directory too, because ResourceSaver
    // cannot write into one that is not there.
    std::error_code directory_error;
    if (target.has_parent_path()) fs::create_directories(target.parent_path(), directory_error);
    if (directory_error) {
        return Error(500, "The directory for " + scene_path + " could not be created: " +
                              directory_error.message());
    }
    auto written = files::writeFileAtomically(target, write.text);
    if (written.isErr()) {
        return Error(500, "The scene file could not be written: " + written.error().message);
    }
    std::ifstream check(target, std::ios::binary);
    std::ostringstream stored;
    stored << check.rdbuf();
    if (stored.str() != write.text) {
        return Error(500, "The scene was written and reads back different from what was written: " +
                              scene_path);
    }
    return write;
}

} // namespace didi::offline
