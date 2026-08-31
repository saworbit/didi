#include "didi/gdextension/runtime_bridge.hpp"
#include "didi/gdextension/editor_hook.hpp"
#include "didi/gdextension/gdextension_api.hpp"
#include "didi/common/logger.hpp"

#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace didi {
namespace godot {
namespace {

constexpr size_t kOpaqueBytes = 64;
constexpr size_t kMaxRuntimeNodes = 10000;
constexpr size_t kMaxRuntimePathBytes = 1024;
constexpr size_t kMaxRuntimeTreeNameBytes = 1024;
constexpr size_t kMaxRuntimeTreeTypeBytes = 256;
constexpr size_t kMaxRuntimeTreeNodePathBytes = 4096;
constexpr size_t kMaxRuntimeTreeResponseBytes = 256 * 1024;
// The public router adds token-free session provenance from a descriptor whose
// validated on-disk representation is capped at 64 KiB. Keep the engine tree
// sufficiently below the public limit for that provenance and fixed metadata.
constexpr size_t kRuntimeTreeEnvelopeReserveBytes = 72 * 1024;
using Opaque = std::array<std::byte, kOpaqueBytes>;

json errorJson(int code, const std::string& message) {
    return {{"error", {{"code", code}, {"message", message}}}};
}

json liveResult(json result, const std::string& session_kind) {
    result["execution_mode"] = "live";
    result["is_live_engine"] = true;
    result["session_kind"] = session_kind;
    return result;
}

class NativeValue {
public:
    explicit NativeValue(GDExtensionVariantType type) : m_type(type) {}
    NativeValue(const NativeValue&) = delete;
    NativeValue& operator=(const NativeValue&) = delete;
    ~NativeValue() {
        if (m_initialized) {
            auto destructor = GodotApi::instance().variant_get_ptr_destructor(m_type);
            if (destructor) destructor(m_storage.data());
        }
    }
    void* ptr() { return m_storage.data(); }
    const void* ptr() const { return m_storage.data(); }
    void markInitialized() { m_initialized = true; }

private:
    alignas(16) Opaque m_storage{};
    GDExtensionVariantType m_type;
    bool m_initialized{false};
};

class NativeName {
public:
    explicit NativeName(const char* value) {
        auto& api = GodotApi::instance();
        if (api.string_name_new_with_utf8_chars) {
            api.string_name_new_with_utf8_chars(m_storage.data(), value);
            m_initialized = true;
        }
    }
    NativeName(const NativeName&) = delete;
    NativeName& operator=(const NativeName&) = delete;
    ~NativeName() {
        if (m_initialized) {
            auto destructor = GodotApi::instance().variant_get_ptr_destructor(
                GDEXTENSION_VARIANT_TYPE_STRING_NAME);
            if (destructor) destructor(m_storage.data());
        }
    }
    const void* ptr() const { return m_storage.data(); }
    bool valid() const { return m_initialized; }

private:
    alignas(16) Opaque m_storage{};
    bool m_initialized{false};
};

Result<GDExtensionMethodBindPtr> methodBind(const char* class_name,
                                            const char* method_name,
                                            int64_t hash) {
    auto& api = GodotApi::instance();
    NativeName klass(class_name);
    NativeName method(method_name);
    if (!klass.valid() || !method.valid() || !api.classdb_get_method_bind) {
        return Error::internal("Failed to construct Godot runtime method identifiers");
    }
    auto bind = api.classdb_get_method_bind(klass.ptr(), method.ptr(), hash);
    if (!bind) {
        return Error::internal(std::string("Godot method binding unavailable: ") +
                               class_name + "." + method_name);
    }
    return bind;
}

Result<GDExtensionObjectPtr> callObjectResult(GDExtensionObjectPtr object,
                                               const char* class_name,
                                               const char* method_name,
                                               int64_t hash,
                                               const void* const* args = nullptr) {
    if (!object) return Error::notFound(std::string("Cannot call ") + method_name + " on a null Godot object");
    auto bind = methodBind(class_name, method_name, hash);
    if (bind.isErr()) return bind.error();
    GDExtensionObjectPtr result = nullptr;
    GodotApi::instance().object_method_bind_ptrcall(bind.value(), object, args, &result);
    return result;
}

Result<GDExtensionBool> callBoolResult(GDExtensionObjectPtr object,
                                       const char* class_name,
                                       const char* method_name,
                                       int64_t hash,
                                       const void* const* args = nullptr) {
    auto bind = methodBind(class_name, method_name, hash);
    if (bind.isErr()) return bind.error();
    GDExtensionBool result = 0;
    GodotApi::instance().object_method_bind_ptrcall(bind.value(), object, args, &result);
    return result;
}

Result<int64_t> callIntResult(GDExtensionObjectPtr object,
                              const char* class_name,
                              const char* method_name,
                              int64_t hash,
                              const void* const* args = nullptr) {
    auto bind = methodBind(class_name, method_name, hash);
    if (bind.isErr()) return bind.error();
    int64_t result = 0;
    GodotApi::instance().object_method_bind_ptrcall(bind.value(), object, args, &result);
    return result;
}

Result<void> callVoid(GDExtensionObjectPtr object, const char* class_name,
                      const char* method_name, int64_t hash,
                      const void* const* args = nullptr) {
    auto bind = methodBind(class_name, method_name, hash);
    if (bind.isErr()) return bind.error();
    GodotApi::instance().object_method_bind_ptrcall(bind.value(), object, args, nullptr);
    return Result<void>::ok();
}

Result<std::string> nativeStringToUtf8(const void* native_string) {
    auto& api = GodotApi::instance();
    const auto length = api.string_to_utf8_chars(native_string, nullptr, 0);
    if (length < 0) return Error::internal("Godot String UTF-8 conversion failed");
    std::string text(static_cast<size_t>(length), '\0');
    if (length > 0) api.string_to_utf8_chars(native_string, text.data(), length);
    return text;
}

Result<std::string> callStringResult(GDExtensionObjectPtr object,
                                     const char* class_name,
                                     const char* method_name,
                                     int64_t hash,
                                     GDExtensionVariantType result_type) {
    auto bind = methodBind(class_name, method_name, hash);
    if (bind.isErr()) return bind.error();
    NativeValue native(result_type);
    GodotApi::instance().object_method_bind_ptrcall(bind.value(), object, nullptr, native.ptr());
    native.markInitialized();
    if (result_type == GDEXTENSION_VARIANT_TYPE_STRING) {
        return nativeStringToUtf8(native.ptr());
    }
    const int constructor_index = result_type == GDEXTENSION_VARIANT_TYPE_STRING_NAME ? 2 : 3;
    auto constructor = GodotApi::instance().variant_get_ptr_constructor(
        GDEXTENSION_VARIANT_TYPE_STRING, constructor_index);
    if (!constructor) return Error::internal("Missing Godot string-like conversion constructor");
    NativeValue text(GDEXTENSION_VARIANT_TYPE_STRING);
    const void* args[] = {native.ptr()};
    constructor(text.ptr(), args);
    text.markInitialized();
    return nativeStringToUtf8(text.ptr());
}

Result<GDExtensionObjectPtr> activeSceneTree() {
    auto& api = GodotApi::instance();
    if (!api.isLiveReady()) return Error::notConnected("Godot main-loop bridge is not ready");
    NativeName engine_name("Engine");
    if (!engine_name.valid() || !api.global_get_singleton) {
        return Error::internal("Godot Engine singleton API is unavailable");
    }
    auto engine = api.global_get_singleton(engine_name.ptr());
    if (!engine) return Error::notConnected("Godot Engine singleton is unavailable");
    auto main_loop = callObjectResult(engine, "Engine", "get_main_loop", 1016888095LL);
    if (main_loop.isErr()) return main_loop.error();
    if (!main_loop.value()) return Error::notConnected("Godot Engine has no active main loop");

    NativeValue scene_tree_name(GDEXTENSION_VARIANT_TYPE_STRING);
    api.string_new_with_utf8_chars(scene_tree_name.ptr(), "SceneTree");
    scene_tree_name.markInitialized();
    const void* args[] = {scene_tree_name.ptr()};
    auto is_scene_tree = callBoolResult(main_loop.value(), "Object", "is_class", 3927539163LL, args);
    if (is_scene_tree.isErr()) return is_scene_tree.error();
    if (!is_scene_tree.value()) return Error::notConnected("Godot active main loop is not a SceneTree");
    return main_loop.value();
}

Result<GDExtensionObjectPtr> sceneTreeRoot(GDExtensionObjectPtr tree) {
    auto root = callObjectResult(tree, "SceneTree", "get_root", 1757182445LL);
    if (root.isErr()) return root.error();
    if (!root.value()) return Error::notFound("Godot SceneTree has no root Window");
    return root.value();
}

Result<void> validateRuntimePath(const std::string& path) {
    if (path.empty() || path.size() > kMaxRuntimePathBytes || path.find('\0') != std::string::npos) {
        return Error::invalidArgument("root_path must be a non-empty UTF-8 path of at most 1024 bytes");
    }
    if (path != "/root" && path.rfind("/root/", 0) != 0) {
        return Error::invalidArgument("root_path must be a canonical absolute path beneath /root");
    }
    if (path.back() == '/' || path.find("//") != std::string::npos ||
        path.find('\\') != std::string::npos || path.find(':') != std::string::npos) {
        return Error::invalidArgument("root_path must be a canonical absolute NodePath");
    }
    size_t start = 1;
    while (start <= path.size()) {
        const auto end = path.find('/', start);
        const auto segment = path.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (segment.empty() || segment == "." || segment == ".." || segment.front() == '%') {
            return Error::invalidArgument(
                "root_path may not contain empty, '.', '..', or unique-name alias segments");
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return Result<void>::ok();
}

Result<GDExtensionObjectPtr> resolveRuntimeNode(GDExtensionObjectPtr root,
                                                const std::string& path) {
    auto valid = validateRuntimePath(path);
    if (valid.isErr()) return valid.error();
    if (path == "/root") return root;

    auto& api = GodotApi::instance();
    NativeValue native_string(GDEXTENSION_VARIANT_TYPE_STRING);
    api.string_new_with_utf8_chars(native_string.ptr(), path.substr(6).c_str());
    native_string.markInitialized();
    auto constructor = api.variant_get_ptr_constructor(GDEXTENSION_VARIANT_TYPE_NODE_PATH, 2);
    if (!constructor) return Error::internal("Godot NodePath(String) constructor is unavailable");
    NativeValue node_path(GDEXTENSION_VARIANT_TYPE_NODE_PATH);
    const void* constructor_args[] = {native_string.ptr()};
    constructor(node_path.ptr(), constructor_args);
    node_path.markInitialized();
    const void* args[] = {node_path.ptr()};
    auto node = callObjectResult(root, "Node", "get_node_or_null", 2734337346LL, args);
    if (node.isErr()) return node.error();
    if (!node.value()) return Error::notFound("Runtime node not found: " + path);
    return node.value();
}

struct TraversalState {
    size_t node_count{0};
    size_t estimated_tree_bytes{0};
    bool truncated{false};
};

size_t estimateNodeBytes(const json& node, int depth, bool needs_separator) {
    (void)depth;
    // CallToolResult::successJson emits compact JSON, so there is no indentation
    // to charge for any more. What is left is the node itself, the "children":[]
    // wrapper each one gains once it is nested, and the comma before a sibling.
    constexpr size_t kChildrenArrayOverhead = 14;
    return node.dump().size() + kChildrenArrayOverhead + (needs_separator ? 1 : 0);
}

Result<json> serializeRuntimeNode(GDExtensionObjectPtr node, int depth,
                                  int max_depth, bool needs_separator,
                                  TraversalState& state) {
    if (state.node_count >= kMaxRuntimeNodes) {
        state.truncated = true;
        return json(nullptr);
    }
    auto name = callStringResult(node, "Node", "get_name", 2002593661LL,
                                 GDEXTENSION_VARIANT_TYPE_STRING_NAME);
    auto type = callStringResult(node, "Object", "get_class", 201670096LL,
                                 GDEXTENSION_VARIANT_TYPE_STRING);
    auto path = callStringResult(node, "Node", "get_path", 4075236667LL,
                                 GDEXTENSION_VARIANT_TYPE_NODE_PATH);
    GDExtensionBool include_internal = 0;
    const void* count_args[] = {&include_internal};
    auto child_count = callIntResult(node, "Node", "get_child_count", 894402480LL,
                                     count_args);
    if (name.isErr()) return name.error();
    if (type.isErr()) return type.error();
    if (path.isErr()) return path.error();
    if (child_count.isErr()) return child_count.error();

    auto bounded_name = boundUtf8(name.value(), kMaxRuntimeTreeNameBytes);
    auto bounded_type = boundUtf8(type.value(), kMaxRuntimeTreeTypeBytes);
    auto bounded_path = boundUtf8(path.value(), kMaxRuntimeTreeNodePathBytes);
    json result = {{"name", std::move(bounded_name.value)},
                   {"type", std::move(bounded_type.value)},
                   {"path", std::move(bounded_path.value)}, {"child_count", child_count.value()},
                   {"children", json::array()}};
    if (bounded_name.truncated) result["name_truncated"] = true;
    if (bounded_type.truncated) result["type_truncated"] = true;
    if (bounded_path.truncated) result["path_truncated"] = true;
    if (depth >= max_depth && child_count.value() > 0) {
        result["children_truncated"] = true;
        state.truncated = true;
    }
    const size_t node_bytes = estimateNodeBytes(result, depth, needs_separator);
    const size_t tree_budget = kMaxRuntimeTreeResponseBytes - kRuntimeTreeEnvelopeReserveBytes;
    if (node_bytes > tree_budget -
                         std::min(state.estimated_tree_bytes, tree_budget)) {
        state.truncated = true;
        return json(nullptr);
    }
    state.estimated_tree_bytes += node_bytes;
    ++state.node_count;
    if (depth >= max_depth) {
        return result;
    }

    for (int64_t index = 0; index < child_count.value(); ++index) {
        if (state.node_count >= kMaxRuntimeNodes) {
            result["children_truncated"] = true;
            state.truncated = true;
            break;
        }
        GDExtensionBool include_child_internal = 0;
        const void* child_args[] = {&index, &include_child_internal};
        auto child = callObjectResult(node, "Node", "get_child", 541253412LL, child_args);
        if (child.isErr()) return child.error();
        if (!child.value()) return Error::internal("Godot returned a null runtime child node");
        auto child_json = serializeRuntimeNode(child.value(), depth + 1, max_depth,
                                               !result["children"].empty(), state);
        if (child_json.isErr()) return child_json.error();
        if (child_json.value().is_null()) {
            result["children_truncated"] = true;
            break;
        }
        result["children"].push_back(std::move(child_json.value()));
    }
    return result;
}

Result<bool> sceneTreePaused(GDExtensionObjectPtr tree) {
    auto paused = callBoolResult(tree, "SceneTree", "is_paused", 36873697LL);
    if (paused.isErr()) return paused.error();
    return paused.value() != 0;
}

Result<void> setSceneTreePaused(GDExtensionObjectPtr tree, bool paused) {
    GDExtensionBool requested = paused ? 1 : 0;
    const void* args[] = {&requested};
    return callVoid(tree, "SceneTree", "set_pause", 2586408642LL, args);
}

bool integerInRange(const json& value, int64_t minimum, int64_t maximum) {
    if (!value.is_number_integer() && !value.is_number_unsigned()) return false;
    if (value.is_number_integer()) {
        const auto number = value.get<int64_t>();
        return number >= minimum && number <= maximum;
    }
    const auto number = value.get<uint64_t>();
    return number >= static_cast<uint64_t>(minimum) &&
           number <= static_cast<uint64_t>(maximum);
}

} // namespace

BoundedUtf8 boundUtf8(std::string_view input, size_t maximum_bytes) {
    BoundedUtf8 bounded;
    bounded.value.reserve(std::min(input.size(), maximum_bytes));
    for (size_t index = 0; index < input.size();) {
        const auto first = static_cast<unsigned char>(input[index]);
        size_t width = 1;
        bool valid = false;
        uint32_t codepoint = 0;
        if ((first & 0x80) == 0) {
            valid = true;
            codepoint = first;
        } else if ((first & 0xE0) == 0xC0 && first >= 0xC2) {
            width = 2;
            valid = true;
            codepoint = first & 0x1F;
        } else if ((first & 0xF0) == 0xE0) {
            width = 3;
            valid = true;
            codepoint = first & 0x0F;
        } else if ((first & 0xF8) == 0xF0 && first <= 0xF4) {
            width = 4;
            valid = true;
            codepoint = first & 0x07;
        }
        valid = valid && index + width <= input.size();
        for (size_t offset = 1; valid && offset < width; ++offset) {
            const auto next = static_cast<unsigned char>(input[index + offset]);
            valid = (next & 0xC0) == 0x80;
            codepoint = (codepoint << 6) | (next & 0x3F);
        }
        valid = valid && (width == 1 || (width == 2 && codepoint >= 0x80) ||
                          (width == 3 && codepoint >= 0x800) ||
                          (width == 4 && codepoint >= 0x10000));
        valid = valid && !(codepoint >= 0xD800 && codepoint <= 0xDFFF) &&
                codepoint <= 0x10FFFF;
        if (!valid) {
            bounded.truncated = true;
            if (bounded.value.size() + 1 > maximum_bytes) break;
            bounded.value.push_back('?');
            ++index;
            continue;
        }
        if (bounded.value.size() + width > maximum_bytes) {
            bounded.truncated = true;
            break;
        }
        bounded.value.append(input.substr(index, width));
        index += width;
    }
    if (bounded.value.size() < input.size()) bounded.truncated = true;
    return bounded;
}

Result<void> quitSceneTree(int64_t exit_code) {
    auto tree = activeSceneTree();
    if (tree.isErr()) return tree.error();
    const void* args[] = {&exit_code};
    auto requested = callVoid(tree.value(), "SceneTree", "quit", 1995695955LL, args);
    return requested.isOk() ? Result<void>::ok() : Result<void>(requested.error());
}

json executeRuntimeBridge(const std::string& method, const json& params,
                          const std::string& session_kind) {
    if (!params.is_object()) return errorJson(400, "Runtime params must be an object");
    if (session_kind != "editor" && session_kind != "game") {
        return errorJson(500, "Runtime session kind is unavailable");
    }
    if (method != "runtime.getTree" && session_kind != "game") {
        return errorJson(409, "Runtime execution control is available only for game sessions");
    }

    auto tree = activeSceneTree();
    if (tree.isErr()) return errorJson(tree.error().code, tree.error().message);

    if (method == "runtime.getTree") {
        if (params.contains("root_path") && !params["root_path"].is_string()) {
            return errorJson(400, "root_path must be a string");
        }
        if (params.contains("max_depth") && !integerInRange(params["max_depth"], 0, 16)) {
            return errorJson(400, "max_depth must be an integer from 0 to 16");
        }
        const auto root_path = params.value("root_path", std::string("/root"));
        auto valid_path = validateRuntimePath(root_path);
        if (valid_path.isErr()) return errorJson(valid_path.error().code, valid_path.error().message);
        const int max_depth = params.value("max_depth", 4);
        auto root = sceneTreeRoot(tree.value());
        if (root.isErr()) return errorJson(root.error().code, root.error().message);
        auto target = resolveRuntimeNode(root.value(), root_path);
        if (target.isErr()) return errorJson(target.error().code, target.error().message);
        TraversalState state;
        auto serialized = serializeRuntimeNode(target.value(), 0, max_depth, false, state);
        if (serialized.isErr()) return errorJson(serialized.error().code, serialized.error().message);
        if (serialized.value().is_null()) {
            return errorJson(507, "Runtime tree root exceeds the serialized response budget");
        }
        auto paused = sceneTreePaused(tree.value());
        if (paused.isErr()) return errorJson(paused.error().code, paused.error().message);
        auto response = liveResult({{"root_path", root_path}, {"scene_tree", serialized.value()},
                                    {"paused", paused.value()}, {"node_count", state.node_count},
                                    {"max_nodes", kMaxRuntimeNodes}, {"max_depth", max_depth},
                                    {"max_response_bytes", kMaxRuntimeTreeResponseBytes},
                                    {"truncated", state.truncated}},
                                   session_kind);
        if (response.dump().size() > kMaxRuntimeTreeResponseBytes) {
            return errorJson(507, "Runtime tree response exceeds the 256 KiB serialized budget");
        }
        return response;
    }

    if (method == "runtime.setPaused") {
        if (!params.contains("paused") || !params["paused"].is_boolean()) {
            return errorJson(400, "paused must be a boolean");
        }
        const bool requested = params["paused"].get<bool>();
        auto changed = setSceneTreePaused(tree.value(), requested);
        if (changed.isErr()) return errorJson(changed.error().code, changed.error().message);
        auto observed = sceneTreePaused(tree.value());
        if (observed.isErr()) return errorJson(observed.error().code, observed.error().message);
        if (observed.value() != requested) {
            return errorJson(500, "Godot SceneTree pause state did not match the requested value");
        }
        DIDI_LOG_INFO("RUNTIME_BRIDGE", requested ? "Game paused" : "Game resumed");
        return liveResult({{"status", "success"}, {"paused", observed.value()}}, session_kind);
    }

    if (method == "runtime.stop") {
        if (params.contains("exit_code") && !integerInRange(params["exit_code"], 0, 255)) {
            return errorJson(400, "exit_code must be an integer from 0 to 255");
        }
        const int64_t exit_code = params.value("exit_code", 0);
        // Deliberately not calling quit here. SceneTree.quit exits the main loop
        // at the end of the current iteration, which can tear the process down
        // before the IPC worker has framed and written this response, so the
        // caller sees a broken pipe instead of the exit code. Hand it to the
        // frame pump instead and answer now.
        EditorHook::instance().requestSceneTreeQuit(exit_code);
        DIDI_LOG_INFO("RUNTIME_BRIDGE", "Game shutdown requested with exit code ", exit_code);
        return liveResult({{"status", "success"}, {"shutdown_requested", true},
                           {"exit_code", exit_code}}, session_kind);
    }

    return errorJson(404, "Unknown runtime bridge method: " + method);
}

} // namespace godot
} // namespace didi
