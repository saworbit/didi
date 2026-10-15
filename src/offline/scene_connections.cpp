#include "didi/offline/scene_connections.hpp"

#include "didi/common/atomic_write.hpp"
#include "didi/common/project_path.hpp"
#include "didi/common/scene_node_path.hpp"
#include "didi/offline/class_reference.hpp"
#include "didi/offline/gdscript_diagnostics.hpp"
#include "didi/offline/project_file_lock.hpp"
#include "didi/offline/project_text_scan.hpp"

#include <algorithm>
#include <fstream>
#include <optional>
#include <regex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace didi::offline {
namespace {

constexpr uintmax_t kMaxSceneBytes = 16 * 1024 * 1024;

Error notJudged(const std::string& why) {
    return Error(422,
                 "The connection could not be judged offline: " + why +
                     " With an editor attached, signal_connect asks the engine instead.",
                 json{{"code", "connection_not_judged"}, {"retryable", false}});
}

// One quoted attribute of a section header, unescaped as Godot reads it.
std::optional<std::string> headerAttribute(const std::string& line, const std::string& name) {
    const auto key = " " + name + "=\"";
    const auto start = line.find(key);
    if (start == std::string::npos) return std::nullopt;
    std::string value;
    for (auto at = start + key.size(); at < line.size(); ++at) {
        if (line[at] == '\\' && at + 1 < line.size()) {
            value += line[++at];
        } else if (line[at] == '"') {
            return value;
        } else {
            value += line[at];
        }
    }
    return std::nullopt;
}

std::string quoted(const std::string& text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + "\"";
}

std::optional<std::string> extResourceId(const std::string& text) {
    static const std::regex id(R"re(ExtResource\(\s*"([^"]{1,256})"\s*\))re");
    std::smatch match;
    if (std::regex_search(text, match, id)) return match[1].str();
    return std::nullopt;
}

struct SceneNode {
    std::string type;
    std::string script_id;  // empty for none; "\n" for one that is not an ext_resource
    bool instanced{false};
};

struct SceneConnection {
    std::string signal;
    std::string from;
    std::string to;
    std::string method;
};

struct SceneText {
    std::vector<std::string> lines;  // without their terminators
    bool crlf{false};
    std::string root_name;
    bool inherited{false};
    std::unordered_map<std::string, std::string> resources;
    std::unordered_map<std::string, SceneNode> nodes;
    std::vector<std::string> instanced;
    std::vector<SceneConnection> connections;
    int last_connection{-1};
    int first_editable{-1};
};

std::string withoutDotSlash(std::string path) {
    if (path.rfind("./", 0) == 0) path = path.substr(2);
    return path.empty() ? std::string(".") : path;
}

SceneText readScene(const std::string& text) {
    SceneText scene;
    std::size_t start = 0;
    std::size_t crlf = 0;
    while (start <= text.size()) {
        const auto newline = text.find('\n', start);
        std::string line = text.substr(start, newline == std::string::npos ? std::string::npos : newline - start);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
            ++crlf;
        }
        if (newline == std::string::npos) {
            if (!line.empty()) scene.lines.push_back(line);
            break;
        }
        scene.lines.push_back(line);
        start = newline + 1;
    }
    scene.crlf = crlf > 0 && crlf * 2 >= scene.lines.size();

    SceneNode* current = nullptr;
    bool root_seen = false;
    for (std::size_t index = 0; index < scene.lines.size(); ++index) {
        const auto& line = scene.lines[index];
        if (line.rfind("[", 0) == 0) current = nullptr;
        if (line.rfind("[ext_resource ", 0) == 0) {
            const auto id = headerAttribute(line, "id");
            const auto path = headerAttribute(line, "path");
            if (id && path) scene.resources[*id] = *path;
        } else if (line.rfind("[node ", 0) == 0) {
            const auto name = headerAttribute(line, "name");
            if (!name) continue;
            const auto parent = headerAttribute(line, "parent");
            std::string path;
            if (!parent) {
                if (root_seen) continue;
                root_seen = true;
                scene.root_name = *name;
                path = ".";
            } else {
                path = *parent == "." ? *name : withoutDotSlash(*parent) + "/" + *name;
            }
            auto& node = scene.nodes[path];
            node.type = headerAttribute(line, "type").value_or("");
            node.instanced = line.find(" instance=ExtResource(") != std::string::npos;
            if (node.instanced) {
                scene.instanced.push_back(path);
                if (path == ".") scene.inherited = true;
            }
            current = &node;
        } else if (current && line.rfind("script = ", 0) == 0) {
            current->script_id = extResourceId(line).value_or("\n");
        } else if (line.rfind("[connection ", 0) == 0) {
            scene.last_connection = static_cast<int>(index);
            scene.connections.push_back({headerAttribute(line, "signal").value_or(""),
                                         withoutDotSlash(headerAttribute(line, "from").value_or("")),
                                         withoutDotSlash(headerAttribute(line, "to").value_or("")),
                                         headerAttribute(line, "method").value_or("")});
        } else if (line.rfind("[editable ", 0) == 0 && scene.first_editable < 0) {
            scene.first_editable = static_cast<int>(index);
        }
    }
    return scene;
}

// A node argument as the scene file names the node, by the rules the live
// tool resolves it with.
Result<std::string> sceneRelative(const std::string& requested, const std::string& root_name) {
    if (requested.empty() || requested == "/root" || requested == ".") return std::string(".");
    if (auto refused = paths::refuseParentRelativeNodePath(requested)) return *refused;
    std::string relative = requested;
    if (relative.rfind("/root/", 0) == 0) relative = relative.substr(6);
    if (relative == root_name) return std::string(".");
    if (relative.rfind(root_name + "/", 0) == 0) relative = relative.substr(root_name.size() + 1);
    while (!relative.empty() && relative.back() == '/') relative.pop_back();
    return withoutDotSlash(relative);
}

// The arguments in a declaration's parameter or signal argument text, split
// at the commas outside brackets.
std::vector<std::string> parameterList(const std::string& text) {
    std::vector<std::string> out;
    std::string current;
    int depth = 0;
    for (const char c : text) {
        if (c == '(' || c == '[' || c == '{') ++depth;
        if ((c == ')' || c == ']' || c == '}') && depth > 0) --depth;
        if (c == ',' && depth == 0) {
            out.push_back(strings::trim(current));
            current.clear();
            continue;
        }
        current += c;
    }
    if (!strings::trim(current).empty()) out.push_back(strings::trim(current));
    return out;
}

struct Arity {
    int required{0};
    int total{0};
    bool vararg{false};
};

Arity arityOf(const std::vector<std::string>& parameters) {
    Arity arity;
    for (const auto& parameter : parameters) {
        if (parameter.rfind("...", 0) == 0) {
            arity.vararg = true;
            continue;
        }
        ++arity.total;
        if (parameter.find('=') == std::string::npos) ++arity.required;
    }
    return arity;
}

// What a chain of scripts and engine classes declares. nullopt from a lookup
// means some step did not resolve.
struct Found {
    bool found{false};
    std::optional<int> signal_arity;  // a script's signal; unknown for an engine one
    std::optional<Arity> method_arity;
};

std::optional<Found> engineSignal(std::string class_name, const std::string& signal) {
    const auto& reference = ClassReference::instance();
    if (!reference.loaded()) return std::nullopt;
    for (int depth = 0; depth < 64 && !class_name.empty(); ++depth) {
        const json* record = reference.find(class_name);
        if (!record) return std::nullopt;
        const auto signals = record->find("signals");
        if (signals != record->end() && signals->is_array() &&
            std::find(signals->begin(), signals->end(), json(signal)) != signals->end()) {
            return Found{true, std::nullopt, std::nullopt};
        }
        class_name = record->value("inherits", "");
    }
    if (!class_name.empty()) return std::nullopt;
    return Found{};
}

std::optional<Found> engineMethod(std::string class_name, const std::string& method) {
    const auto& reference = ClassReference::instance();
    if (!reference.loaded()) return std::nullopt;
    for (int depth = 0; depth < 64 && !class_name.empty(); ++depth) {
        const json* record = reference.find(class_name);
        if (!record) return std::nullopt;
        const auto methods = record->find("methods");
        if (methods != record->end() && methods->is_object() && methods->contains(method)) {
            std::vector<std::string> parameters;
            const auto& args = (*methods)[method].value("args", json::array());
            for (const auto& arg : args) {
                if (arg.is_string()) parameters.push_back(arg.get<std::string>());
            }
            return Found{true, std::nullopt, arityOf(parameters)};
        }
        class_name = record->value("inherits", "");
    }
    if (!class_name.empty()) return std::nullopt;
    return Found{};
}

class ScriptIndex {
public:
    explicit ScriptIndex(const std::filesystem::path& project_root) {
        static const std::regex class_name(R"re(^class_name\s+([A-Za-z_][A-Za-z0-9_]*))re");
        auto scan = scanProjectText(paths::projectPathToUtf8(project_root), ScanIndex::fresh);
        for (auto& source : scan.sources) {
            if (!strings::endsWith(source.path, ".gd")) continue;
            std::istringstream lines(source.contents);
            std::string line;
            while (std::getline(lines, line)) {
                std::smatch match;
                if (std::regex_search(line, match, class_name)) {
                    m_class_names[match[1].str()] = source.path;
                    break;
                }
            }
            m_scripts[source.path] = std::move(source.contents);
        }
    }

