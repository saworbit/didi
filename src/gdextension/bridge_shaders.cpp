#include "bridge_internal.hpp"
#include "didi/gdextension/godot_bridge.hpp"
#include "didi/gdextension/godot_object.hpp"
#include "didi/gdextension/runtime_bridge.hpp"
#include <algorithm>
#include <cmath>
#include <sstream>
#include <tuple>
#include <utility>

namespace didi::godot {
using namespace bridge_internal;
namespace {
// What a shader declares a uniform to be when nothing overrides it.
//
// The default lives in the rendering server rather than on the Shader resource,
// so this needs the shader's RID and a renderer behind it. A session running
// without one answers nil, and the caller is told nothing rather than told
// something invented.

Result<VariantValue> shaderParameterDefault(GDExtensionObjectPtr shader, VariantValue& uniform_name) {
    for (const auto& bind : {std::make_tuple("Resource", "get_rid", 2944877500LL),
                             std::make_tuple("RenderingServer", "shader_get_parameter_default",
                                             2621281810LL)}) {
        auto required = requireMethodBind(std::get<0>(bind), std::get<1>(bind), std::get<2>(bind));
        if (required.isErr()) return Error(501, required.error().message);
    }
    auto server = singleton("RenderingServer");
    if (server.isErr()) return server.error();
    auto rid = callObject(shader, "Resource", "get_rid", 2944877500LL);
    if (rid.isErr()) return rid.error();
    return callObject(server.value(), "RenderingServer", "shader_get_parameter_default",
                      2621281810LL, {&rid.value(), &uniform_name});
}


// Whether a shader's code compiles, asked of the engine (#1345). A shader that
// failed to compile lists no uniforms, and so does one that compiled with none
// the engine lists: a VisualShader's graph textures and any sampler with a
// default texture are left out of the list, and a uniform the preprocessor
// turns off is never compiled. Reading the code for a uniform line refused all
// three. A copy of the code with one uniform added tells them apart, since the
// engine lists that uniform only when the whole code compiles, and the
// compiler prints its lines during the call, so they land in the answer's
// engine_diagnostics. No code is nothing to compile. Shader.set_code is
// 83702148 on 4.5.1, 4.6.2 and 4.7.2.
Result<bool> shaderCodeCompiles(GDExtensionObjectPtr shader, const std::string& shader_path) {
    auto code_value = callObject(shader, "Shader", "get_code", 201670096LL);
    if (code_value.isErr()) return code_value.error();
    auto code = stringFromVariant(code_value.value(), GDEXTENSION_VARIANT_TYPE_STRING);
    if (code.isErr()) return code.error();
    if (strings::trim(code.value()).empty()) return true;
    if (auto bind = requireMethodBind("Shader", "set_code", 83702148LL); bind.isErr()) return bind.error();

    const auto probe = shaderCompileProbe(code.value(), shader_path);
    NativeName shader_class("Shader");
    auto copy = constructObject(shader_class.ptr());
    if (!copy) return Error(501, "Godot ClassDB could not construct a Shader");
    // The Variant holds the copy's only reference and frees it on the way out.
    auto held = makeObject(copy);
    if (held.isErr()) {
        GodotApi::instance().object_destroy(copy);
        return held.error();
    }
    auto text = makeString(probe.code);
    if (text.isErr()) return text.error();
    auto applied = callObject(copy, "Shader", "set_code", 83702148LL, {&text.value()});
    if (applied.isErr()) return applied.error();
    auto groups = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, static_cast<GDExtensionBool>(0));
    if (groups.isErr()) return groups.error();
    auto uniforms = callObject(copy, "Shader", "get_shader_uniform_list", 1230511656LL, {&groups.value()});
    if (uniforms.isErr()) return uniforms.error();
    auto size_value = callVariant(uniforms.value(), "size");
    if (size_value.isErr()) return size_value.error();
    auto count = scalarFromVariant<int64_t>(size_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
    if (count.isErr()) return count.error();
    auto name_key = makeString("name");
    if (name_key.isErr()) return name_key.error();
    for (int64_t index = 0; index < count.value(); ++index) {
        auto index_value = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, index);
        if (index_value.isErr()) return index_value.error();
        auto entry = callVariant(uniforms.value(), "get", {&index_value.value()});
        if (entry.isErr()) return entry.error();
        auto name_value = callVariant(entry.value(), "get", {&name_key.value()});
        if (name_value.isErr()) continue;
        const auto name_type = GodotApi::instance().variant_get_type(name_value.value().ptr());
        if (name_type != GDEXTENSION_VARIANT_TYPE_STRING && name_type != GDEXTENSION_VARIANT_TYPE_STRING_NAME) {
            continue;
        }
        auto name = stringFromVariant(name_value.value(), name_type);
        if (name.isOk() && name.value() == probe.uniform) return true;
    }
    return false;
}

} // namespace

