#include "didi/gdextension/property_paths.hpp"

#include <algorithm>
#include <cctype>
#include <map>

namespace didi {
namespace godot {

namespace {

bool endsWith(const std::string& text, const char* suffix) {
    const std::string tail(suffix);
    return text.size() >= tail.size() &&
           std::equal(tail.rbegin(), tail.rend(), text.rbegin(), [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) ==
                      std::tolower(static_cast<unsigned char>(b));
           });
}

std::string lowered(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// The first word of a property name: "theme_override_styles/panel" and
// "theme_override_colors/font_color" share "theme", "bg_color" and
// "border_color" do not.
std::string firstWord(const std::string& name) {
    const auto end = name.find_first_of("_/");
    return lowered(name.substr(0, end));
}

}  // namespace

PropertyPath parsePropertyPath(const std::string& path) {
    PropertyPath parsed;
    if (path.empty()) {
        parsed.problem = "The property path is empty.";
        return parsed;
    }
    if (path.size() > kMaxPropertyPathLength) {
        parsed.problem = "The property path is longer than " +
                         std::to_string(kMaxPropertyPathLength) + " characters.";
        return parsed;
    }
    size_t start = 0;
    while (true) {
        const auto colon = path.find(':', start);
        const auto step = path.substr(start, colon == std::string::npos ? std::string::npos
                                                                       : colon - start);
        if (step.empty()) {
            parsed.steps.clear();
            parsed.problem =
                "\"" + path + "\" has an empty step. A path is a property name, then a colon "
                "before each property inside what it holds, such as "
                "theme_override_styles/panel:bg_color.";
            return parsed;
        }
        // The write goes through a NodePath, which reads a slash as a separator
        // and drops an empty name between two of them. No property is named
        // like that, and a step that changed on the way through would be
        // checked under one name and written under another.
        if (step.front() == '/' || step.back() == '/' || step.find("//") != std::string::npos) {
            parsed.steps.clear();
            parsed.problem = "\"" + step + "\" is not a property name: a name never starts or ends "
                             "with a slash or holds two in a row.";
            return parsed;
        }
        parsed.steps.push_back(step);
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
    if (parsed.steps.size() > kMaxPropertyPathSteps) {
        parsed.steps.clear();
        parsed.problem = "\"" + path + "\" has more than " +
                         std::to_string(kMaxPropertyPathSteps) + " steps.";
    }
    return parsed;
}

ResourceHomeVerdict resourceHome(const std::string& resource_path,
                                 const std::string& edited_scene_path) {
    // A resource made in memory has no path, and the save embeds it in the
    // scene that holds it.
    if (resource_path.empty()) return {ResourceHome::EditedScene, {}};
    const auto separator = resource_path.find("::");
    const std::string file =
        separator == std::string::npos ? resource_path : resource_path.substr(0, separator);
    if (!edited_scene_path.empty() && file == edited_scene_path) {
        return {ResourceHome::EditedScene, {}};
    }
    if (endsWith(file, ".tscn") || endsWith(file, ".scn")) {
        return {ResourceHome::OtherScene, file};
    }
    if (endsWith(file, ".tres") || endsWith(file, ".res")) {
        return {ResourceHome::ResourceFile, file};
    }
    return {ResourceHome::NotSaved, file};
}

std::optional<std::string> excludedPropertyWrite(const std::string& property, bool on_node) {
    // Kept short on purpose. An entry here is a write the layer refuses, so
    // each one needs a reason that is about the scene file, not a preference.
    static const std::map<std::string, std::string> kNodeExclusions = {
        {"owner",
         "A node's owner decides whether the scene file holds it at all; clearing or "
         "changing it drops the node, and its children, from the next save."},
        {"scene_file_path",
         "It marks the node as an instance of another scene, and the next save writes it "
         "as one, without the nodes it holds."},
    };
    static const std::map<std::string, std::string> kResourceExclusions = {
        {"resource_path",
         "It decides which file the resource is saved in, so writing it moves the resource "
         "rather than changing a value in it."},
    };
    const auto& table = on_node ? kNodeExclusions : kResourceExclusions;
    const auto found = table.find(property);
    if (found == table.end()) return std::nullopt;
    return found->second;
}

std::string toolForExcludedWrite(const std::string& property) {
    if (property == "owner") return "scene_reparent_node";
    if (property == "scene_file_path") return "scene_instantiate_node";
    return {};
}

std::optional<std::pair<size_t, size_t>> overlappingWrites(const std::vector<BatchTarget>& targets) {
    for (size_t later = 1; later < targets.size(); ++later) {
        for (size_t earlier = 0; earlier < later; ++earlier) {
            const auto& a = targets[earlier];
            const auto& b = targets[later];
            if (a.node != b.node) continue;
            const auto shared = std::min(a.steps.size(), b.steps.size());
            if (std::equal(a.steps.begin(), a.steps.begin() + static_cast<std::ptrdiff_t>(shared),
                           b.steps.begin())) {
                return std::make_pair(earlier, later);
            }
        }
    }
    return std::nullopt;
}

std::optional<json> declaredConstraint(int hint, const std::string& hint_string) {
    // Godot's PropertyHint numbers. RANGE, ENUM and ENUM_SUGGESTION have been
    // 1, 2 and 3 since 4.0, and RESOURCE_TYPE 17, on every supported line.
    const char* kind = nullptr;
    switch (hint) {
        case 1: kind = "range"; break;
        case 2: kind = "enum"; break;
        case 3: kind = "enum_suggestion"; break;
        case 17: kind = "resource_type"; break;
        default: break;
    }
    if (kind == nullptr || hint_string.empty()) return std::nullopt;
    return json{{"kind", kind}, {"hint_string", hint_string}};
}

std::vector<std::string> propertyNameCandidates(const std::string& missing,
                                                const std::vector<std::string>& available,
                                                size_t limit) {
    const auto wanted = lowered(missing);
    const auto wanted_word = firstWord(missing);
    std::vector<std::pair<int, size_t>> ranked;
    ranked.reserve(available.size());
    for (size_t index = 0; index < available.size(); ++index) {
        const auto name = lowered(available[index]);
        int rank = 3;
        if (name == wanted) {
            rank = 0;
        } else if (!wanted.empty() && (name.find(wanted) != std::string::npos ||
                                       wanted.find(name) != std::string::npos)) {
            rank = 1;
        } else if (!wanted_word.empty() && firstWord(available[index]) == wanted_word) {
            rank = 2;
        }
        ranked.emplace_back(rank, index);
    }
    std::stable_sort(ranked.begin(), ranked.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<std::string> names;
    for (const auto& entry : ranked) {
        if (names.size() >= limit) break;
        names.push_back(available[entry.second]);
    }
    return names;
}

}  // namespace godot
}  // namespace didi