    std::optional<Found> signalOf(const std::string& script, const std::string& signal) {
        return walk(script, [&](const json& symbols) -> std::optional<Found> {
            for (const auto& declared : symbols.value("signals", json::array())) {
                if (declared.value("name", "") != signal) continue;
                return Found{true,
                             static_cast<int>(parameterList(declared.value("arguments", "")).size()),
                             std::nullopt};
            }
            return std::nullopt;
        }, [&](const std::string& engine) { return engineSignal(engine, signal); });
    }

    std::optional<Found> methodOf(const std::string& script, const std::string& method) {
        return walk(script, [&](const json& symbols) -> std::optional<Found> {
            for (const auto& declared : symbols.value("functions", json::array())) {
                if (declared.value("name", "") != method) continue;
                return Found{true, std::nullopt, arityOf(parameterList(declared.value("parameters", "")))};
            }
            return std::nullopt;
        }, [&](const std::string& engine) { return engineMethod(engine, method); });
    }

private:
    template <typename Own, typename Engine>
    std::optional<Found> walk(std::string path, Own own, Engine engine) {
        static const std::regex extends(
            R"re(^(?:class_name\s+\S+\s+)?extends\s+("[^"]*"|[A-Za-z_][A-Za-z0-9_.]*))re");
        std::unordered_set<std::string> visited;
        while (visited.insert(path).second) {
            const auto script = m_scripts.find(path);
            if (script == m_scripts.end()) return std::nullopt;
            auto symbols = m_symbols.find(path);
            if (symbols == m_symbols.end()) {
                symbols = m_symbols.emplace(path, GDScriptDiagnostics::extractSymbols(script->second)).first;
            }
            if (symbols->second.value("truncated", false)) return std::nullopt;
            if (auto found = own(symbols->second)) return found;
            std::string base;
            std::istringstream lines(script->second);
            std::string line;
            while (std::getline(lines, line)) {
                std::smatch match;
                if (std::regex_search(line, match, extends)) {
                    base = match[1].str();
                    break;
                }
            }
            // No extends line means RefCounted, which is what Godot assumes.
            if (base.empty()) return engine("RefCounted");
            if (base.front() == '"') {
                path = base.substr(1, base.size() - 2);
                if (path.rfind("res://", 0) != 0) return std::nullopt;
                continue;
            }
            const auto named = m_class_names.find(base);
            if (named != m_class_names.end()) {
                path = named->second;
                continue;
            }
            if (base.find('.') != std::string::npos) return std::nullopt;
            return engine(base);
        }
        return std::nullopt;
    }