json GodotBridge::bridgeShaders(const std::string& method, const json& params, const std::string& /*session_kind*/, GDExtensionObjectPtr editor) {

    // The uniform PropertyInfo the engine hands back carries the hint a shader
    // author wrote, and the bridge read only name and type out of it, so a
    // caller had no way to learn a declared range short of reading the shader
    // source -- which is the thing these tools exist to avoid (#620).
    //
    // PROPERTY_HINT_RANGE is 1 and PROPERTY_HINT_ENUM is 2 on 4.5.1, 4.6.2 and
    // 4.7.2. Any other hint is reported by number rather than guessed at.
    const auto readUniformHint = [](VariantValue& entry, VariantValue& hint_key,
                                    VariantValue& hint_string_key) -> json {
        auto hint_value = callVariant(entry, "get", {&hint_key});
        if (hint_value.isErr()) return json(nullptr);
        auto hint = scalarFromVariant<int64_t>(hint_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
        if (hint.isErr() || hint.value() == 0) return json(nullptr);

        std::string hint_string;
        auto text_value = callVariant(entry, "get", {&hint_string_key});
        if (text_value.isOk()) {
            const auto text_type = GodotApi::instance().variant_get_type(text_value.value().ptr());
            if (text_type == GDEXTENSION_VARIANT_TYPE_STRING ||
                text_type == GDEXTENSION_VARIANT_TYPE_STRING_NAME) {
                auto text = stringFromVariant(text_value.value(), text_type);
                if (text.isOk()) hint_string = text.value();
            }
        }

        if (hint.value() == 1) {
            const auto range = parseShaderHintRange(hint_string);
            if (!range.has_value()) return json(nullptr);
            return json{{"kind", "range"},
                        {"minimum", range->minimum},
                        {"maximum", range->maximum},
                        {"step", range->step.has_value() ? json(*range->step) : json(nullptr)},
                        {"or_greater", range->or_greater},
                        {"or_less", range->or_less},
                        {"hint_string", hint_string}};
        }
        if (hint.value() == 2) {
            json options = json::array();
            std::string current;
            for (const char character : hint_string) {
                if (character == ',') {
                    options.push_back(current);
                    current.clear();
                    continue;
                }
                current.push_back(character);
            }
            if (!current.empty() || !options.empty()) options.push_back(current);
            return json{{"kind", "enum"}, {"options", options}, {"hint_string", hint_string}};
        }
        return json{{"kind", "other"}, {"hint", hint.value()}, {"hint_string", hint_string}};
    };
    // One path to the material for all three, so a slot one of them accepts
    // is never a slot another refuses.
    const bool setting = method == "shader.setUniform";
    const bool graphing = method == "shader.getVisualGraph";
    const bool shape_ok = setting
        ? (hasOnlyKeys(params, {"target_node", "property_name", "uniform_name", "value"}) &&
           params.contains("uniform_name") && params["uniform_name"].is_string() &&
           params.contains("value"))
        : hasOnlyKeys(params, {"target_node", "property_name"});
    if (!shape_ok || !params.contains("target_node") || !params["target_node"].is_string() ||
        !params.contains("property_name") || !params["property_name"].is_string()) {
        return bridgeError(400, setting ? "invalid_shader_set_uniform_request"
                                        : (graphing ? "invalid_shader_get_visual_graph_request"
                                                    : "invalid_shader_list_uniforms_request"));
    }
    if (graphing) {
        for (const auto& bind : {
                 std::make_tuple("VisualShader", "get_node_list", 2370592410LL),
                 std::make_tuple("VisualShader", "get_node", 3784670312LL),
                 std::make_tuple("VisualShader", "get_node_position", 2175036082LL),
                 std::make_tuple("VisualShader", "get_node_connections", 1441964831LL)}) {
            if (requireMethodBind(std::get<0>(bind), std::get<1>(bind), std::get<2>(bind)).isErr()) {
                return bridgeError(501, "required_bind_unavailable");
            }
        }
    }
    for (const auto& bind : {
             std::make_tuple("ShaderMaterial", "get_shader", 2078273437LL),
             std::make_tuple("Shader", "get_shader_uniform_list", 1230511656LL),
             std::make_tuple("Shader", "get_mode", 3392948163LL),
             std::make_tuple("ShaderMaterial", "get_shader_parameter", 2760726917LL)}) {
        if (requireMethodBind(std::get<0>(bind), std::get<1>(bind), std::get<2>(bind)).isErr()) {
            return bridgeError(501, "required_bind_unavailable");
        }
    }
    if (setting && preflightUndoManagerBindings().isErr()) {
        return bridgeError(501, "required_bind_unavailable");
    }
    auto root = editedSceneRoot(editor);
    if (root.isErr()) return errorJson(root.error());
    const auto target_path = params["target_node"].get<std::string>();
    const auto property = params["property_name"].get<std::string>();
    auto node = resolveNode(root.value(), target_path);
    // The resolver already says which path it could not find. The
    // identifier said neither what was looked for nor what happened (#424).
    if (node.isErr()) return errorJson(404, node.error().message);

    auto has_property = objectHasProperty(node.value(), property);
    if (has_property.isErr()) return errorJson(has_property.error());
    if (!has_property.value()) {
        return errorJson(404, "Property not found on target node: " + property);
    }
    auto property_name = makeStringName(property);
    if (property_name.isErr()) return errorJson(500, property_name.error().message);
    auto slot = callObject(node.value(), "Object", "get", 2760726917LL, {&property_name.value()});
    if (slot.isErr()) return errorJson(500, slot.error().message);
    auto material = objectFromVariant(slot.value());
    if (material.isErr()) return errorJson(500, material.error().message);
    if (!material.value()) {
        return errorJson(404, "Property \"" + property + "\" on " + target_path +
                                  " holds nothing; there is no material to read");
    }
    auto is_shader_material = objectIsClass(material.value(), "ShaderMaterial");
    if (is_shader_material.isErr()) return errorJson(500, is_shader_material.error().message);
    if (!is_shader_material.value()) {
        // A StandardMaterial3D has properties, not shader uniforms. Reading
        // it here and reporting an empty uniform list would look like a
        // shader with nothing to set.
        auto actual = callObject(material.value(), "Object", "get_class", 201670096LL);
        std::string actual_name = "an unrecognised type";
        if (actual.isOk()) {
            auto text = stringFromVariant(actual.value(), GDEXTENSION_VARIANT_TYPE_STRING);
            if (text.isOk()) actual_name = text.value();
        }
        return errorJson(409, "Property \"" + property + "\" on " + target_path + " holds " +
                                  actual_name + " and not a ShaderMaterial",
                         {{"code", "not_a_shader_material"}});
    }

    auto shader_value = callObject(material.value(), "ShaderMaterial", "get_shader", 2078273437LL);
    if (shader_value.isErr()) return errorJson(500, shader_value.error().message);
    auto shader = objectFromVariant(shader_value.value());
    if (shader.isErr()) return errorJson(500, shader.error().message);
    if (!shader.value()) {
        return errorJson(409, "The ShaderMaterial on " + target_path + " has no shader assigned",
                         {{"code", "no_shader_assigned"}});
    }

    json result = {{"target_node", target_path}, {"property_name", property}};
    auto shader_path_name = makeStringName("resource_path");
    if (shader_path_name.isOk()) {
        auto shader_path = callObject(shader.value(), "Object", "get", 2760726917LL,
                                      {&shader_path_name.value()});
        if (shader_path.isOk()) {
            auto text = stringFromVariant(shader_path.value(), GDEXTENSION_VARIANT_TYPE_STRING);
            result["shader_path"] = (text.isOk() && !text.value().empty())
                                        ? json(text.value()) : json(nullptr);
        }
    }
    auto mode = callObject(shader.value(), "Shader", "get_mode", 3392948163LL);
    if (mode.isOk()) {
        auto mode_value = scalarFromVariant<int64_t>(mode.value(), GDEXTENSION_VARIANT_TYPE_INT);
        if (mode_value.isOk()) {
            // 0 spatial, 1 canvas_item, 2 particles, 3 sky, 4 fog. Named so
            // a caller is not handed a bare number to look up.
            static const char* kModes[] = {"spatial", "canvas_item", "particles", "sky", "fog"};
            const auto index = mode_value.value();
            result["shader_mode"] = (index >= 0 && index < 5) ? json(kModes[index])
                                                              : json(nullptr);
        }
    }

    if (graphing) {
        auto is_visual = objectIsClass(shader.value(), "VisualShader");
        if (is_visual.isErr()) return errorJson(500, is_visual.error().message);
        if (!is_visual.value()) {
            // A hand written .gdshader has code and no graph. Returning an
            // empty node list would read as a graph with nothing in it.
            return errorJson(409, "The shader on this material is written in code, not built as "
                                  "a VisualShader graph, so it has no nodes to report",
                             {{"code", "not_a_visual_shader"}});
        }
        // The shader types a VisualShader can hold, in enum order. Named so
        // a caller is not handed a bare number, and skipped entirely when a
        // type holds nothing.
        static const char* kGraphTypes[] = {"vertex", "fragment", "light", "start", "process",
                                            "collide", "start_custom", "process_custom", "sky",
                                            "fog"};
        constexpr int64_t kMaxGraphNodes = 256;
        constexpr int64_t kMaxGraphConnections = 512;
        json types = json::array();
        bool any_truncated = false;
        for (int64_t type_index = 0; type_index < 10; ++type_index) {
            auto type_value = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, type_index);
            if (type_value.isErr()) return errorJson(500, type_value.error().message);
            auto ids = callObject(shader.value(), "VisualShader", "get_node_list",
                                  2370592410LL, {&type_value.value()});
            if (ids.isErr()) continue;
            auto id_count_value = callVariant(ids.value(), "size");
            if (id_count_value.isErr()) continue;
            auto id_count = scalarFromVariant<int64_t>(id_count_value.value(),
                                                       GDEXTENSION_VARIANT_TYPE_INT);
            if (id_count.isErr() || id_count.value() == 0) continue;

            json nodes = json::array();
            const int64_t reported_nodes = std::min<int64_t>(id_count.value(), kMaxGraphNodes);
            for (int64_t slot = 0; slot < reported_nodes; ++slot) {
                auto slot_value = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, slot);
                if (slot_value.isErr()) return errorJson(500, slot_value.error().message);
                auto id_value = callVariant(ids.value(), "get", {&slot_value.value()});
                if (id_value.isErr()) continue;
                auto node_id = scalarFromVariant<int64_t>(id_value.value(),
                                                          GDEXTENSION_VARIANT_TYPE_INT);
                if (node_id.isErr()) continue;
                auto graph_id = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, node_id.value());
                if (graph_id.isErr()) return errorJson(500, graph_id.error().message);

                json node_entry = {{"id", node_id.value()}};
                auto node_object_value = callObject(shader.value(), "VisualShader", "get_node",
                                                    3784670312LL,
                                                    {&type_value.value(), &graph_id.value()});
                if (node_object_value.isOk()) {
                    auto node_object = objectFromVariant(node_object_value.value());
                    if (node_object.isOk() && node_object.value()) {
                        auto class_name = callObject(node_object.value(), "Object", "get_class",
                                                     201670096LL);
                        if (class_name.isOk()) {
                            auto text = stringFromVariant(class_name.value(),
                                                          GDEXTENSION_VARIANT_TYPE_STRING);
                            if (text.isOk()) {
                                node_entry["class"] = boundUtf8(text.value(), 256).value;
                            }
                        }
                    }
                }
                auto position = callObject(shader.value(), "VisualShader", "get_node_position",
                                           2175036082LL, {&type_value.value(), &graph_id.value()});
                if (position.isOk()) {
                    auto point = pointVariantToJson(position.value(), 2);
                    if (point.isOk()) node_entry["position"] = point.value();
                }
                nodes.push_back(std::move(node_entry));
            }

            json links = json::array();
            int64_t connection_count = 0;
            auto connections = callObject(shader.value(), "VisualShader", "get_node_connections",
                                          1441964831LL, {&type_value.value()});
            if (connections.isOk()) {
                auto connection_size = callVariant(connections.value(), "size");
                if (connection_size.isOk()) {
                    auto total = scalarFromVariant<int64_t>(connection_size.value(),
                                                             GDEXTENSION_VARIANT_TYPE_INT);
                    if (total.isOk()) {
                        connection_count = total.value();
                        const int64_t reported =
                            std::min<int64_t>(connection_count, kMaxGraphConnections);
                        for (int64_t link = 0; link < reported; ++link) {
                            auto link_index = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, link);
                            if (link_index.isErr()) break;
                            auto entry = callVariant(connections.value(), "get",
                                                      {&link_index.value()});
                            if (entry.isErr()) continue;
                            json link_entry = json::object();
                            for (const auto* field : {"from_node", "from_port", "to_node",
                                                      "to_port"}) {
                                auto field_key = makeString(field);
                                if (field_key.isErr()) continue;
                                auto field_value = callVariant(entry.value(), "get",
                                                                {&field_key.value()});
                                if (field_value.isErr()) continue;
                                auto number = scalarFromVariant<int64_t>(
                                    field_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
                                if (number.isOk()) link_entry[field] = number.value();
                            }
                            if (!link_entry.empty()) links.push_back(std::move(link_entry));
                        }
                    }
                }
            }

            const bool truncated = id_count.value() > kMaxGraphNodes ||
                                   connection_count > kMaxGraphConnections;
            any_truncated = any_truncated || truncated;
            types.push_back({{"type", kGraphTypes[type_index]},
                             {"nodes", std::move(nodes)},
                             {"node_count", id_count.value()},
                             {"connections", std::move(links)},
                             {"connection_count", connection_count},
                             {"truncated", truncated}});
        }
        result["shader_types"] = std::move(types);
        result["truncated"] = any_truncated;
        return liveResult(result);
    }

    auto groups = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, static_cast<GDExtensionBool>(0));
    if (groups.isErr()) return errorJson(500, groups.error().message);
    auto code_value = callObject(shader.value(), "Shader", "get_code", 201670096LL);
    if (code_value.isErr()) return errorJson(500, code_value.error().message);
    auto code = stringFromVariant(code_value.value(), GDEXTENSION_VARIANT_TYPE_STRING);
    if (code.isErr()) return errorJson(500, code.error().message);
    // Shader.new() has no rendering-server shader until code is assigned.
    // Asking it for uniforms prints the engine's null-shader error (#1358).
    // An empty code string declares none, for reads and writes alike.
    auto uniforms = code.value().empty()
                        ? makeJsonVariant(json::array())
                        : callObject(shader.value(), "Shader", "get_shader_uniform_list", 1230511656LL,
                                     {&groups.value()});
    if (uniforms.isErr()) return errorJson(500, uniforms.error().message);
    auto size_value = callVariant(uniforms.value(), "size");
    if (size_value.isErr()) return errorJson(500, size_value.error().message);
    auto count = scalarFromVariant<int64_t>(size_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
    if (count.isErr()) return errorJson(500, count.error().message);
    // A shader that failed to compile lists no uniforms, and an empty list
    // read as "this shader has none" (#1300). The engine is asked whether the
    // code compiles.
    if (count.value() == 0) {
        const auto path = result.value("shader_path", json(nullptr));
        auto compiles = shaderCodeCompiles(shader.value(), path.is_string() ? path.get<std::string>() : "");
        if (compiles.isOk() && !compiles.value()) {
            return errorJson(422,
                             "The shader's code does not compile, so the engine lists none of its "
                             "uniforms. engine_diagnostics carries the compiler's message; "
                             "shader_check_compile names the line.",
                             {{"code", "shader_not_compiled"},
                              {"target_node", target_path},
                              {"property_name", property},
                              {"shader_path", result.value("shader_path", json(nullptr))},
                              {"retryable", false}});
        }
    }

    if (setting) {
        const auto requested_name = params["uniform_name"].get<std::string>();
        // The shader list decides whether this uniform exists.
        // set_shader_parameter accepts any name and does nothing with one
        // the shader never declared, so a typo would otherwise come back as
        // a write that worked.
        auto name_key = makeString("name");
        auto type_key = makeString("type");
        auto class_key = makeString("class_name");
        auto hint_key = makeString("hint");
        auto hint_string_key = makeString("hint_string");
        if (name_key.isErr() || type_key.isErr() || class_key.isErr() || hint_key.isErr() ||
            hint_string_key.isErr()) {
            return errorJson(500, "Failed to build uniform keys");
        }
        bool declared = false;
        int64_t declared_type = 0;
        std::string declared_class;
        json declared_hint = json(nullptr);
        for (int64_t index = 0; index < count.value() && !declared; ++index) {
            auto index_value = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, index);
            if (index_value.isErr()) return errorJson(500, index_value.error().message);
            auto entry = callVariant(uniforms.value(), "get", {&index_value.value()});
            if (entry.isErr()) return errorJson(500, entry.error().message);
            auto name_value = callVariant(entry.value(), "get", {&name_key.value()});
            if (name_value.isErr()) return errorJson(500, name_value.error().message);
            const auto name_type = GodotApi::instance().variant_get_type(name_value.value().ptr());
            if (name_type != GDEXTENSION_VARIANT_TYPE_STRING &&
                name_type != GDEXTENSION_VARIANT_TYPE_STRING_NAME) {
                continue;
            }
            auto name = stringFromVariant(name_value.value(), name_type);
            if (name.isErr()) return errorJson(500, name.error().message);
            if (name.value() != requested_name) continue;
            declared = true;
            auto type_value = callVariant(entry.value(), "get", {&type_key.value()});
            if (type_value.isOk()) {
                auto value = scalarFromVariant<int64_t>(type_value.value(),
                                                        GDEXTENSION_VARIANT_TYPE_INT);
                if (value.isOk()) declared_type = value.value();
            }
            auto class_value = callVariant(entry.value(), "get", {&class_key.value()});
            if (class_value.isOk()) {
                const auto class_type =
                    GodotApi::instance().variant_get_type(class_value.value().ptr());
                if (class_type == GDEXTENSION_VARIANT_TYPE_STRING ||
                    class_type == GDEXTENSION_VARIANT_TYPE_STRING_NAME) {
                    auto text = stringFromVariant(class_value.value(), class_type);
                    if (text.isOk()) declared_class = text.value();
                }
            }
            declared_hint = readUniformHint(entry.value(), hint_key.value(),
                                            hint_string_key.value());
        }
        if (!declared) {
            return errorJson(404, "The shader declares no uniform named " + requested_name);
        }

        // The same contract scene_set_property applies, so a caller learns
        // one set of JSON spellings rather than two.
        const auto uniform_type = static_cast<GDExtensionVariantType>(declared_type);
        auto compatible = validateJsonForPropertyType(requested_name, params["value"], uniform_type);
        if (compatible.isErr()) return errorJson(compatible.error());

        // A value outside a declared hint_range is usually a slipped digit
        // or a confusion between a normalised and an absolute scale, and
        // this server already treats that class as worth refusing by name:
        // audio.configureBus bounds volume_db for exactly this reason. The
        // range a shader author wrote down is the same kind of statement.
        // or_greater and or_less say the author meant a slider bound rather
        // than a limit, and those are honoured.
        if (declared_hint.is_object() && declared_hint.value("kind", "") == "range" &&
            params["value"].is_number()) {
            const double sent = params["value"].get<double>();
            const double minimum = declared_hint.value("minimum", 0.0);
            const double maximum = declared_hint.value("maximum", 0.0);
            const bool under = sent < minimum && !declared_hint.value("or_less", false);
            const bool over = sent > maximum && !declared_hint.value("or_greater", false);
            if (under || over) {
                std::ostringstream refusal;
                refusal << "Uniform \"" << requested_name << "\" declares hint_range("
                        << minimum << ", " << maximum << "); " << sent
                        << " is outside it. Send a value from " << minimum << " to " << maximum
                        << ".";
                return errorJson(400, refusal.str());
            }
        }
        auto new_value = uniform_type == GDEXTENSION_VARIANT_TYPE_OBJECT
            ? makeResourceForProperty(requested_name, params["value"], declared_class)
            : makeJsonVariantForProperty(params["value"], uniform_type);
        if (new_value.isErr()) return errorJson(new_value.error());

        auto uniform_name = makeStringName(requested_name);
        if (uniform_name.isErr()) return errorJson(500, uniform_name.error().message);
        auto old_value = callObject(material.value(), "ShaderMaterial", "get_shader_parameter",
                                    2760726917LL, {&uniform_name.value()});
        if (old_value.isErr()) return errorJson(500, old_value.error().message);

        // Undo goes through the shader_parameter/<name> property on the
        // material, which is the one the scene file writes and the one the
        // inspector edits, so undoing this is the undo a person expects.
        auto stored_name = makeStringName("shader_parameter/" + requested_name);
        if (stored_name.isErr()) return errorJson(500, stored_name.error().message);
        auto manager = undoManager(editor);
        if (manager.isErr()) return errorJson(manager.error());
        auto material_value = makeObject(material.value());
        if (material_value.isErr()) return errorJson(500, material_value.error().message);
        auto action = createAction(manager.value(), "Didi: set shader uniform " + requested_name,
                                   material.value());
        if (action.isErr()) return errorJson(500, action.error().message);
        auto do_property = callObject(manager.value(), "EditorUndoRedoManager", "add_do_property",
                                      1017172818LL,
                                      {&material_value.value(), &stored_name.value(),
                                       &new_value.value()});
        auto undo_property = callObject(manager.value(), "EditorUndoRedoManager", "add_undo_property",
                                        1017172818LL,
                                        {&material_value.value(), &stored_name.value(),
                                         &old_value.value()});
        if (do_property.isErr() || undo_property.isErr()) {
            // Closed, so the next mutation does not merge into it (#1152).
            abandonAction(manager.value());
            return errorJson(500, (do_property.isErr() ? do_property : undo_property).error().message);
        }
        auto committed = commitAction(manager.value());
        if (committed.isErr()) return errorJson(committed.error());

        // Report what it now holds and not what was asked for, the same way
        // scene_set_property does and for the same reason.
        auto observed = callObject(material.value(), "ShaderMaterial", "get_shader_parameter",
                                   2760726917LL, {&uniform_name.value()});
        if (observed.isErr()) return errorJson(500, observed.error().message);
        auto observed_json = variantToJson(observed.value(), 0, true);
        auto old_json = variantToJson(old_value.value(), 0, true);
        json old_payload = old_json.isOk() ? old_json.value() : json(nullptr);
        if (isNilVariant(old_value.value())) {
            // The undo entry above restores nil on purpose, which takes the
            // override off again rather than pinning the default in its
            // place. What gets reported is the value that was in effect,
            // and for a uniform the material did not set that is the
            // shader's declared default, read the same way the list reads
            // it.
            auto fallback = shaderParameterDefault(shader.value(), uniform_name.value());
            if (fallback.isOk() && !isNilVariant(fallback.value())) {
                auto encoded = variantToJson(fallback.value(), 0, true);
                if (encoded.isOk()) old_payload = encoded.value();
            }
        }
        json observed_payload = observed_json.isOk() ? observed_json.value() : json(nullptr);
        // What was asked for, encoded the way it was actually sent to the
        // engine. A colour may be spelled {r,g,b} with the alpha left off,
        // which this tool's own schema documents, and comparing the
        // observed four keys against the requested three reported every
        // such write as one that did not land (#618).
        auto requested_json = variantToJson(new_value.value(), 0, true);
        const json& requested_payload =
            requested_json.isOk() ? requested_json.value() : params["value"];
        // The same disclosure every other open-scene mutator carries. The
        // change is in the edited scene and its undo history, and is
        // discarded the same way if the editor closes without saving, so
        // the absence of scene_saved read as "this one did not need
        // saving" (#623).
        return liveSceneMutation({{"status", "success"},
                                  {"target_node", target_path},
                                  {"property_name", property},
                                  {"uniform_name", requested_name},
                                  {"type", godotVariantTypeName(static_cast<int>(declared_type))},
                                  {"hint", declared_hint},
                                  {"value", observed_payload},
                                  {"requested_value", params["value"]},
                                  {"old_value", old_payload},
                                  {"applied", jsonValuesEquivalent(observed_payload, requested_payload)},
                                  {"undo_redo_registered", true}});
    }

    constexpr int64_t kMaxUniforms = 256;
    const int64_t reported = std::min<int64_t>(count.value(), kMaxUniforms);
    auto name_key = makeString("name");
    auto type_key = makeString("type");
    auto hint_key = makeString("hint");
    auto hint_string_key = makeString("hint_string");
    if (name_key.isErr() || type_key.isErr() || hint_key.isErr() || hint_string_key.isErr()) {
        return errorJson(500, "Failed to build uniform keys");
    }

    json listed = json::array();
    for (int64_t index = 0; index < reported; ++index) {
        auto index_value = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, index);
        if (index_value.isErr()) return errorJson(500, index_value.error().message);
        auto entry = callVariant(uniforms.value(), "get", {&index_value.value()});
        if (entry.isErr()) return errorJson(500, entry.error().message);
        auto name_value = callVariant(entry.value(), "get", {&name_key.value()});
        if (name_value.isErr()) return errorJson(500, name_value.error().message);
        const auto name_type = GodotApi::instance().variant_get_type(name_value.value().ptr());
        if (name_type != GDEXTENSION_VARIANT_TYPE_STRING &&
            name_type != GDEXTENSION_VARIANT_TYPE_STRING_NAME) {
            continue;
        }
        auto name = stringFromVariant(name_value.value(), name_type);
        if (name.isErr()) return errorJson(500, name.error().message);

        int64_t declared_type = 0;
        auto type_value = callVariant(entry.value(), "get", {&type_key.value()});
        if (type_value.isOk()) {
            auto declared = scalarFromVariant<int64_t>(type_value.value(),
                                                       GDEXTENSION_VARIANT_TYPE_INT);
            if (declared.isOk()) declared_type = declared.value();
        }

        json uniform = {{"name", name.value()},
                        {"type", godotVariantTypeName(static_cast<int>(declared_type))},
                        {"hint", readUniformHint(entry.value(), hint_key.value(),
                                                 hint_string_key.value())},
                        {"settable", jsonTypeIsInsidePropertyContract(static_cast<int>(declared_type))}};

        auto uniform_name = makeStringName(name.value());
        if (uniform_name.isErr()) return errorJson(500, uniform_name.error().message);
        auto current = callObject(material.value(), "ShaderMaterial", "get_shader_parameter",
                                  2760726917LL, {&uniform_name.value()});
        if (current.isErr()) return errorJson(500, current.error().message);
        // The effective value: the material's override where it has one and
        // the shader's own declared default otherwise.
        //
        // get_shader_parameter alone cannot supply that. In a running game
        // on 4.5.1, 4.6.2 and 4.7.2 it answers nil for a uniform the
        // material does not set, so the declared default was reported as no
        // value at all. The shader keeps its defaults in the rendering
        // server, and that is where they have to be read from.
        //
        // There is still deliberately no flag separating an override from a
        // default. The same call that answers nil in a game answers with the
        // default in a 4.7.2 editor, so nil is not evidence of anything and
        // a flag built on it would be wrong in one context or the other.
        if (isNilVariant(current.value())) {
            auto fallback = shaderParameterDefault(shader.value(), uniform_name.value());
            if (fallback.isOk()) current = std::move(fallback.value());
        }
        // Lenient: a uniform of a type this cannot encode is reported by
        // name and type with a null value, rather than failing the whole
        // read and leaving the caller with nothing.
        auto encoded = variantToJson(current.value(), 0, true);
        // A null value here is not a uniform without one. It is a value
        // neither the material nor the rendering server could supply, which
        // is what a session with no renderer looks like.
        uniform["value"] = encoded.isOk() ? encoded.value() : json(nullptr);
        listed.push_back(std::move(uniform));
    }

    result["uniforms"] = std::move(listed);
    result["uniform_count"] = count.value();
    result["truncated"] = count.value() > kMaxUniforms;
    return liveResult(result);
}

} // namespace didi::godot
