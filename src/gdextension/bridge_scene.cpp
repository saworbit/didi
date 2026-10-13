#include "bridge_internal.hpp"
#include "didi/gdextension/godot_bridge.hpp"
#include "didi/gdextension/godot_object.hpp"
#include "didi/gdextension/runtime_bridge.hpp"
#include "didi/gdextension/property_paths.hpp"
#include "didi/common/godot_error.hpp"
#include <algorithm>
#include <functional>
#include <utility>

namespace didi::godot {
using namespace bridge_internal;
namespace {
struct PropertyDescriptor {
    int declared_type{0};
    std::string class_name;
    // What the engine declares about the values the property takes. Godot
    // fills these in on every property it exports, and they are the only
    // account of a rejected value that does not require knowing the class.
    // See notAppliedReport.
    int hint{0};
    std::string hint_string;
};


Result<std::optional<PropertyDescriptor>> findPropertyDescriptor(GDExtensionObjectPtr object,
                                                                 const std::string& property) {
    auto properties = callObject(object, "Object", "get_property_list", 3995934104LL);
    if (properties.isErr()) return properties.error();
    auto size_value = callVariant(properties.value(), "size");
    if (size_value.isErr()) return size_value.error();
    auto size = scalarFromVariant<int64_t>(size_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
    if (size.isErr()) return size.error();
    auto name_key = makeString("name");
    if (name_key.isErr()) return name_key.error();
    auto type_key = makeString("type");
    if (type_key.isErr()) return type_key.error();
    auto class_key = makeString("class_name");
    if (class_key.isErr()) return class_key.error();
    auto hint_key = makeString("hint");
    if (hint_key.isErr()) return hint_key.error();
    auto hint_string_key = makeString("hint_string");
    if (hint_string_key.isErr()) return hint_string_key.error();

    for (int64_t i = 0; i < size.value(); ++i) {
        auto index = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, i);
        if (index.isErr()) return index.error();
        auto descriptor = callVariant(properties.value(), "get", {&index.value()});
        if (descriptor.isErr()) return descriptor.error();
        auto name_value = callVariant(descriptor.value(), "get", {&name_key.value()});
        if (name_value.isErr()) return name_value.error();
        const auto type = GodotApi::instance().variant_get_type(name_value.value().ptr());
        if (type != GDEXTENSION_VARIANT_TYPE_STRING && type != GDEXTENSION_VARIANT_TYPE_STRING_NAME) {
            continue;
        }
        auto name = stringFromVariant(name_value.value(), type);
        if (name.isErr()) return name.error();
        if (name.value() != property) continue;

        PropertyDescriptor found;
        auto declared_value = callVariant(descriptor.value(), "get", {&type_key.value()});
        if (declared_value.isOk()) {
            auto declared = scalarFromVariant<int64_t>(declared_value.value(),
                                                       GDEXTENSION_VARIANT_TYPE_INT);
            if (declared.isOk()) found.declared_type = static_cast<int>(declared.value());
        }
        auto class_value = callVariant(descriptor.value(), "get", {&class_key.value()});
        if (class_value.isOk()) {
            const auto class_type = GodotApi::instance().variant_get_type(class_value.value().ptr());
            if (class_type == GDEXTENSION_VARIANT_TYPE_STRING ||
                class_type == GDEXTENSION_VARIANT_TYPE_STRING_NAME) {
                auto class_text = stringFromVariant(class_value.value(), class_type);
                if (class_text.isOk()) found.class_name = class_text.value();
            }
        }
        auto hint_value = callVariant(descriptor.value(), "get", {&hint_key.value()});
        if (hint_value.isOk()) {
            auto hint = scalarFromVariant<int64_t>(hint_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
            if (hint.isOk()) found.hint = static_cast<int>(hint.value());
        }
        auto hint_string_value = callVariant(descriptor.value(), "get", {&hint_string_key.value()});
        if (hint_string_value.isOk()) {
            const auto hint_string_type =
                GodotApi::instance().variant_get_type(hint_string_value.value().ptr());
            if (hint_string_type == GDEXTENSION_VARIANT_TYPE_STRING ||
                hint_string_type == GDEXTENSION_VARIANT_TYPE_STRING_NAME) {
                auto hint_text = stringFromVariant(hint_string_value.value(), hint_string_type);
                if (hint_text.isOk()) found.hint_string = hint_text.value();
            }
        }
        return std::optional<PropertyDescriptor>(found);
    }
    return std::optional<PropertyDescriptor>{};
}


std::optional<json> previewMutationPreconditions(GDExtensionObjectPtr root, GDExtensionObjectPtr node,
                                                 const std::string& described_path,
                                                 const json& mutation) {
    if (!mutation.is_object()) return std::nullopt;
    const std::string tool = mutation.value("tool", "");
    const json arguments = mutation.value("arguments", json::object());
    auto string_argument = [&](const char* name) -> std::optional<std::string> {
        if (!arguments.is_object() || !arguments.contains(name) || !arguments[name].is_string()) {
            return std::nullopt;
        }
        return arguments[name].get<std::string>();
    };
    if (tool == "scene_remove_node" || tool == "scene_duplicate_node" ||
        tool == "scene_reparent_node") {
        if (node == root) return errorJson(400, "Cannot mutate the edited scene root");
        if (auto refused = refuseUnsavableEdit(root, node, described_path, SceneEdit::Structure)) {
            return refused;
        }
        if (auto new_parent_path = string_argument("new_parent_path")) {
            auto new_parent = resolveNode(root, *new_parent_path);
            if (new_parent.isErr()) return errorJson(new_parent.error());
            if (auto refused = refuseUnsavableEdit(root, new_parent.value(), *new_parent_path,
                                                   SceneEdit::AddChild)) {
                return refused;
            }
        }
        return std::nullopt;
    }
    if (tool == "scene_instantiate_node") {
        if (auto refused = refuseUnsavableEdit(root, node, described_path, SceneEdit::AddChild)) {
            return refused;
        }
        if (auto scene_path = string_argument("scene_path")) {
            auto valid_scene = validateResPath(*scene_path, ".tscn");
            if (valid_scene.isErr()) {
                return errorJson(valid_scene.error());
            }
            auto present = packedSceneExists(*scene_path);
            if (present.isErr()) return errorJson(present.error());
            if (!present.value()) return errorJson(404, "PackedScene not found: " + *scene_path);
            if (auto refused = refuseInstanceCycle(root, *scene_path)) return refused;
        }
        return std::nullopt;
    }
    // scene_set_property is not here: a dry run of it runs the write's whole
    // check, in getSceneProperties.
    if (tool == "scene_add_to_group" || tool == "scene_remove_from_group" ||
        tool == "script_attach_to_node" || tool == "script_detach_from_node") {
        if (auto refused = refuseUnsavableEdit(root, node, described_path, SceneEdit::Property)) {
            return refused;
        }
        if (tool == "script_attach_to_node") {
            if (auto script_path = string_argument("script_path")) {
                auto attachable = loadAttachableScript(node, *script_path);
                if (attachable.isErr()) {
                    const auto& failure = attachable.error();
                    return errorJson(failure);
                }
            }
        }
    }
    return std::nullopt;
}


json responseOfError(const Error& error) {
    return errorJson(error);
}


std::vector<std::string> inspectorPropertyNames(GDExtensionObjectPtr object) {
    std::vector<std::string> names;
    auto properties = callObject(object, "Object", "get_property_list", 3995934104LL);
    if (properties.isErr()) return names;
    auto size_value = callVariant(properties.value(), "size");
    if (size_value.isErr()) return names;
    auto size = scalarFromVariant<int64_t>(size_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
    auto name_key = makeString("name");
    auto usage_key = makeString("usage");
    if (size.isErr() || name_key.isErr() || usage_key.isErr()) return names;
    // PROPERTY_USAGE_EDITOR, and GROUP, CATEGORY and SUBGROUP, the same on
    // every supported line.
    constexpr int64_t kEditor = 4;
    constexpr int64_t kHeadings = 64 | 128 | 256;
    for (int64_t i = 0; i < size.value(); ++i) {
        auto index = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, i);
        if (index.isErr()) break;
        auto entry = callVariant(properties.value(), "get", {&index.value()});
        if (entry.isErr()) continue;
        auto usage_value = callVariant(entry.value(), "get", {&usage_key.value()});
        if (usage_value.isErr()) continue;
        auto usage = scalarFromVariant<int64_t>(usage_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
        if (usage.isErr() || (usage.value() & kEditor) == 0 || (usage.value() & kHeadings) != 0) {
            continue;
        }
        auto name_value = callVariant(entry.value(), "get", {&name_key.value()});
        if (name_value.isErr()) continue;
        const auto type = GodotApi::instance().variant_get_type(name_value.value().ptr());
        if (type != GDEXTENSION_VARIANT_TYPE_STRING && type != GDEXTENSION_VARIANT_TYPE_STRING_NAME) {
            continue;
        }
        auto name = stringFromVariant(name_value.value(), type);
        if (name.isOk() && !name.value().empty()) names.push_back(name.value());
    }
    return names;
}


std::string joinSteps(const std::vector<std::string>& steps, size_t count) {
    std::string joined;
    for (size_t index = 0; index < count && index < steps.size(); ++index) {
        if (index > 0) joined += ':';
        joined += steps[index];
    }
    return joined;
}

// A property reached by path: the object that owns its last step, and what
// that object's class declares about it.
struct ResolvedProperty {
    GDExtensionObjectPtr holder{nullptr};
    std::string leaf;
    std::vector<std::string> steps;
    // Each resource the path entered, outermost first: resources[k] is what
    // steps[k] holds. Empty for a property of the node itself.
    std::vector<GDExtensionObjectPtr> resources;
    PropertyDescriptor descriptor;
    // Where a change to the holder is kept when the scene is saved. Always
    // the edited scene for a property of the node itself.
    ResourceHomeVerdict home;
};

// Walks a path one step at a time, each step checked against the object it is
// on. get_indexed and set_indexed answer null and do nothing for a step that
// names nothing, on every supported line, so the engine is never handed a path
// this has not resolved.
//
// A path steps only into resources. A property that holds a node would hand a
// write to a different node than target_node names, past the ownership check
// that node would get, and a value such as a Vector2 has members ClassDB does
// not describe.
Result<ResolvedProperty> resolvePropertyPath(GDExtensionObjectPtr node, const std::string& path,
                                             const std::string& edited_scene) {
    const auto parsed = parsePropertyPath(path);
    if (!parsed.problem.empty()) {
        return bridgeRefusal(400, "property_path_invalid", {{"property_name", path}}, parsed.problem);
    }
    ResolvedProperty resolved;
    resolved.steps = parsed.steps;
    GDExtensionObjectPtr holder = node;
    for (size_t index = 0; index < resolved.steps.size(); ++index) {
        const auto& step = resolved.steps[index];
        auto descriptor = findPropertyDescriptor(holder, step);
        if (descriptor.isErr()) return descriptor.error();
        if (!descriptor.value().has_value()) {
            json data = {{"property_name", path},
                         {"candidates", propertyNameCandidates(step, inspectorPropertyNames(holder), 24)}};
            // The first step keeps the sentence a missing property always had.
            if (index == 0) {
                data["code"] = "property_not_found";
                data["retryable"] = false;
                return Error(404, "Property not found on target node: " + step, std::move(data));
            }
            const auto holder_class = nodeClassName(holder);
            data["holder_class"] = holder_class;
            return bridgeRefusal(404, "property_not_found", std::move(data),
                                 "The " + (holder_class.empty() ? std::string("resource") : holder_class) +
                                     " at " + joinSteps(resolved.steps, index) + " has no property " +
                                     step + "; candidates lists the ones it does have.");
        }
        if (index + 1 == resolved.steps.size()) {
            resolved.holder = holder;
            resolved.leaf = step;
            resolved.descriptor = *descriptor.value();
            break;
        }
        const auto prefix = joinSteps(resolved.steps, index + 1);
        auto name = makeStringName(step);
        if (name.isErr()) return name.error();
        auto value = callObject(holder, "Object", "get", 2760726917LL, {&name.value()});
        if (value.isErr()) return value.error();
        const auto type = GodotApi::instance().variant_get_type(value.value().ptr());
        if (type == GDEXTENSION_VARIANT_TYPE_NIL) {
            return bridgeRefusal(404, "property_slot_empty", {{"property_name", path}, {"step", prefix}},
                                 prefix + " holds nothing, so there is nothing inside it to reach. "
                                 "Set it to a resource first.");
        }
        if (type != GDEXTENSION_VARIANT_TYPE_OBJECT) {
            return bridgeRefusal(400, "property_path_not_resource",
                                 {{"property_name", path}, {"step", prefix}},
                                 prefix + " holds a " + godotVariantTypeName(static_cast<int>(type)) +
                                     ", and a path steps only into a resource. Write the whole value "
                                     "with property_name " + prefix + ".");
        }
        auto object = objectFromVariant(value.value());
        if (object.isErr()) return object.error();
        if (!object.value()) {
            return bridgeRefusal(404, "property_slot_empty", {{"property_name", path}, {"step", prefix}},
                                 prefix + " holds nothing, so there is nothing inside it to reach. "
                                 "Set it to a resource first.");
        }
        auto is_resource = objectIsClass(object.value(), "Resource");
        if (is_resource.isErr()) return is_resource.error();
        if (!is_resource.value()) {
            return bridgeRefusal(400, "property_path_not_resource",
                                 {{"property_name", path}, {"step", prefix}},
                                 prefix + " holds a " + nodeClassName(object.value()) +
                                     ", and a path steps only into a resource. A node is named by "
                                     "target_node.");
        }
        holder = object.value();
        resolved.resources.push_back(holder);
        // The last resource the path enters is the one the write changes, so
        // its file is the one the save has to keep.
        resolved.home = resourceHome(resourcePathOf(holder), edited_scene);
    }
    return std::move(resolved);
}

// Whether an object holds one of these resources in a property of its own,
// one it stores. PROPERTY_USAGE_STORAGE is 2 and an Object-typed property is
// type 24, on every supported line.
bool holdsAnyOf(GDExtensionObjectPtr object, const std::vector<GDExtensionObjectPtr>& resources) {
    auto properties = callObject(object, "Object", "get_property_list", 3995934104LL);
    if (properties.isErr()) return false;
    auto size_value = callVariant(properties.value(), "size");
    if (size_value.isErr()) return false;
    auto size = scalarFromVariant<int64_t>(size_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
    auto name_key = makeString("name");
    auto type_key = makeString("type");
    auto usage_key = makeString("usage");
    if (size.isErr() || name_key.isErr() || type_key.isErr() || usage_key.isErr()) return false;
    constexpr int64_t kStorage = 2;
    for (int64_t i = 0; i < size.value(); ++i) {
        auto index = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, i);
        if (index.isErr()) return false;
        auto entry = callVariant(properties.value(), "get", {&index.value()});
        if (entry.isErr()) continue;
        auto type_value = callVariant(entry.value(), "get", {&type_key.value()});
        auto usage_value = callVariant(entry.value(), "get", {&usage_key.value()});
        if (type_value.isErr() || usage_value.isErr()) continue;
        auto type = scalarFromVariant<int64_t>(type_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
        auto usage = scalarFromVariant<int64_t>(usage_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
        if (type.isErr() || usage.isErr() || type.value() != GDEXTENSION_VARIANT_TYPE_OBJECT ||
            (usage.value() & kStorage) == 0) {
            continue;
        }
        auto name_value = callVariant(entry.value(), "get", {&name_key.value()});
        if (name_value.isErr()) continue;
        const auto name_type = GodotApi::instance().variant_get_type(name_value.value().ptr());
        if (name_type != GDEXTENSION_VARIANT_TYPE_STRING && name_type != GDEXTENSION_VARIANT_TYPE_STRING_NAME) {
            continue;
        }
        auto name = stringFromVariant(name_value.value(), name_type);
        if (name.isErr()) continue;
        auto property = makeStringName(name.value());
        if (property.isErr()) continue;
        auto held = callObject(object, "Object", "get", 2760726917LL, {&property.value()});
        if (held.isErr() || GodotApi::instance().variant_get_type(held.value().ptr()) != GDEXTENSION_VARIANT_TYPE_OBJECT) {
            continue;
        }
        auto held_object = objectFromVariant(held.value());
        if (held_object.isErr() || !held_object.value()) continue;
        if (std::find(resources.begin(), resources.end(), held_object.value()) != resources.end()) return true;
    }
    return false;
}

// The other nodes of the edited scene that hold, in a property of their own,
// one of the resources a path steps through (#1134). A write inside one
// changes it for every node holding it, and Node.duplicate's default flags
// share sub-resources rather than copy them, so a duplicated node shares all
// of its. Bounded by the nodes walked and the names kept; either limit sets
// shared_with_truncated. Nothing at all when the path enters no resource or
// no other node holds one.
constexpr size_t kMaxSharingWalk = 2048;
constexpr size_t kMaxSharedWith = 16;

json sharedWith(GDExtensionObjectPtr root, GDExtensionObjectPtr target,
                const std::vector<GDExtensionObjectPtr>& resources) {
    json found = json::object();
    if (resources.empty() || !root) return found;
    json names = json::array();
    bool truncated = false;
    std::vector<GDExtensionObjectPtr> pending{root};
    size_t walked = 0;
    while (!pending.empty()) {
        if (walked == kMaxSharingWalk) {
            truncated = true;
            break;
        }
        GDExtensionObjectPtr node = pending.back();
        pending.pop_back();
        ++walked;
        if (node != target && holdsAnyOf(node, resources)) {
            if (names.size() == kMaxSharedWith) {
                truncated = true;
                break;
            }
            auto path = logicalPathFromEditedRoot(root, node);
            if (path.isOk()) names.push_back(path.value());
        }
        auto children = childNodes(node);
        if (children.isErr()) continue;
        for (auto it = children.value().rbegin(); it != children.value().rend(); ++it) pending.push_back(*it);
    }
    if (!names.empty()) found["shared_with"] = std::move(names);
    if (truncated) found["shared_with_truncated"] = true;
    return found;
}

// One property's answer to a read: its value, and what its class declares
// about it. `type` is the declared type, which is what a write is checked
// against; a Variant-typed property says what it holds instead.
// PROPERTY_HINT_NODE_TYPE, 34 on 4.5.1, 4.6.2 and 4.7.2: an Object slot that
// holds a node, which is what `@export var target: Node2D` declares. Godot
// saves it as a NodePath from the node that holds it (#1292).
constexpr int kPropertyHintNodeType = 34;

bool isNodeSlot(const PropertyDescriptor& descriptor) {
    return descriptor.declared_type == GDEXTENSION_VARIANT_TYPE_OBJECT &&
           descriptor.hint == kPropertyHintNodeType;
}

// An exported Array[Node] has ARRAY_TYPE (23) and an Object element (24)
// with NODE_TYPE (34), followed by the native or script class (#1357).
std::optional<std::string> nodeArrayClass(const PropertyDescriptor& descriptor) {
    if (descriptor.declared_type != GDEXTENSION_VARIANT_TYPE_ARRAY || descriptor.hint != 23 ||
        !strings::startsWith(descriptor.hint_string, "24/34:")) return std::nullopt;
    return descriptor.hint_string.substr(6);
}

// A node slot's value as the inspector shows it and the scene file keeps it:
// the path from the node that holds the slot. Nothing when it holds no node.
// Node.get_path_to is 498846349 on all three lines.
std::optional<json> nodeSlotToJson(const ResolvedProperty& resolved, VariantValue& value) {
    if (nodeArrayClass(resolved.descriptor)) {
        if (GodotApi::instance().variant_get_type(value.ptr()) != GDEXTENSION_VARIANT_TYPE_ARRAY) {
            return std::nullopt;
        }
        auto count = callVariant(value, "size");
        if (count.isErr()) return std::nullopt;
        auto size = scalarFromVariant<int64_t>(count.value(), GDEXTENSION_VARIANT_TYPE_INT);
        if (size.isErr()) return std::nullopt;
        ResolvedProperty element;
        element.holder = resolved.holder;
        element.descriptor.declared_type = GDEXTENSION_VARIANT_TYPE_OBJECT;
        element.descriptor.hint = kPropertyHintNodeType;
        json items = json::array();
        for (int64_t i = 0; i < size.value(); ++i) {
            auto index = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, i);
            if (index.isErr()) return std::nullopt;
            auto item = callVariant(value, "get", {&index.value()});
            if (item.isErr()) return std::nullopt;
            if (auto path = nodeSlotToJson(element, item.value())) items.push_back(*path);
            else items.push_back(nullptr);
        }
        return items;
    }
    if (!isNodeSlot(resolved.descriptor)) return std::nullopt;
    if (GodotApi::instance().variant_get_type(value.ptr()) != GDEXTENSION_VARIANT_TYPE_OBJECT) {
        return std::nullopt;
    }
    auto object = objectFromVariant(value);
    if (object.isErr() || !object.value()) return std::nullopt;
    auto is_node = objectIsClass(object.value(), "Node");
    if (is_node.isErr() || !is_node.value()) return std::nullopt;
    auto use_unique_path = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, static_cast<GDExtensionBool>(0));
    if (use_unique_path.isErr()) return std::nullopt;
    auto path = callObject(resolved.holder, "Node", "get_path_to", 498846349LL,
                           {&value, &use_unique_path.value()});
    if (path.isErr()) return std::nullopt;
    auto text = stringFromVariant(path.value(), GDEXTENSION_VARIANT_TYPE_NODE_PATH);
    if (text.isErr()) return std::nullopt;
    return json(text.value());
}

// Whether a node is the class a node slot names: a native class it is or
// inherits, or a class_name its script, or a script that one extends,
// declares. Object.is_class answers for native classes only. Object.get_script
// is 1214101251, Script.get_global_name 2002593661 and Script.get_base_script
// 278624046 on all three lines.
Result<bool> nodeScriptMatches(GDExtensionObjectPtr node,
                                const std::function<Result<bool>(GDExtensionObjectPtr)>& matches) {
    auto script_value = callObject(node, "Object", "get_script", 1214101251LL);
    if (script_value.isErr()) return script_value.error();
    VariantValue held = std::move(script_value.value());
    for (int depth = 0; depth < 32; ++depth) {
        auto script = objectFromVariant(held);
        if (script.isErr() || !script.value()) return false;
        auto matched = matches(script.value());
        if (matched.isErr()) return matched.error();
        if (matched.value()) return true;
        auto base = callObject(script.value(), "Script", "get_base_script", 278624046LL);
        if (base.isErr()) return false;
        held = std::move(base.value());
    }
    return false;
}

Result<bool> nodeIsOfClass(GDExtensionObjectPtr node, const std::string& class_name) {
    auto native = objectIsClass(node, class_name.c_str());
    if (native.isErr()) return native.error();
    if (native.value()) return true;
    return nodeScriptMatches(node, [&](GDExtensionObjectPtr script) -> Result<bool> {
        auto name = callObject(script, "Script", "get_global_name", 2002593661LL);
        if (name.isErr()) return false;
        auto text = stringFromVariant(name.value(), GDEXTENSION_VARIANT_TYPE_STRING_NAME);
        return text.isOk() && text.value() == class_name;
    });
}

Result<json> describeProperty(const ResolvedProperty& resolved, const std::string& path,
                              VariantValue& value) {
    const auto held = GodotApi::instance().variant_get_type(value.ptr());
    int type = resolved.descriptor.declared_type;
    if (type == GDEXTENSION_VARIANT_TYPE_NIL) type = held;
    auto encoded = variantToJson(value);
    // A node reads as its path from the holder, the form a write takes and
    // the scene file keeps (#1292).
    if (auto slot = nodeSlotToJson(resolved, value)) encoded = *slot;
    if (encoded.isErr()) {
        return bridgeRefusal(400, "property_type_unsupported",
                             {{"property_name", path}, {"type", godotVariantTypeName(type)}},
                             path + " is a " + godotVariantTypeName(type) +
                                 ", which this tool has no JSON form for yet.");
    }
    json entry = {{"value", std::move(encoded.value())},
                  {"type", type == GDEXTENSION_VARIANT_TYPE_NIL ? std::string("Variant")
                                                                 : godotVariantTypeName(type)}};
    if (auto constraint = declaredConstraint(resolved.descriptor.hint, resolved.descriptor.hint_string)) {
        entry["engine_constraint"] = std::move(*constraint);
    }
    if (held == GDEXTENSION_VARIANT_TYPE_OBJECT) {
        auto object = objectFromVariant(value);
        if (object.isOk() && object.value()) {
            const auto held_class = nodeClassName(object.value());
            if (!held_class.empty()) entry["holds"] = held_class;
        } else if (holdsFreedObject(value)) {
            // Godot prints this value as <Freed Object>. It reads null, and
            // says why, so a caller can tell it from a slot nobody filled.
            entry["freed"] = true;
        }
    }
    if (resolved.home.home != ResourceHome::EditedScene) entry["resource_file"] = resolved.home.file;
    return std::move(entry);
}

// Everything a write needs, checked and built before anything is applied.
struct PreparedWrite {
    std::string target_path;
    std::string property;
    GDExtensionObjectPtr node{nullptr};
    ResolvedProperty resolved;
    json requested;
    VariantValue old_value;
    VariantValue new_value;
    // The steps given copies of their own by make_unique, outermost first.
    json made_unique = json::array();
    // make_unique gave this write copies, so its leaf goes into the innermost
    // copy itself rather than through the node's path (#1245).
    bool into_copy{false};
};

// The node a node slot is given: a path from the node holding the slot, or an
// absolute /root/... path, found in the edited scene and of the class the slot
// names. A path was refused as not a resource path, and Godot itself drops a
// NodePath written into the slot, or a node of another class, and leaves the
// slot empty without a word, on 4.5.1, 4.6.2 and 4.7.2 (#1292).
Result<VariantValue> makeNodeForSlot(GDExtensionObjectPtr root, GDExtensionObjectPtr holder,
                                     const std::string& property, const std::string& target_path,
                                     const std::string& slot_class, const json& value,
                                     GDExtensionObjectPtr prospective_parent = nullptr) {
    if (value.is_null()) return VariantValue{};
    std::string path;
    if (value.is_string()) {
        path = value.get<std::string>();
    } else if (value.is_object() && value.contains("type") && value["type"] == "NodePath" &&
               value.contains("value") && value["value"].is_string()) {
        path = value["value"].get<std::string>();
    }
    if (path.empty()) {
        return Error::invalidArgument(
            "Property \"" + property + "\" holds a " + slot_class +
            " node. Send its path from " + target_path +
            ", such as \"../Target\", or from the scene root, such as \"/root/Main/Target\", "
            "or null to empty the slot.");
    }
    GDExtensionObjectPtr found = nullptr;
    if (strings::startsWith(path, "/")) {
        auto resolved = resolveNode(root, path);
        if (resolved.isErr()) return resolved.error();
        found = resolved.value();
    } else {
        auto node_path = makeNodePath(path);
        if (node_path.isErr()) return node_path.error();
        // An instance is still orphaned during preflight. Walk the real
        // NodePath names, substituting its prospective parent only when a
        // '..' steps out of its root. No add_child runs a tool script early.
        if (prospective_parent) {
            auto names = callVariant(node_path.value(), "get_name_count");
            if (names.isErr()) return names.error();
            auto count = scalarFromVariant<int64_t>(names.value(), GDEXTENSION_VARIANT_TYPE_INT);
            if (count.isErr()) return count.error();
            found = holder;
            for (int64_t i = 0; i < count.value() && found; ++i) {
                auto index = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, i);
                if (index.isErr()) return index.error();
                auto name = callVariant(node_path.value(), "get_name", {&index.value()});
                if (name.isErr()) return name.error();
                auto text = stringFromVariant(name.value(), GDEXTENSION_VARIANT_TYPE_STRING_NAME);
                if (text.isErr()) return text.error();
                if (text.value() == ".") continue;
                if (text.value() == ".." && found == holder) {
                    found = prospective_parent;
                    continue;
                }
                auto part = makeNodePath(text.value());
                if (part.isErr()) return part.error();
                auto next = text.value() == ".."
                    ? callObject(found, "Node", "get_parent", 3160264692LL)
                    : callObject(found, "Node", "get_node_or_null", 2734337346LL, {&part.value()});
                if (next.isErr()) return next.error();
                auto object = objectFromVariant(next.value());
                if (object.isErr()) return object.error();
                found = object.value();
            }
        } else {
            auto result = callObject(holder, "Node", "get_node_or_null", 2734337346LL,
                                     {&node_path.value()});
            if (result.isErr()) return result.error();
            auto object = objectFromVariant(result.value());
            if (object.isErr()) return object.error();
            found = object.value();
        }
    }
    if (!found) {
        return Error::notFound("Property \"" + property + "\": there is no node at " + path +
                               " from " + target_path + ".");
    }
    bool in_instance = prospective_parent && found == holder;
    if (prospective_parent && !in_instance) {
        auto found_value = makeObject(found);
        if (found_value.isErr()) return found_value.error();
        auto beneath = callObject(holder, "Node", "is_ancestor_of", 3093956946LL, {&found_value.value()});
        if (beneath.isErr()) return beneath.error();
        auto yes = scalarFromVariant<GDExtensionBool>(beneath.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
        if (yes.isErr()) return yes.error();
        in_instance = yes.value();
    }
    if (!in_instance && relativePathWithinEditedRoot(root, found).isErr()) {
        return Error::invalidArgument("Property \"" + property + "\": " + path +
                                      " is outside the edited scene, and a scene keeps a path to "
                                      "its own nodes only.");
    }
    bool matches = strings::trim(slot_class).empty();
    for (const auto& name : strings::split(slot_class, ',')) {
        const auto wanted = strings::trim(name);
        if (wanted.empty() || matches) continue;
        auto is = nodeIsOfClass(found, wanted);
        if (is.isErr()) return is.error();
        matches = is.value();
    }
    if (!matches) {
        return Error::invalidArgument("Property \"" + property + "\" takes a " + slot_class +
                                      ", and " + path + " is a " + nodeClassName(found) +
                                      ". Godot leaves the slot empty for a node of another class.");
    }
    return makeObject(found);
}

Result<VariantValue> makeNodeArrayForSlot(GDExtensionObjectPtr root, GDExtensionObjectPtr holder,
                                          const std::string& property, const std::string& target_path,
                                          const std::string& slot_class, const json& value,
                                          VariantValue& current,
                                          GDExtensionObjectPtr prospective_parent = nullptr) {
    // An unnamed script's export hint names only its native base. The typed
    // Array still requires that exact script, or one extending it. Godot's
    // Array constructor prints an error and returns empty on a mismatch.
    auto script = callVariant(current, "get_typed_script");
    if (script.isErr()) return script.error();
    GDExtensionObjectPtr required_script = nullptr;
    if (GodotApi::instance().variant_get_type(script.value().ptr()) == GDEXTENSION_VARIANT_TYPE_OBJECT) {
        auto object = objectFromVariant(script.value());
        if (object.isErr()) return object.error();
        required_script = object.value();
    }
    return makeArrayForProperty(property, value, current,
        [&](const std::string& name, const json& item, const std::string&) -> Result<VariantValue> {
            auto built = makeNodeForSlot(root, holder, name, target_path, slot_class, item, prospective_parent);
            if (built.isErr()) return built.error();
            if (required_script && !item.is_null()) {
                auto node = objectFromVariant(built.value());
                if (node.isErr()) return node.error();
                auto matches = nodeScriptMatches(node.value(), [&](GDExtensionObjectPtr actual) -> Result<bool> {
                    return actual == required_script;
                });
                if (matches.isErr()) return matches.error();
                if (!matches.value()) {
                    return Error::invalidArgument("Property \"" + name +
                        "\" requires nodes using the array's declared script or a script that extends it.");
                }
            }
            return std::move(built.value());
        });
}

// Every check the write makes, short of making it. A dry run runs this too,
// so a preview fails where the write would.
Result<PreparedWrite> prepareWrite(GDExtensionObjectPtr root, const std::string& edited_scene,
                                   const json& item) {
    PreparedWrite write;
    write.target_path = item.value("target_node", "");
    write.property = item.value("property_name", "");
    if (write.property.empty()) return Error::invalidArgument("property_name is required");
    if (!item.contains("value")) return Error::invalidArgument("value is required");
    auto node = resolveNode(root, write.target_path);
    if (node.isErr()) return node.error();
    write.node = node.value();
    if (auto refused = refuseUnsavableEdit(root, write.node, write.target_path, SceneEdit::Property)) {
        return errorOfResponse(*refused);
    }
    // Before the path is resolved, as it always was: on 4.7 a player lists no
    // `libraries` property, so the lookup would answer 404 and hide the
    // refusal that names the tool to use.
    if (auto refused = refuseAnimationLibrarySlot(write.node,
                                                  write.property.substr(0, write.property.find(':')))) {
        return errorOfResponse(*refused);
    }
    const auto excluded = [&](const std::string& property, bool on_node) -> std::optional<Error> {
        auto reason = excludedPropertyWrite(property, on_node);
        if (!reason) return std::nullopt;
        json data = {{"property_name", write.property}};
        const auto tool = toolForExcludedWrite(property);
        if (!tool.empty()) data["use_tool"] = tool;
        return bridgeRefusal(400, "property_write_excluded", std::move(data), *reason);
    };
    // A property of the node is refused by name before it is looked up, so the
    // refusal never depends on what the lookup finds. Godot lists owner and
    // scene_file_path with no usage flags at all (0 on 4.5.1 and 4.7.2), which
    // is why the inspector never shows them and no candidate list offers them.
    if (write.property.find(':') == std::string::npos) {
        if (auto refused = excluded(write.property, true)) return *refused;
    }
    auto resolved = resolvePropertyPath(write.node, write.property, edited_scene);
    if (resolved.isErr()) return resolved.error();
    write.resolved = std::move(resolved.value());
    if (write.resolved.steps.size() > 1) {
        if (auto refused = excluded(write.resolved.leaf, false)) return *refused;
    }
    const auto& home = write.resolved.home;
    if (home.home == ResourceHome::OtherScene || home.home == ResourceHome::NotSaved) {
        const auto holder = joinSteps(write.resolved.steps, write.resolved.steps.size() - 1);
        std::string detail = holder + " on " + write.target_path + " is kept in " + home.file;
        detail += home.home == ResourceHome::OtherScene
            ? ", a scene this one instances or inherits, and a save of the edited scene keeps no "
              "change to it. Open " + home.file + " and make the change there."
            : ", which the editor does not save from a scene, so the change would last only until "
              "the editor next loads that file.";
        return bridgeRefusal(409, "subresource_not_saved",
                             {{"property_name", write.property}, {"resource_file", home.file}}, detail);
    }
    auto leaf = makeStringName(write.resolved.leaf);
    if (leaf.isErr()) return leaf.error();
    auto old_value = callObject(write.resolved.holder, "Object", "get", 2760726917LL, {&leaf.value()});
    if (old_value.isErr()) return old_value.error();
    // An empty resource slot holds nil, so the current value cannot say what
    // the slot is for. The class's own declaration can.
    auto property_type = GodotApi::instance().variant_get_type(old_value.value().ptr());
    if (property_type == GDEXTENSION_VARIANT_TYPE_NIL &&
        write.resolved.descriptor.declared_type != GDEXTENSION_VARIANT_TYPE_NIL) {
        property_type = static_cast<GDExtensionVariantType>(write.resolved.descriptor.declared_type);
    }
    const auto& value = item["value"];
    if (property_type == GDEXTENSION_VARIANT_TYPE_OBJECT && isNodeSlot(write.resolved.descriptor)) {
        auto node_value = makeNodeForSlot(root, write.resolved.holder, write.property,
                                          write.target_path, write.resolved.descriptor.hint_string, value);
        if (node_value.isErr()) return node_value.error();
        write.requested = value;
        write.old_value = std::move(old_value.value());
        write.new_value = std::move(node_value.value());
        return std::move(write);
    }
    auto compatible = validateJsonForPropertyType(write.property, value, property_type);
    if (compatible.isErr()) return compatible.error();
    auto new_value = property_type == GDEXTENSION_VARIANT_TYPE_OBJECT
        ? makeResourceForProperty(write.property, value, write.resolved.descriptor.class_name)
        : property_type == GDEXTENSION_VARIANT_TYPE_ARRAY
        ? (nodeArrayClass(write.resolved.descriptor)
            ? makeNodeArrayForSlot(root, write.resolved.holder, write.property, write.target_path,
                                   *nodeArrayClass(write.resolved.descriptor), value, old_value.value())
            : makeArrayForProperty(write.property, value, old_value.value()))
        : property_type == GDEXTENSION_VARIANT_TYPE_DICTIONARY
        ? makeDictionaryForProperty(write.property, value, old_value.value())
        : makeJsonVariantForProperty(value, property_type);
    if (new_value.isErr()) return new_value.error();
    write.requested = value;
    write.old_value = std::move(old_value.value());
    write.new_value = std::move(new_value.value());
    return std::move(write);
}

// The writes one call asks for, prepared, or the first refusal. A batch names
// the item at fault, in the message and in data.index, and points at writes,
// because that is the argument the caller changes.
constexpr size_t kMaxBatchItems = 64;

Error inBatch(Error error, const char* key, size_t index) {
    error.message += " (" + std::string(key) + "[" + std::to_string(index) + "])";
    if (!error.data.is_object()) error.data = json::object();
    error.data["index"] = index;
    error.data["field"] = key;
    return error;
}

Result<std::vector<PreparedWrite>> prepareWrites(GDExtensionObjectPtr root,
                                                 const std::string& edited_scene,
                                                 const json& arguments, bool& batch) {
    std::vector<PreparedWrite> writes;
    batch = arguments.is_object() && arguments.contains("writes");
    if (arguments.is_object() && arguments.contains("make_unique") && !arguments["make_unique"].is_boolean()) {
        return Error(400, "make_unique must be a boolean.", {{"field", "make_unique"}});
    }
    if (!batch) {
        auto write = prepareWrite(root, edited_scene, arguments);
        if (write.isErr()) return write.error();
        writes.push_back(std::move(write.value()));
        return std::move(writes);
    }
    const auto& items = arguments["writes"];
    if (arguments.contains("target_node") || arguments.contains("property_name") ||
        arguments.contains("value")) {
        return Error(400, "Send one write as target_node, property_name and value, or several as "
                          "writes, not both.", {{"field", "writes"}});
    }
    if (!items.is_array() || items.empty() || items.size() > kMaxBatchItems) {
        return Error(400, "writes must be an array of 1 to " + std::to_string(kMaxBatchItems) +
                          " objects, each with target_node, property_name and value.",
                     {{"field", "writes"}});
    }
    std::vector<BatchTarget> targets;
    for (size_t index = 0; index < items.size(); ++index) {
        const auto& item = items[index];
        if (!item.is_object() || !hasOnlyKeys(item, {"target_node", "property_name", "value"}) ||
            !item.contains("target_node") || !item["target_node"].is_string() ||
            !item.contains("property_name") || !item["property_name"].is_string()) {
            return inBatch(Error(400, "Each write is an object with exactly target_node, "
                                      "property_name and value."),
                           "writes", index);
        }
        auto write = prepareWrite(root, edited_scene, item);
        if (write.isErr()) return inBatch(write.error(), "writes", index);
        targets.push_back({std::to_string(reinterpret_cast<uintptr_t>(write.value().node)),
                           write.value().resolved.steps});
        writes.push_back(std::move(write.value()));
    }
    if (const auto overlap = overlappingWrites(targets)) {
        const auto first = std::to_string(overlap->first);
        const auto second = std::to_string(overlap->second);
        return bridgeRefusal(400, "batch_writes_overlap",
                             {{"index", overlap->second}, {"overlaps", overlap->first}, {"field", "writes"}},
                             "writes[" + first + "] and writes[" + second + "] change the same "
                             "property, or one inside the other. Send them as two calls, outer "
                             "property first.");
    }
    return std::move(writes);
}

// make_unique (#1134): every resource each write's path steps through, given to
// its node as a copy of its own, the way the inspector's Make Unique does.
// Shallow copies, linked along the path, so the write lands in copies and the
// branches it does not touch stay shared. Two writes into the same resource on
// the same node share its copy. The node takes its copy inside the write's own
// UndoRedo action, so one undo reverts both. `kept` holds every copy until the
// action holds it.
struct UniqueAssignment {
    GDExtensionObjectPtr node{nullptr};
    std::string step;
    GDExtensionObjectPtr original{nullptr};
    GDExtensionObjectPtr copy{nullptr};
};

Result<std::vector<UniqueAssignment>> makeWritesUnique(std::vector<PreparedWrite>& writes,
                                                       std::vector<VariantValue>& kept) {
    std::vector<UniqueAssignment> assignments;
    std::map<std::string, GDExtensionObjectPtr> copies;
    auto shallow = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, static_cast<GDExtensionBool>(0));
    if (shallow.isErr()) return shallow.error();
    for (auto& write : writes) {
        auto& resolved = write.resolved;
        if (resolved.resources.empty()) continue;
        const auto node_key = std::to_string(reinterpret_cast<uintptr_t>(write.node)) + "|";
        std::vector<GDExtensionObjectPtr> chain;
        for (size_t k = 0; k < resolved.resources.size(); ++k) {
            const auto prefix = joinSteps(resolved.steps, k + 1);
            const auto found = copies.find(node_key + prefix);
            GDExtensionObjectPtr copy = nullptr;
            if (found != copies.end()) {
                copy = found->second;
            } else {
                // Resource.duplicate is 482882304 on 4.5.1, 4.6.2 and 4.7.2.
                auto duplicated = callObject(resolved.resources[k], "Resource", "duplicate", 482882304LL,
                                             {&shallow.value()});
                if (duplicated.isErr()) return duplicated.error();
                auto object = objectFromVariant(duplicated.value());
                if (object.isErr() || !object.value()) {
                    return Error::internal("The engine returned no copy of " + prefix);
                }
                copy = object.value();
                kept.push_back(std::move(duplicated.value()));
                if (k == 0) {
                    assignments.push_back({write.node, resolved.steps[0], resolved.resources[0], copy});
                } else {
                    auto step = makeStringName(resolved.steps[k]);
                    auto value = makeObject(copy);
                    if (step.isErr()) return step.error();
                    if (value.isErr()) return value.error();
                    auto linked = callObject(chain.back(), "Object", "set", 3776071444LL,
                                             {&step.value(), &value.value()});
                    if (linked.isErr()) return linked.error();
                }
                copies.emplace(node_key + prefix, copy);
            }
            chain.push_back(copy);
            write.made_unique.push_back(prefix);
        }
        resolved.resources = chain;
        resolved.holder = chain.back();
        resolved.home = ResourceHomeVerdict{};
        write.into_copy = true;
    }
    return assignments;
}

// The object a write's leaf is on, reached from the node the way the engine
// reaches it: one get per step. Nothing when a step holds no object.
Result<GDExtensionObjectPtr> heldHolder(GDExtensionObjectPtr node, const std::vector<std::string>& steps) {
    GDExtensionObjectPtr current = node;
    for (size_t k = 0; k + 1 < steps.size(); ++k) {
        auto name = makeStringName(steps[k]);
        if (name.isErr()) return name.error();
        auto value = callObject(current, "Object", "get", 2760726917LL, {&name.value()});
        if (value.isErr()) return value.error();
        auto object = objectFromVariant(value.value());
        if (object.isErr() || !object.value()) return static_cast<GDExtensionObjectPtr>(nullptr);
        current = object.value();
    }
    return current;
}

// The node taking its copy, or its original back. One per node and outermost
// step, before every write on the do side and after every write on the undo.
Result<void> recordAssignment(GDExtensionObjectPtr manager, const UniqueAssignment& assignment, bool undo) {
    auto node = makeObject(assignment.node);
    auto name = makeStringName(assignment.step);
    auto value = makeObject(undo ? assignment.original : assignment.copy);
    if (node.isErr()) return node.error();
    if (name.isErr()) return name.error();
    if (value.isErr()) return value.error();
    auto recorded = callObject(manager, "EditorUndoRedoManager", undo ? "add_undo_property" : "add_do_property",
                               1017172818LL, {&node.value(), &name.value(), &value.value()});
    return recorded.isOk() ? Result<void>::ok() : Result<void>(recorded.error());
}

// Adds one prepared write to the open action. A property of the node itself is
// recorded the way it always was. A path goes through set_indexed on the node:
// measured on 4.5.1, 4.6.2 and 4.7.2, an action holding several of those, on
// more than one node, undoes and redoes as a single step, and every operation
// stays in the edited scene's history whichever file the resource it reaches
// is kept in.
Result<void> recordWrite(GDExtensionObjectPtr manager, PreparedWrite& write) {
    if (write.into_copy) {
        // Into the copy, not through the node's path. A setter can refuse the
        // copy without a word, and the path then still leads to the shared
        // original, so the write changed every node holding it while the
        // answer read the copy and called it unchanged (#1245). A copy has no
        // path for the manager to place it by, so it is told to keep the
        // action's history, the scene's, for each of these two.
        auto holder = makeObject(write.resolved.holder);
        auto name = makeStringName(write.resolved.leaf);
        if (holder.isErr()) return holder.error();
        if (name.isErr()) return name.error();
        for (const bool undo : {false, true}) {
            auto forced = callObject(manager, "EditorUndoRedoManager", "force_fixed_history", 3218959716LL);
            if (forced.isErr()) return forced.error();
            auto recorded = callObject(manager, "EditorUndoRedoManager",
                                       undo ? "add_undo_property" : "add_do_property", 1017172818LL,
                                       {&holder.value(), &name.value(),
                                        undo ? &write.old_value : &write.new_value});
            if (recorded.isErr()) return recorded.error();
        }
        return Result<void>::ok();
    }
    if (write.resolved.steps.size() == 1) {
        auto object_value = makeObject(write.node);
        if (object_value.isErr()) return object_value.error();
        auto name = makeStringName(write.resolved.leaf);
        if (name.isErr()) return name.error();
        auto done = callObject(manager, "EditorUndoRedoManager", "add_do_property", 1017172818LL,
                               {&object_value.value(), &name.value(), &write.new_value});
        if (done.isErr()) return done.error();
        auto undone = callObject(manager, "EditorUndoRedoManager", "add_undo_property", 1017172818LL,
                                 {&object_value.value(), &name.value(), &write.old_value});
        if (undone.isErr()) return undone.error();
        return Result<void>::ok();
    }
    auto path = makeNodePath(write.property);
    if (path.isErr()) return path.error();
    auto done = managerMethod(manager, "add_do_method", write.node, "set_indexed",
                              {&path.value(), &write.new_value});
    if (done.isErr()) return done.error();
    return managerMethod(manager, "add_undo_method", write.node, "set_indexed",
                         {&path.value(), &write.old_value});
}

// What a write left behind, read back from the object it changed.
//
// Report what the property now holds, not what was asked for. A commit that
// succeeds is not a property that changed: Godot discards some writes
// outright, `anchors_preset` on a Control still in layout_mode 0 being the case
// that found this, and echoing the request back reported success for a scene
// that had not moved. Same shape as vision.setCameraTransform, which returns
// observed state rather than the values it was handed.
Result<json> observeWrite(PreparedWrite& write) {
    auto leaf = makeStringName(write.resolved.leaf);
    if (leaf.isErr()) return leaf.error();
    auto observed = callObject(write.resolved.holder, "Object", "get", 2760726917LL, {&leaf.value()});
    if (observed.isErr()) return observed.error();
    auto observed_json = variantToJson(observed.value());
    if (observed_json.isErr()) return observed_json.error();
    if (auto slot = nodeSlotToJson(write.resolved, observed.value())) observed_json = *slot;
    auto old_json = variantToJson(write.old_value);
    if (old_json.isErr()) return old_json.error();
    if (auto slot = nodeSlotToJson(write.resolved, write.old_value)) old_json = *slot;
    // Against the Variant that was actually sent, not the JSON it was built
    // from. A Color written as {r,g,b} -- the spelling the instructions
    // document -- comes back with four keys, and comparing four observed
    // against three requested reported a write that landed perfectly as one
    // that did not. #638 fixed exactly this for shader_set_uniform.
    auto requested_json = variantToJson(write.new_value, 0, true);
    if (auto slot = nodeSlotToJson(write.resolved, write.new_value)) requested_json = *slot;
    const json& requested_payload = requested_json.isOk() ? requested_json.value() : write.requested;
    const bool applied = jsonValuesEquivalent(observed_json.value(), requested_payload);
    json result = {{"target_node", write.target_path}, {"property_name", write.property},
                   {"value", observed_json.value()}, {"requested_value", write.requested},
                   {"old_value", old_json.value()}, {"applied", applied}};
    // Only on the path that needs it. A write that landed says so in one key
    // and costs nothing extra, which is the whole reason this is a block
    // rather than three more keys on every response (#776).
    if (!applied) {
        result["not_applied"] = notAppliedReport(observed_json.value(), old_json.value(),
                                                 write.resolved.descriptor.hint,
                                                 write.resolved.descriptor.hint_string);
    }
    // A resource kept in its own file is rewritten when the scene is saved,
    // and every scene that uses it sees the change. Measured on all three
    // lines; the inspector does the same, and says so by showing the path.
    if (write.resolved.home.home == ResourceHome::ResourceFile) {
        result["resource_file"] = write.resolved.home.file;
    }
    if (!write.made_unique.empty()) result["made_unique"] = write.made_unique;
    return std::move(result);
}

// scene.setProperty: one write, or a batch committed as one UndoRedo action.
json setSceneProperties(GDExtensionObjectPtr editor, GDExtensionObjectPtr root, const json& params) {
    bool batch = false;
    auto prepared = prepareWrites(root, editedScenePath(root), params, batch);
    if (prepared.isErr()) return responseOfError(prepared.error());
    auto& writes = prepared.value();
    auto manager = undoManager(editor);
    if (manager.isErr()) return errorJson(manager.error());
    auto preflight = preflightUndoManagerBindings();
    if (preflight.isErr()) return errorJson(preflight.error());
    std::vector<VariantValue> kept_copies;
    std::vector<UniqueAssignment> assignments;
    if (params.value("make_unique", false)) {
        if (requireMethodBind("Resource", "duplicate", 482882304LL).isErr() ||
            requireMethodBind("Object", "set", 3776071444LL).isErr() ||
            requireMethodBind("EditorUndoRedoManager", "force_fixed_history", 3218959716LL).isErr()) {
            return bridgeError(501, "required_bind_unavailable");
        }
        auto made = makeWritesUnique(writes, kept_copies);
        if (made.isErr()) return errorJson(made.error());
        assignments = std::move(made.value());
    }
    // A batch is recorded against the scene root, which puts the action in the
    // edited scene's history whichever node comes first.
    auto action = batch
        ? createAction(manager.value(), "Didi: set " + std::to_string(writes.size()) + " properties",
                       root)
        : createAction(manager.value(), "Didi: set " + writes.front().property, writes.front().node);
    if (action.isErr()) return errorJson(action.error());
    for (const auto& assignment : assignments) {
        auto recorded = recordAssignment(manager.value(), assignment, false);
        if (recorded.isErr()) {
            abandonAction(manager.value());
            return errorJson(recorded.error());
        }
    }
    for (auto& write : writes) {
        auto recorded = recordWrite(manager.value(), write);
        if (recorded.isErr()) {
            abandonAction(manager.value());
            return errorJson(recorded.error());
        }
    }
    // Undo puts each write's old value into the copy first, then the original
    // back in place of the copy, so the node ends as it started.
    for (const auto& assignment : assignments) {
        auto recorded = recordAssignment(manager.value(), assignment, true);
        if (recorded.isErr()) {
            abandonAction(manager.value());
            return errorJson(recorded.error());
        }
    }
    auto committed = commitAction(manager.value());
    if (committed.isErr()) return errorJson(committed.error());

    json results = json::array();
    bool all_applied = true;
    for (auto& write : writes) {
        // Whether the node took its copy is read from the node, never assumed:
        // a setter that refused it leaves the node holding its original, and
        // the write went into a copy nothing holds.
        bool kept_original = false;
        if (write.into_copy) {
            auto held = heldHolder(write.node, write.resolved.steps);
            if (held.isErr()) return errorJson(held.error());
            if (held.value() != write.resolved.holder) {
                kept_original = true;
                write.resolved.holder = held.value();
            }
        }
        if (kept_original && !write.resolved.holder) {
            return bridgeError(409, "make_unique_refused",
                                 {{"property_name", write.property}, {"make_unique_refused", write.made_unique}},
                                 write.target_path + " holds nothing at " +
                                     joinSteps(write.resolved.steps, write.resolved.steps.size() - 1) +
                                     " after taking its copy, so the write cannot be read back.");
        }
        auto observed = observeWrite(write);
        if (observed.isErr()) return errorJson(observed.error());
        if (kept_original) {
            // Nothing the scene holds changed, which is what the answer says.
            auto& answer = observed.value();
            answer.erase("made_unique");
            answer["applied"] = false;
            answer["make_unique_refused"] = write.made_unique;
            answer["not_applied"] = {
                {"outcome", "unchanged"},
                {"reason", "make_unique_refused"},
                {"detail", write.target_path + " kept its own " + write.made_unique[0].get<std::string>() +
                               " when it was given a copy, so the write went into a copy nothing holds. "
                               "The resource it shares was not touched. Its setter decides what it holds; "
                               "change the resource where the setter takes it from, or without make_unique, "
                               "which changes every node that shares it."}};
            all_applied = false;
            results.push_back(std::move(answer));
            continue;
        }
        all_applied = all_applied && observed.value().value("applied", false);
        // Who else the write reached: every other node holding what it changed.
        auto shared = sharedWith(root, write.node, write.resolved.resources);
        for (auto& [key, field] : shared.items()) observed.value()[key] = std::move(field);
        results.push_back(std::move(observed.value()));
    }
    json result = {{"status", "success"}};
    if (batch) {
        result["writes"] = std::move(results);
        result["applied"] = all_applied;
    } else {
        for (auto& [key, field] : results.front().items()) result[key] = std::move(field);
    }
    result["undo_redo_registered"] = true;
    return liveSceneMutation(std::move(result));
}

// scene.getProperty: one read, or a batch. A dry run of scene_set_property
// reads its targets through here and names itself in `mutation`, so the
// write's own checks run first: everything the real call refuses on, short of
// applying it (#571).
json getSceneProperties(GDExtensionObjectPtr root, const json& params) {
    const auto edited_scene = editedScenePath(root);
    const bool batch = params.contains("reads");
    if (params.contains("mutation")) {
        const auto& mutation = params["mutation"];
        if (mutation.is_object() && mutation.value("tool", "") == "scene_set_property") {
            bool write_batch = false;
            auto prepared = prepareWrites(root, edited_scene, mutation.value("arguments", json::object()),
                                          write_batch);
            if (prepared.isErr()) return responseOfError(prepared.error());
        } else if (!batch) {
            auto node = resolveNode(root, params.value("target_node", ""));
            if (node.isErr()) return errorJson(node.error());
            if (auto refused = previewMutationPreconditions(root, node.value(),
                                                            params.value("target_node", ""), mutation)) {
                return *refused;
            }
        }
    }
    json items = json::array();
    if (batch) {
        if (params.contains("target_node") || params.contains("property_name")) {
            return errorJson(400, "Send one read as target_node and property_name, or several as "
                                  "reads, not both.", {{"field", "reads"}});
        }
        const auto& reads = params["reads"];
        if (!reads.is_array() || reads.empty() || reads.size() > kMaxBatchItems) {
            return errorJson(400, "reads must be an array of 1 to " + std::to_string(kMaxBatchItems) +
                                  " objects, each with target_node and property_name.",
                             {{"field", "reads"}});
        }
        items = reads;
    } else {
        items.push_back({{"target_node", params.value("target_node", "")},
                         {"property_name", params.value("property_name", "")}});
    }
    json answers = json::array();
    for (size_t index = 0; index < items.size(); ++index) {
        const auto& item = items[index];
        const auto fail = [&](Error error) {
            return responseOfError(batch ? inBatch(std::move(error), "reads", index) : std::move(error));
        };
        if (!item.is_object() || !hasOnlyKeys(item, {"target_node", "property_name"}) ||
            !item.contains("target_node") || !item["target_node"].is_string() ||
            !item.contains("property_name") || !item["property_name"].is_string()) {
            return fail(Error(400, "Each read is an object with exactly target_node and property_name."));
        }
        const auto target = item["target_node"].get<std::string>();
        const auto property = item["property_name"].get<std::string>();
        if (property.empty()) return fail(Error::invalidArgument("property_name is required"));
        auto node = resolveNode(root, target);
        if (node.isErr()) return fail(node.error());
        auto resolved = resolvePropertyPath(node.value(), property, edited_scene);
        if (resolved.isErr()) return fail(resolved.error());
        auto leaf = makeStringName(resolved.value().leaf);
        if (leaf.isErr()) return fail(leaf.error());
        auto value = callObject(resolved.value().holder, "Object", "get", 2760726917LL, {&leaf.value()});
        if (value.isErr()) return fail(value.error());
        auto described = describeProperty(resolved.value(), property, value.value());
        if (described.isErr()) return fail(described.error());
        json answer = {{"target_node", target}, {"property_name", property}};
        for (auto& [key, field] : described.value().items()) answer[key] = std::move(field);
        // Named, so the object items() walks outlives the loop.
        auto shared = sharedWith(root, node.value(), resolved.value().resources);
        for (auto& [key, field] : shared.items()) answer[key] = std::move(field);
        answers.push_back(std::move(answer));
    }
    json result = {{"status", "success"}};
    if (batch) {
        result["reads"] = std::move(answers);
    } else {
        for (auto& [key, field] : answers.front().items()) result[key] = std::move(field);
    }
    return liveResult(result);
}

} // namespace

namespace bridge_internal {
Result<bool> objectHasProperty(GDExtensionObjectPtr object, const std::string& property) {
    auto descriptor = findPropertyDescriptor(object, property);
    if (descriptor.isErr()) return descriptor.error();
    return descriptor.value().has_value();
}

} // namespace bridge_internal

json GodotBridge::bridgeSceneProperties(const std::string& method, const json& params, const std::string& /*session_kind*/, GDExtensionObjectPtr editor) {
    auto root = editedSceneRoot(editor);
    if (root.isErr()) return errorJson(root.error());
    return method == "scene.setProperty" ? setSceneProperties(editor, root.value(), params)
                                         : getSceneProperties(root.value(), params);
}


json GodotBridge::bridgeSceneInstantiateNode(const std::string& /*method*/, const json& params, const std::string& /*session_kind*/, GDExtensionObjectPtr editor) {
    auto root = editedSceneRoot(editor);
    if (root.isErr()) return errorJson(root.error());
    auto parent = resolveNode(root.value(), params.value("parent_path", "/root"));
    if (parent.isErr()) return errorJson(parent.error());
    if (auto refused = refuseUnsavableEdit(root.value(), parent.value(),
                                           params.value("parent_path", "/root"),
                                           SceneEdit::AddChild)) {
        return *refused;
    }
    const std::string instance_scene_path = params.value("scene_path", "");
    std::string node_type = params.value("node_type", "");
    // The tool handler refuses this too, but mutate_scene_tree with
    // action: "instantiate" forwards straight here without passing through
    // it. The mutation happens on this side, so the refusal belongs on
    // this side as well (#471).
    if (node_type.empty() && instance_scene_path.empty()) {
        return errorJson(400,
                         "Name what to instantiate: node_type for a built-in ClassDB "
                         "type, or scene_path for a res:// .tscn to instance. There is "
                         "no default, because adding a bare Node to the edited scene is "
                         "not what an empty request means.");
    }
    GDExtensionObjectPtr node = nullptr;

    if (!instance_scene_path.empty()) {
        // Putting a packed scene in the tree is close to the most common
        // single operation in Godot editing, and scene_pack_branch produced
        // scenes nothing in the surface could then consume. The instance is
        // made the way the editor makes one, so what is saved is an
        // instance of the scene rather than a copy of its nodes.
        auto valid_scene = validateResPath(instance_scene_path, ".tscn");
        if (valid_scene.isErr()) {
            return errorJson(valid_scene.error());
        }
        auto loader = singleton("ResourceLoader");
        if (loader.isErr()) return errorJson(loader.error());
        auto scene_path_value = makeString(instance_scene_path);
        auto packed_hint = makeString("PackedScene");
        auto cache_mode = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, static_cast<int64_t>(1));
        if (scene_path_value.isErr() || packed_hint.isErr() || cache_mode.isErr()) {
            return errorJson(500, "Failed to construct scene resource arguments");
        }
        auto exists_value = callObject(loader.value(), "ResourceLoader", "exists", 4185558881LL,
                                       {&scene_path_value.value(), &packed_hint.value()});
        if (exists_value.isErr()) {
            return errorJson(exists_value.error());
        }
        auto scene_exists = scalarFromVariant<GDExtensionBool>(exists_value.value(),
                                                               GDEXTENSION_VARIANT_TYPE_BOOL);
        if (scene_exists.isErr()) {
            return errorJson(scene_exists.error());
        }
        if (!scene_exists.value()) {
            return errorJson(404, "PackedScene not found: " + instance_scene_path);
        }
        if (auto refused = refuseInstanceCycle(root.value(), instance_scene_path)) return *refused;
        auto resource = callObject(loader.value(), "ResourceLoader", "load", 3358495409LL,
                                   {&scene_path_value.value(), &packed_hint.value(),
                                    &cache_mode.value()});
        if (resource.isErr()) return errorJson(resource.error());
        auto packed = objectFromVariant(resource.value());
        if (packed.isErr() || !packed.value()) {
            return errorJson(422, "Resource is not a loadable PackedScene: " + instance_scene_path,
                                 {{"code", "not_a_packed_scene"}});
        }
        auto packed_class = makeString("PackedScene");
        auto class_value = packed_class.isOk()
            ? callObject(packed.value(), "Object", "is_class", 3927539163LL,
                         {&packed_class.value()})
            : Result<VariantValue>(packed_class.error());
        auto is_packed = class_value.isOk()
            ? scalarFromVariant<GDExtensionBool>(class_value.value(),
                                                 GDEXTENSION_VARIANT_TYPE_BOOL)
            : Result<GDExtensionBool>(class_value.error());
        if (is_packed.isErr() || !is_packed.value()) {
            return is_packed.isErr()
                ? errorJson(is_packed.error())
                : errorJson(422, "Resource is not a PackedScene: " + instance_scene_path,
                                 {{"code", "not_a_packed_scene"}});
        }
        // A scene whose root script or dependency is missing answers false
        // here, and instantiating it anyway returns null and puts the reason
        // in a console the caller cannot read.
        auto can_value = callObject(packed.value(), "PackedScene", "can_instantiate",
                                    36873697LL);
        if (can_value.isErr()) return errorJson(can_value.error());
        auto can_instantiate = scalarFromVariant<GDExtensionBool>(
            can_value.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
        if (can_instantiate.isErr()) {
            return errorJson(can_instantiate.error());
        }
        if (!can_instantiate.value()) {
            return errorJson(422, "PackedScene cannot be instantiated, so something it depends "
                                  "on is missing or failed to load: " + instance_scene_path,
                             {{"code", "packed_scene_dependencies_missing"}});
        }
        // GEN_EDIT_STATE_INSTANCE, which is what the editor's own scene drop
        // uses. It is the difference between a saved instance of the scene
        // and a saved copy of the nodes that were in it.
        auto edit_state = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, static_cast<int64_t>(1));
        if (edit_state.isErr()) return errorJson(edit_state.error());
        auto instance = callObject(packed.value(), "PackedScene", "instantiate", 2628778455LL,
                                   {&edit_state.value()});
        if (instance.isErr()) return errorJson(instance.error());
        auto instance_node = objectFromVariant(instance.value());
        if (instance_node.isErr() || !instance_node.value()) {
            return errorJson(500, "Godot returned no node for PackedScene: " + instance_scene_path);
        }
        node = instance_node.value();
        // Report what was made, not what the caller happened to type in
        // node_type, which this branch does not use.
        auto instance_class = callObject(node, "Object", "get_class", 201670096LL);
        auto instance_class_name = instance_class.isOk()
            ? stringFromVariant(instance_class.value(), GDEXTENSION_VARIANT_TYPE_STRING)
            : Result<std::string>(instance_class.error());
        if (instance_class_name.isOk()) node_type = instance_class_name.value();
    } else {
        if (!engineHasClass(node_type)) {
            return errorJson(400, "Godot ClassDB could not instantiate node type: " + node_type +
                                      ". The engine has no class by that name.");
        }
        NativeName type_name(node_type);
        node = constructObject(type_name.ptr());
        if (!node) return errorJson(400, "Godot ClassDB could not instantiate node type: " + node_type);
        auto node_class = makeString("Node");
        auto is_node_variant = node_class.isOk()
            ? callObject(node, "Object", "is_class", 3927539163LL, {&node_class.value()})
            : Result<VariantValue>(node_class.error());
        auto is_node = is_node_variant.isOk()
            ? scalarFromVariant<GDExtensionBool>(is_node_variant.value(), GDEXTENSION_VARIANT_TYPE_BOOL)
            : Result<GDExtensionBool>(is_node_variant.error());
        if (is_node.isErr() || !is_node.value()) {
            GodotApi::instance().object_destroy(node);
            return is_node.isErr()
                ? errorJson(is_node.error())
                : errorJson(400, "Godot ClassDB type does not inherit Node: " + node_type);
        }
    }
    if (!params.value("name", "").empty()) {
        auto name = makeStringName(params.value("name", ""));
        auto named = name.isOk() ? callObject(node, "Node", "set_name", 3304788590LL, {&name.value()})
                                 : Result<VariantValue>(name.error());
        if (named.isErr()) {
            GodotApi::instance().object_destroy(node);
            return errorJson(named.error());
        }
    }
    json initial_properties = params.value("properties", json::object());
    if (!initial_properties.is_object()) {
        GodotApi::instance().object_destroy(node);
        return errorJson(400, "properties must be a JSON object");
    }
    // What each initial property was set to, as the engine's own Variant,
    // and what it held before, so the node can be read back once it is in
    // the tree. scene_set_property has read its write back since it
    // learned that a commit is not a change; this path never did, so a
    // player created with a bus that does not exist reported success and
    // was saved with no bus at all (vibe session nineteen).
    struct InitialProperty {
        std::string name;
        json requested;
        json written;
        json before;
        int type{0};
        int hint{0};
        std::string hint_string;
        // The engine's value, kept to set again once the node is parented.
        std::optional<VariantValue> value;
    };
    std::vector<InitialProperty> initial_written;
    for (auto it = initial_properties.begin(); it != initial_properties.end(); ++it) {
        auto has_property = objectHasProperty(node, it.key());
        if (has_property.isErr() || !has_property.value()) {
            GodotApi::instance().object_destroy(node);
            return has_property.isErr()
                ? errorJson(has_property.error())
                : errorJson(404, "Property not found on new " + node_type + " node: " + it.key());
        }
        auto property_name = makeStringName(it.key());
        if (property_name.isErr()) {
            GodotApi::instance().object_destroy(node);
            return errorJson(property_name.error());
        }
        auto current_value = callObject(node, "Object", "get", 2760726917LL, {&property_name.value()});
        if (current_value.isErr()) {
            GodotApi::instance().object_destroy(node);
            return errorJson(current_value.error());
        }
        auto declared = findPropertyDescriptor(node, it.key());
        auto property_type =
            GodotApi::instance().variant_get_type(current_value.value().ptr());
        std::string declared_class;
        if (declared.isOk() && declared.value().has_value()) {
            declared_class = declared.value()->class_name;
            if (property_type == GDEXTENSION_VARIANT_TYPE_NIL &&
                declared.value()->declared_type != GDEXTENSION_VARIANT_TYPE_NIL) {
                property_type =
                    static_cast<GDExtensionVariantType>(declared.value()->declared_type);
            }
        }
        const bool node_slot = declared.isOk() && declared.value() && isNodeSlot(*declared.value());
        const auto node_array = declared.isOk() && declared.value()
            ? nodeArrayClass(*declared.value()) : std::nullopt;
        auto compatible = node_slot ? Result<void>::ok()
                                    : validateJsonForPropertyType(it.key(), it.value(), property_type);
        if (compatible.isErr()) {
            GodotApi::instance().object_destroy(node);
            return errorJson(compatible.error());
        }
        const auto target_path = params.value("parent_path", "") + "/" + params.value("name", node_type);
        auto property_value = node_slot
            ? makeNodeForSlot(root.value(), node, it.key(), target_path, declared.value()->hint_string,
                              it.value(), parent.value())
            : property_type == GDEXTENSION_VARIANT_TYPE_OBJECT
            ? makeResourceForProperty(it.key(), it.value(), declared_class)
            : property_type == GDEXTENSION_VARIANT_TYPE_ARRAY
            ? (node_array
                ? makeNodeArrayForSlot(root.value(), node, it.key(), target_path, *node_array,
                                       it.value(), current_value.value(), parent.value())
                : makeArrayForProperty(it.key(), it.value(), current_value.value()))
            : property_type == GDEXTENSION_VARIANT_TYPE_DICTIONARY
            ? makeDictionaryForProperty(it.key(), it.value(), current_value.value())
            : makeJsonVariantForProperty(it.value(), property_type);
        if (property_value.isErr()) {
            GodotApi::instance().object_destroy(node);
            return errorJson(property_value.error());
        }
        auto set = callObject(node, "Object", "set", 3776071444LL,
                              {&property_name.value(), &property_value.value()});
        if (set.isErr()) {
            GodotApi::instance().object_destroy(node);
            return errorJson(set.error());
        }
        InitialProperty record;
        record.name = it.key();
        record.type = static_cast<int>(property_type);
        record.requested = it.value();
        // Against the Variant that was sent, not the JSON it came from, for
        // the reason scene_set_property gives: {r,g,b} comes back with four
        // keys and a Vector2 in single precision.
        auto written_json = variantToJson(property_value.value(), 0, true);
        record.written = written_json.isOk() ? written_json.value() : it.value();
        auto before_json = variantToJson(current_value.value());
        record.before = before_json.isOk() ? before_json.value() : json();
        if (declared.isOk() && declared.value().has_value()) {
            record.hint = declared.value()->hint;
            record.hint_string = declared.value()->hint_string;
        }
        record.value = std::move(property_value.value());
        initial_written.push_back(std::move(record));
    }
    auto manager = undoManager(editor);
    if (manager.isErr()) { GodotApi::instance().object_destroy(node); return errorJson(manager.error()); }
    auto child = makeObject(node);
    auto readable = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, static_cast<GDExtensionBool>(1));
    auto internal = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, static_cast<int64_t>(0));
    auto owner = makeObject(root.value());
    if (child.isErr() || readable.isErr() || internal.isErr() || owner.isErr()) {
        GodotApi::instance().object_destroy(node);
        return errorJson(500, "Failed to construct node transaction arguments");
    }
    auto logical_parent = logicalPathFromEditedRoot(root.value(), parent.value());
    if (logical_parent.isErr()) {
        GodotApi::instance().object_destroy(node);
        return errorJson(logical_parent.error());
    }
    const std::string logical_name = params.value("name", "").empty() ? node_type : params.value("name", "");
    auto preflight = preflightNodeUndoTransaction();
    if (preflight.isErr()) {
        GodotApi::instance().object_destroy(node);
        return errorJson(preflight.error());
    }
    auto action = createAction(manager.value(),
                               instance_scene_path.empty()
                                   ? "Didi: instantiate " + node_type
                                   : "Didi: instantiate " + instance_scene_path,
                               root.value());
    if (action.isErr()) { GodotApi::instance().object_destroy(node); return errorJson(action.error()); }
    auto keep = managerReference(manager.value(), "add_do_reference", node);
    auto add = managerMethod(manager.value(), "add_do_method", parent.value(), "add_child",
                             {&child.value(), &readable.value(), &internal.value()});
    auto own = managerMethod(manager.value(), "add_do_method", node, "set_owner", {&owner.value()});
    auto remove = managerMethod(manager.value(), "add_undo_method", parent.value(), "remove_child", {&child.value()});
    if (keep.isErr() || add.isErr() || own.isErr() || remove.isErr()) {
        abandonAction(manager.value());
        // add_do_reference hands the node to the undo history, which then
        // owns it. Destroying it after that would double free when the
        // history is cleared, so only clean up when the history never
        // took it. Node is not RefCounted, so nothing else will.
        if (keep.isErr()) GodotApi::instance().object_destroy(node);
        return errorJson(500, "Failed to register instantiate UndoRedo transaction");
    }
    auto committed = commitAction(manager.value());
    if (committed.isErr()) return errorJson(committed.error());
    // Godot uniquifies names on insert: a second Enemy becomes Enemy2. The
    // name was set before add_child, so the path built from it is a guess.
    // Read the real one back now that the node is actually in the tree,
    // otherwise the caller's next scene_set_property hits a sibling.
    auto actual_path = logicalPathFromEditedRoot(root.value(), node);
    json result = {{"status", "success"}, {"action", "instantiate_node"},
                   {"node_type", node_type},
                   {"node_path", actual_path.isOk()
                                     ? actual_path.value()
                                     : logical_parent.value() + "/" + logical_name},
                   {"undo_redo_registered", true}};
    // Read back now the node is in the tree, which is what the scene saves.
    // Only a property that did not land is reported, in scene_set_property's
    // words, so a call where every one landed costs nothing extra.
    json not_applied = json::array();
    for (auto& record : initial_written) {
        ResolvedProperty resolved;
        resolved.holder = node;
        resolved.descriptor.declared_type = record.type;
        resolved.descriptor.hint = record.hint;
        resolved.descriptor.hint_string = record.hint_string;
        const auto encode = [&](VariantValue& value) -> Result<json> {
            if (auto slot = nodeSlotToJson(resolved, value)) return *slot;
            return variantToJson(value);
        };
        if (record.value && (isNodeSlot(resolved.descriptor) || nodeArrayClass(resolved.descriptor))) {
            auto written = encode(*record.value);
            if (written.isOk()) record.written = written.value();
        }
        auto name = makeStringName(record.name);
        if (name.isErr()) continue;
        auto observed = callObject(node, "Object", "get", 2760726917LL, {&name.value()});
        if (observed.isErr()) continue;
        auto observed_json = encode(observed.value());
        if (observed_json.isErr() || jsonValuesEquivalent(observed_json.value(), record.written)) {
            continue;
        }
        // A property whose effect depends on the parent, such as a Control's
        // anchors_preset, is discarded while the node has none: the same write
        // one call later landed (#1297). Set again now that it is in the tree.
        // Undo removes the node and redo adds this same object back, so the
        // value needs no step of its own in the history.
        if (record.value) {
            auto again = callObject(node, "Object", "set", 3776071444LL, {&name.value(), &*record.value});
            if (again.isOk()) {
                auto reread = callObject(node, "Object", "get", 2760726917LL, {&name.value()});
                auto reread_json = reread.isOk() ? encode(reread.value()) : Result<json>(reread.error());
                if (reread_json.isOk() && jsonValuesEquivalent(reread_json.value(), record.written)) continue;
                if (reread_json.isOk()) observed_json = std::move(reread_json);
            }
        }
        json entry = notAppliedReport(observed_json.value(), record.before, record.hint,
                                      record.hint_string);
        entry["property_name"] = record.name;
        entry["requested_value"] = record.requested;
        entry["value"] = observed_json.value();
        not_applied.push_back(std::move(entry));
    }
    if (!not_applied.empty()) result["properties_not_applied"] = std::move(not_applied);
    // Say when the engine did not use the name it was given.
    //
    // Godot forbids . : @ / % and " in a node name and substitutes rather
    // than refusing, and it uniquifies a duplicate. Reporting the real path
    // is the important half and was already right; what was missing was any
    // field saying a substitution happened, so an agent that named a node
    // after a class_name, a filename or a JSON key then built its next
    // NodePath from the name it chose, and the 404 that followed had its
    // reason four responses back (#710). An empty or absent name asks the
    // engine to name the node, so it is not a substitution.
    const std::string requested_name = params.value("name", "");
    if (actual_path.isOk() && !requested_name.empty()) {
        const auto slash = actual_path.value().find_last_of('/');
        const std::string engine_name = slash == std::string::npos
                                            ? actual_path.value()
                                            : actual_path.value().substr(slash + 1);
        result["node_name"] = engine_name;
        if (engine_name != requested_name) {
            result["requested_name"] = requested_name;
            result["name_substituted"] = true;
        }
    }
    // Which scene this is an instance of, so the caller can tell an
    // instance apart from a node of the same class that merely looks like
    // one in the tree.
    if (!instance_scene_path.empty()) result["scene_path"] = instance_scene_path;
    if (actual_path.isErr()) result["node_path_verified"] = false;
    return liveSceneMutation(result);
}

} // namespace didi::godot