    std::unordered_map<std::string, std::string> m_scripts;
    std::unordered_map<std::string, std::string> m_class_names;
    std::unordered_map<std::string, json> m_symbols;
};

// The script a node runs, or the engine class it is, as a judge can read it.
struct NodeShape {
    std::string script;  // res:// .gd, empty for none
    std::string type;
};

Result<NodeShape> shapeOf(const SceneText& scene, const std::string& path, const std::string& argument,
                          const std::string& scene_path) {
    for (const auto& instance : scene.instanced) {
        if (path == instance || (instance == "." ? path != "." : path.rfind(instance + "/", 0) == 0)) {
            return notJudged(argument + " is inside or is an instanced scene, whose nodes and scripts "
                                        "this file does not declare.");
        }
    }
    const auto node = scene.nodes.find(path);
    if (node == scene.nodes.end()) {
        return Error::notFound("Scene node not found: " + argument + " (searched " + scene_path + ")");
    }
    NodeShape shape;
    shape.type = node->second.type;
    if (!node->second.script_id.empty()) {
        const auto resource = scene.resources.find(node->second.script_id);
        if (resource == scene.resources.end() || !strings::endsWith(resource->second, ".gd")) {
            return notJudged(argument + " has a script that is built into the scene or is not GDScript.");
        }
        shape.script = resource->second;
    } else if (shape.type.empty()) {
        return notJudged(argument + " declares no type.");
    }
    return shape;
}

} // namespace

std::string connectionLine(const std::string& signal, const std::string& from, const std::string& to,
                           const std::string& method, int64_t flags) {
    std::string line = "[connection signal=" + quoted(signal) + " from=" + quoted(from) +
                       " to=" + quoted(to) + " method=" + quoted(method);
    if (flags != 2) line += " flags=" + std::to_string(flags);
    return line + "]";
}

Result<SceneConnectionWrite> connectSignalInScene(const std::filesystem::path& project_root,
                                                  const SceneConnectionRequest& request) {
    const auto& requested = request.scene_path;
    std::string lowered = requested;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (!strings::endsWith(lowered, ".tscn")) {
        return Error(400, "scene_path must name a text scene, a .tscn file.",
                     json{{"code", "invalid_arguments"}, {"field", "scene_path"}, {"retryable", false}});
    }
    auto resolved = paths::resolveProjectFile(requested);
    if (resolved.isErr()) {
        auto error = resolved.error();
        if (!error.data.is_object()) error.data = json::object();
        error.data["field"] = "scene_path";
        return error;
    }
    const auto file = resolved.value();
    const auto scene_path = paths::resourcePathOf(file);
    std::error_code size_error;
    const auto size = std::filesystem::file_size(file, size_error);
    if (size_error || size > kMaxSceneBytes) {
        return Error(413, "The scene file is larger than 16 MiB, or its size cannot be read.",
                     json{{"code", "file_too_large"}, {"field", "scene_path"}});
    }

    // Built before the lock: the project's scripts are read, not written.
    ScriptIndex scripts(project_root);

    // Held from the read to the write, so another writer's change to the same
    // scene is wholly before this one or wholly after it.
    auto lock = lockProjectFile(project_root, scene_path.substr(6));
    if (lock.isErr()) return lock.error();
    std::string text;
    {
        std::ifstream input(file, std::ios::binary);
        if (!input) return Error(500, "The scene file could not be opened: " + scene_path);
        std::ostringstream buffer;
        buffer << input.rdbuf();
        text = buffer.str();
    }
    auto scene = readScene(text);
    if (scene.root_name.empty()) {
        return notJudged(scene_path + " declares no root node.");
    }
    if (scene.inherited) {
        return notJudged(scene_path + " inherits another scene, which declares its nodes.");
    }
    auto from = sceneRelative(request.emitter_node, scene.root_name);
    if (from.isErr()) return from.error();
    auto to = sceneRelative(request.target_node, scene.root_name);
    if (to.isErr()) return to.error();
    auto emitter = shapeOf(scene, from.value(), request.emitter_node, scene_path);
    if (emitter.isErr()) return emitter.error();
    auto target = shapeOf(scene, to.value(), request.target_node, scene_path);
    if (target.isErr()) return target.error();

    const auto signal = emitter.value().script.empty()
                            ? engineSignal(emitter.value().type, request.signal_name)
                            : scripts.signalOf(emitter.value().script, request.signal_name);
    if (!signal) {
        return notJudged("what " + request.emitter_node + " declares does not resolve to a script in "
                         "the project and an engine class the class reference names.");
    }
    if (!signal->found) {
        return Error(404, "Looked for signal '" + request.signal_name + "' on " + request.emitter_node + ".",
                     json{{"code", "declared_signal_not_found"},
                          {"emitter_node", request.emitter_node},
                          {"signal_name", request.signal_name}});
    }
    const auto method = target.value().script.empty()
                            ? engineMethod(target.value().type, request.target_method)
                            : scripts.methodOf(target.value().script, request.target_method);
    if (!method) {
        return notJudged("what " + request.target_node + " declares does not resolve to a script in "
                         "the project and an engine class the class reference names.");
    }
    if (!method->found) {
        return Error(404, "Looked for method '" + request.target_method + "' on " + request.target_node + ".",
                     json{{"code", "target_method_not_found"},
                          {"target_node", request.target_node},
                          {"target_method", request.target_method}});
    }
    SceneConnectionWrite write;
    write.scene_path = scene_path;
    write.from = from.value();
    write.to = to.value();
    if (signal->signal_arity && method->method_arity) {
        write.arity_checked = true;
        const auto arguments = *signal->signal_arity;
        const auto& takes = *method->method_arity;
        if (arguments < takes.required || (!takes.vararg && arguments > takes.total)) {
            return Error(409,
                         "The signal passes " + std::to_string(arguments) + " argument(s) and " +
                             request.target_method + " takes " + std::to_string(takes.required) +
                             (takes.vararg ? " or more" : " to " + std::to_string(takes.total)) + ".",
                         json{{"code", "signal_target_arity_incompatible"}, {"retryable", false}});
        }
    }
    for (const auto& existing : scene.connections) {
        if (existing.signal == request.signal_name && existing.from == write.from &&
            existing.to == write.to && existing.method == request.target_method) {
            return Error(409, "The scene already connects that signal to that method.",
                         json{{"code", "signal_connection_already_exists"}, {"retryable", false}});
        }
    }

    write.line = connectionLine(request.signal_name, write.from, write.to, request.target_method, request.flags);
    // Where Godot puts it: after the last connection, else before the first
    // [editable] section, else at the end after a blank line.
    auto lines = scene.lines;
    if (scene.last_connection >= 0) {
        lines.insert(lines.begin() + scene.last_connection + 1, write.line);
    } else {
        const auto at = scene.first_editable >= 0 ? static_cast<std::size_t>(scene.first_editable) : lines.size();
        std::vector<std::string> block;
        if (at > 0 && !strings::trim(lines[at - 1]).empty()) block.push_back("");
        block.push_back(write.line);
        if (scene.first_editable >= 0) block.push_back("");
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(at), block.begin(), block.end());
    }
    const std::string ending = scene.crlf ? "\r\n" : "\n";
    std::string edited;
    edited.reserve(text.size() + write.line.size() + 8);
    for (const auto& line : lines) edited += line + ending;
    auto written = files::writeFileAtomically(file, edited);
    if (written.isErr()) return Error(500, "The scene file could not be written: " + written.error().message);

    // Read back, so the answer reports what the file holds.
    std::ifstream check(file, std::ios::binary);
    std::ostringstream after;
    after << check.rdbuf();
    if (after.str().find(write.line) == std::string::npos) {
        return Error(500, "The scene was written and the connection is not in it when read back.");
    }
    return write;
}

} // namespace didi::offline
