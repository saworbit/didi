#include "didi/offline/speculative_verify.hpp"
#include "didi/mcp/mcp_protocol.hpp"
#include "didi/common/ipc_channel.hpp"
#include "didi/common/logger.hpp"
#include "didi/offline/resource_indexer.hpp"
#include "didi/offline/project_audit.hpp"
#include "didi/offline/project_impact.hpp"
#include "didi/offline/audio_bus_layout.hpp"
#include "didi/offline/class_reference.hpp"
#include "didi/common/project_path.hpp"
#include "didi/common/atomic_write.hpp"
#include "didi/common/engine_version.hpp"
#include "didi/runtime/session_client.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <map>
#include <sstream>
#include <set>
#include <string>
#include <vector>

namespace didi {
namespace mcp {

CallToolResult handleQueryProjectResources(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    (void)ipc;
    std::string search_path = args.value("search_path", "res://");
    std::string type_filter = args.value("type_filter", "");
    std::string fuzzy_query = args.value("fuzzy_query", "");
    bool include_uid = args.value("include_uid", true);

    const auto indexer = offline::ResourceIndexer::sharedIndex(".");
    auto results = indexer->query(search_path, type_filter, fuzzy_query);

    json res_arr = json::array();
    for (const auto& item : results) {
        json j = item.toJson();
        if (!include_uid) j.erase("uid");
        res_arr.push_back(j);
    }

    json out = {
        {"total_found", results.size()},
        {"resources", res_arr}
    };
    if (indexer->truncated()) out["truncated"] = true;
    // A file whose name is not valid UTF-8 is not in the list and cannot be,
    // because no JSON response can carry it. Said rather than silently omitted:
    // before this the serialisation threw and the call answered that an
    // argument had the wrong type, on a call that carried none (#650).
    if (indexer->undecodablePathCount() > 0) {
        out["undecodable_paths"] = indexer->undecodablePaths();
        out["undecodable_path_count"] = indexer->undecodablePathCount();
    }
    return CallToolResult::successJson(out);
}

namespace {

bool hasNumericKeys(const json& value, std::initializer_list<const char*> keys) {
    for (const auto* key : keys) {
        if (!value.contains(key) || !value[key].is_number()) return false;
    }
    return true;
}

std::string escapeTresString(const std::string& text) {
    std::string out;
    for (char c : text) {
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else out += c;
    }
    return out;
}

// The typed constructors a caller can ask for by name.
//
// A .tres property has a type and the JSON does not. `tile_size` on a TileSet
// is a Vector2i, and two plain numbers under x and y were written as
// Vector2(32, 32) because the literal was chosen from the shape of the JSON
// rather than from the property. Naming the type is the only way the caller
// can say which one they mean, so it is spelled out rather than guessed.
struct ComponentType {
    const char* name;
    const char* const* members;
    size_t member_count;
    bool integral;
};

const char* const kXY[] = {"x", "y"};
const char* const kXYZ[] = {"x", "y", "z"};
const char* const kXYZW[] = {"x", "y", "z", "w"};
const char* const kRGBA[] = {"r", "g", "b", "a"};

const ComponentType kComponentTypes[] = {
    {"Vector2", kXY, 2, false},      {"Vector2i", kXY, 2, true},
    {"Vector3", kXYZ, 3, false},     {"Vector3i", kXYZ, 3, true},
    {"Vector4", kXYZW, 4, false},    {"Vector4i", kXYZW, 4, true},
    {"Quaternion", kXYZW, 4, false}, {"Color", kRGBA, 4, false},
};

// The packed arrays. The scalar ones are written element by element by the same
// writer as anything else; the composite ones are written flat, see
// packedElementType below.
const char* const kPackedArrayTypes[] = {
    "PackedByteArray",    "PackedInt32Array",   "PackedInt64Array",
    "PackedFloat32Array", "PackedFloat64Array", "PackedStringArray",
    "PackedVector2Array", "PackedVector3Array", "PackedVector4Array",
    "PackedColorArray",
};

// The types whose payload is a single string.
const char* const kStringTypes[] = {"NodePath", "StringName"};

bool namesType(const std::string& value, const char* const* names, size_t count) {
    for (size_t index = 0; index < count; ++index) {
        if (value == names[index]) return true;
    }
    return false;
}

const ComponentType* findComponentType(const std::string& name) {
    for (const auto& entry : kComponentTypes) {
        if (name == entry.name) return &entry;
    }
    return nullptr;
}

// The element type of a packed array whose element is itself composite, and
// null for the ones whose element is a scalar.
//
// Godot writes these flat -- `PackedVector2Array(0, 0, 512, 0)`, one number per
// component -- and its text parser refuses a nested constructor with
// "Expected float in constructor", which fails the whole resource rather than
// the one property (#765). Confirmed against 4.5.1: ResourceSaver on a
// NavigationPolygon and a Gradient writes the flat form, and var_to_str agrees
// for all four.
const ComponentType* packedElementType(const std::string& name) {
    if (name == "PackedVector2Array") return findComponentType("Vector2");
    if (name == "PackedVector3Array") return findComponentType("Vector3");
    if (name == "PackedVector4Array") return findComponentType("Vector4");
    if (name == "PackedColorArray") return findComponentType("Color");
    return nullptr;
}

// Whether this object is asking for a named type rather than being a plain
// Dictionary. Godot type names begin with a capital, so a dictionary whose
// "type" is an ordinary word is still a dictionary; a capitalised name this
// writer does not know is refused rather than quietly written as a Dictionary,
// because a misspelled PackedFloat32Array is the failure this whole change is
// about.
bool asksForANamedType(const json& value) {
    const auto type = value.find("type");
    if (type == value.end() || !type->is_string()) return false;
    const auto name = type->get<std::string>();
    return !name.empty() && name[0] >= 'A' && name[0] <= 'Z';
}

// What a .tres needs at the top of the file for the references its body makes.
//
// A property that points at another resource is not a value: it is a name that
// only means anything because a header entry declares it. So the writer cannot
// render a property in isolation any more. It renders into a collector, which
// accumulates the [ext_resource] lines to emit and knows which [sub_resource]
// ids are legal to name at this point in the file.
struct ExternalReference {
    std::string path;
    std::string type;
    std::string uid;
    std::string id;
};

struct ReferenceScope {
    std::vector<ExternalReference> externals;
    // The sub-resource ids already declared above the current point in the
    // file. Godot's parser resolves SubResource against what it has already
    // read, so naming one declared further down yields a null rather than an
    // error, which is the kind of silent wrong write this writer exists to
    // refuse.
    std::vector<std::string> visible_sub_ids;
};

// The JSON kind, for a refusal to name what it was given.
const char* jsonKindOf(const json& value) {
    if (value.is_string()) return "a string";
    if (value.is_boolean()) return "a boolean";
    if (value.is_number()) return "a number";
    if (value.is_array()) return "an array";
    if (value.is_object()) return "an object";
    return "that";
}

// What a slot of this declared type takes, or empty for a type this writer
// does not rule on.
std::string wantedFor(const std::string& declared_type) {
    if (declared_type == "int") return "a whole number";
    if (declared_type == "float") return "a number";
    if (declared_type == "bool") return "true or false";
    if (declared_type == "String" || namesType(declared_type, kStringTypes,
                                               std::size(kStringTypes))) {
        return "a string";
    }
    if (declared_type == "Color") {
        return "an object with \"r\", \"g\" and \"b\", or a colour string such as \"#ff8800\"";
    }
    if (const auto* component = findComponentType(declared_type)) {
        std::string wanted = "an object with ";
        for (size_t index = 0; index < component->member_count; ++index) {
            if (index) wanted += index + 1 == component->member_count ? " and " : ", ";
            wanted += "\"" + std::string(component->members[index]) + "\"";
        }
        return wanted;
    }
    if (declared_type == "Array" || declared_type.rfind("typedarray::", 0) == 0 ||
        namesType(declared_type, kPackedArrayTypes, std::size(kPackedArrayTypes))) {
        return "an array";
    }
    return {};
}

// Whether a value of this JSON kind survives being written into a slot the
// class reference declares as `declared_type`.
//
// Godot converts what it can on load and silently keeps the property's default
// for the rest, so a value it cannot convert produces a file that reports one
// thing and holds another (#764). Measured on 4.5.1: `radius = "big"` loads as
// 0.0, `size = 7` as (0, 0), `shadow_offset = "over there"` as (0, 0) and
// `radius = Vector2(1, 2)` as 0.0, none of them with an error the caller sees.
//
// Objects are accepted wherever a constructor or a named type is a legitimate
// spelling, and the #730 guard below decides those. The three types with no
// object spelling at all are the exception.
//
// A declared type not listed here -- a Transform3D, a Dictionary, a Resource
// slot, an enum -- is left alone. Refusing on a rule that was never checked is
// its own wrong answer.
bool declaredTypeAccepts(const std::string& declared_type, const json& value) {
    // A whole number is a whole number however it was spelled. JSON does not
    // separate 4 from 4.0 and plenty of clients serialise every number as a
    // double, so the value decides and not the notation. int is the most
    // common declared type on the surface, and refusing 4.0 there would break
    // far more callers than it caught.
    if (declared_type == "int") {
        if (value.is_number_integer()) return true;
        if (!value.is_number_float()) return false;
        const double number = value.get<double>();
        return number == std::floor(number) && std::isfinite(number);
    }
    if (declared_type == "float") return value.is_number();
    if (declared_type == "bool") return value.is_boolean();
    if (declared_type == "String" ||
        namesType(declared_type, kStringTypes, std::size(kStringTypes))) {
        return value.is_string() || asksForANamedType(value);
    }
    // Godot's own String -> Color conversion, which is the "#rrggbbaa"
    // spelling scene_set_property documents for the same kind of slot.
    if (declared_type == "Color") return value.is_string() || value.is_object();
    if (findComponentType(declared_type)) return value.is_object();
    if (declared_type == "Array" || declared_type.rfind("typedarray::", 0) == 0 ||
        namesType(declared_type, kPackedArrayTypes, std::size(kPackedArrayTypes))) {
        return value.is_array() || asksForANamedType(value);
    }
    return true;
}

// `declared_type` is what the class reference says this property holds, when it
// carries the type. It is passed at the top level of a property only: the shape
// of the JSON decides everything nested inside an array or a dictionary, as it
// always has.
Result<std::string> tresLiteral(const json& value, const std::string& property,
                                ReferenceScope* scope = nullptr,
                                const std::string* declared_type = nullptr);

// The components of one composite value, comma separated and nothing else.
//
// Separate from the literal because a packed array of these is written as one
// flat run of components with no per-element constructor around them, which is
// what Godot's own saver writes and the only form its parser accepts (#765).
Result<std::string> tresComponentComponents(const ComponentType& type, const json& value,
                                            const std::string& property) {
    std::ostringstream out;
    for (size_t index = 0; index < type.member_count; ++index) {
        const auto member = type.members[index];
        const auto found = value.find(member);
        if (found == value.end() || !found->is_number()) {
            return Error::invalidArgument("Property \"" + property + "\" declares type " +
                                          type.name + ", which needs a number under \"" + member +
                                          "\"");
        }
        if (type.integral && !found->is_number_integer()) {
            return Error::invalidArgument("Property \"" + property + "\" declares type " +
                                          type.name + ", whose \"" + member +
                                          "\" must be a whole number");
        }
        if (index) out << ", ";
        out << found->dump();
    }
    return out.str();
}

Result<std::string> tresComponentLiteral(const ComponentType& type, const json& value,
                                         const std::string& property) {
    // Color takes an alpha, and leaving it out is a common and harmless thing
    // to do, so it defaults rather than being demanded.
    auto components = tresComponentComponents(type, value, property);
    if (components.isErr()) return components.error();
    return type.name + ("(" + components.value() + ")");
}

// Whether the project holds this path, and what Godot calls it.
//
// An [ext_resource] carries the type and the uid of what it points at, so a
// reference to a file that is not there cannot be written correctly and is not
// written at all. That refusal is the point: the alternative is a .tres that
// loads with a null where the texture should be.
Result<ExternalReference> resolveExternalReference(const json& value, const std::string& property) {
    const auto path_value = value.find("path");
    if (path_value == value.end() || !path_value->is_string() ||
        path_value->get<std::string>().empty()) {
        return Error::invalidArgument("Property \"" + property +
                                      "\" declares type ExtResource, which needs the resource it "
                                      "points at under \"path\"");
    }
    const auto path = path_value->get<std::string>();
    if (path.rfind("res://", 0) != 0) {
        return Error::invalidArgument("Property \"" + property +
                                      "\" declares type ExtResource with path \"" + path +
                                      "\", which must be a res:// path inside this project");
    }
    ExternalReference reference;
    reference.path = path;
    const auto indexer = offline::ResourceIndexer::sharedIndex(".");
    if (const auto* found = indexer->findExact(path)) {
        reference.type = found->type;
        reference.uid = found->uid;
    } else {
        return Error::notFound("Property \"" + property +
                               "\" declares type ExtResource pointing at " + path +
                               ", which is not in this project. A .tres naming a file that is "
                               "not there loads with nothing in that slot.");
    }
    // The caller wins where they say so: the index guesses a type from the file
    // extension, and a custom Resource script is the case it cannot guess.
    const auto declared = value.find("resource_type");
    if (declared != value.end()) {
        if (!declared->is_string() || declared->get<std::string>().empty()) {
            return Error::invalidArgument("Property \"" + property +
                                          "\" declares an ExtResource whose resource_type must be "
                                          "a non-empty string");
        }
        reference.type = declared->get<std::string>();
    }
    if (reference.type.empty()) {
        return Error::invalidArgument(
            "Property \"" + property + "\" declares an ExtResource pointing at " + path +
            ", whose resource type could not be determined. Pass resource_type to name it.");
    }
    return reference;
}

Result<std::string> tresNamedTypeLiteral(const json& value, const std::string& property,
                                         ReferenceScope* scope) {
    const auto name = value.at("type").get<std::string>();
    if (const auto* component = findComponentType(name)) {
        if (name == "Color" && !value.contains("a")) {
            json with_alpha = value;
            with_alpha["a"] = 1.0;
            return tresComponentLiteral(*component, with_alpha, property);
        }
        return tresComponentLiteral(*component, value, property);
    }
    if (namesType(name, kStringTypes, std::size(kStringTypes))) {
        const auto text = value.find("value");
        if (text == value.end() || !text->is_string()) {
            return Error::invalidArgument("Property \"" + property + "\" declares type " + name +
                                          ", which needs its text under \"value\"");
        }
        return name + "(\"" + escapeTresString(text->get<std::string>()) + "\")";
    }
    if (namesType(name, kPackedArrayTypes, std::size(kPackedArrayTypes))) {
        const auto items = value.find("values");
        if (items == value.end() || !items->is_array()) {
            return Error::invalidArgument("Property \"" + property + "\" declares type " + name +
                                          ", which needs its elements under \"values\"");
        }
        std::ostringstream out;
        out << name << "(";
        bool first = true;
        // The composite packed arrays are one flat run of components. Both
        // spellings reach the same file: an element per entry, which is the
        // shape the rest of this writer documents, or the components already
        // flattened, which is what the file looks like and what a caller who
        // copied one out of a .tres will send. Mixing them is refused, because
        // neither reading of the mixture is the one they meant.
        if (const auto* element_type = packedElementType(name)) {
            bool flattened = false;
            bool per_element = false;
            for (const auto& element : *items) {
                if (element.is_number()) {
                    flattened = true;
                } else if (element.is_object()) {
                    per_element = true;
                } else {
                    return Error::invalidArgument(
                        "Property \"" + property + "\" declares type " + name +
                        ", whose elements are each a " + element_type->name +
                        " object or a run of plain numbers");
                }
            }
            if (flattened && per_element) {
                return Error::invalidArgument(
                    "Property \"" + property + "\" declares type " + name +
                    ", and its \"values\" mixes " + element_type->name +
                    " objects with plain numbers. Send one or the other.");
            }
            if (flattened && items->size() % element_type->member_count != 0) {
                return Error::invalidArgument(
                    "Property \"" + property + "\" declares type " + name + " and was given " +
                    std::to_string(items->size()) + " numbers, which is not a whole number of " +
                    element_type->name + "s. Godot drops the trailing part-element on load.");
            }
            for (const auto& element : *items) {
                if (!first) out << ", ";
                first = false;
                if (element.is_number()) {
                    out << element.dump();
                    continue;
                }
                // A named type inside the array has to be the array's own, or
                // the caller is describing a file Godot cannot hold.
                if (asksForANamedType(element)) {
                    const auto named = element["type"].get<std::string>();
                    if (named != element_type->name) {
                        return Error::invalidArgument(
                            "Property \"" + property + "\" declares type " + name +
                            ", whose elements are " + element_type->name + ", and one of them is "
                            "a " + named);
                    }
                }
                json element_value = element;
                if (std::strcmp(element_type->name, "Color") == 0 && !element_value.contains("a")) {
                    element_value["a"] = 1.0;
                }
                auto components = tresComponentComponents(*element_type, element_value, property);
                if (components.isErr()) return components.error();
                out << components.value();
            }
            out << ")";
            return out.str();
        }
        for (const auto& element : *items) {
            auto rendered = tresLiteral(element, property, scope);
            if (rendered.isErr()) return rendered.error();
            if (!first) out << ", ";
            first = false;
            out << rendered.value();
        }
        out << ")";
        return out.str();
    }
    if (name == "ExtResource" || name == "SubResource") {
        if (!scope) {
            return Error::invalidArgument(
                "Property \"" + property + "\" declares type " + name +
                ", which is only meaningful inside a resource this writer is assembling.");
        }
        if (name == "ExtResource") {
            auto reference = resolveExternalReference(value, property);
            if (reference.isErr()) return reference.error();
            // One header entry per path, however many properties name it.
            for (const auto& existing : scope->externals) {
                if (existing.path == reference.value().path) return "ExtResource(\"" + existing.id + "\")";
            }
            reference.value().id = std::to_string(scope->externals.size() + 1) + "_didi";
            const auto id = reference.value().id;
            scope->externals.push_back(std::move(reference.value()));
            return "ExtResource(\"" + id + "\")";
        }
        const auto id_value = value.find("id");
        if (id_value == value.end() || !id_value->is_string() ||
            id_value->get<std::string>().empty()) {
            return Error::invalidArgument("Property \"" + property +
                                          "\" declares type SubResource, which needs the id of a "
                                          "sub_resources entry under \"id\"");
        }
        const auto id = id_value->get<std::string>();
        if (std::find(scope->visible_sub_ids.begin(), scope->visible_sub_ids.end(), id) ==
            scope->visible_sub_ids.end()) {
            return Error::invalidArgument(
                "Property \"" + property + "\" names sub-resource \"" + id +
                "\", which is not declared above it. Add it to sub_resources, and put it before "
                "anything that references it: Godot resolves SubResource against what it has "
                "already read, so a later one reads as null.");
        }
        return "SubResource(\"" + id + "\")";
    }
    return Error::invalidArgument(
        "Property \"" + property + "\" declares type " + name +
        ", which resource_create cannot write. It writes the vector and colour "
        "types, NodePath, StringName, the packed arrays, and the ExtResource and "
        "SubResource references.");
}

// Renders one value as the Godot text-resource literal it stands for, or says
// what it could not render and which property it was under.
//
// It used to fall through to JSON for anything it did not recognise, which
// produced a file that loads, reports a plausible resource, and has thrown the
// value away, with the engine's complaints going to a console nobody is
// reading. A refusal costs the caller one call. A silent wrong write costs
// them the time they spend debugging the animation instead of the file.
Result<std::string> tresLiteral(const json& value, const std::string& property,
                                ReferenceScope* scope, const std::string* declared_type) {
    // The declared type decides before the shape of the JSON does. A value the
    // slot cannot hold used to be written verbatim, reported in
    // properties_written with property_check checked: true, and then dropped by
    // Godot on load, so every tool in the chain reported success and the
    // resource held its default (#764).
    if (declared_type && !value.is_null() && !declaredTypeAccepts(*declared_type, value)) {
        if (*declared_type == "int" && value.is_number()) {
            return Error::invalidArgument(
                "Property \"" + property + "\" is declared int by " +
                offline::ClassReference::instance().apiVersion() +
                ", and Godot truncates a fraction written into it on load. Send a whole number.");
        }
        return Error::invalidArgument(
            "Property \"" + property + "\" is declared " + *declared_type + " by " +
            offline::ClassReference::instance().apiVersion() + ", and " + jsonKindOf(value) +
            " written into it is dropped when Godot loads the file. Send " +
            wantedFor(*declared_type) + ".");
    }
    const ComponentType* declared = declared_type ? findComponentType(*declared_type) : nullptr;
    if (value.is_string()) return "\"" + escapeTresString(value.get<std::string>()) + "\"";
    if (value.is_boolean()) return std::string(value.get<bool>() ? "true" : "false");
    if (value.is_number()) return value.dump();
    if (value.is_null()) return std::string("null");
    if (value.is_array()) {
        std::ostringstream out;
        out << "[";
        bool first = true;
        for (const auto& element : value) {
            auto rendered = tresLiteral(element, property, scope);
            if (rendered.isErr()) return rendered.error();
            if (!first) out << ", ";
            first = false;
            out << rendered.value();
        }
        out << "]";
        return out.str();
    }
    if (!value.is_object()) {
        return Error::invalidArgument("Property \"" + property +
                                      "\" holds a value resource_create cannot write");
    }
    if (asksForANamedType(value)) {
        // A type the caller named that the property does not have is the same
        // silent loss as a property the type does not declare: Godot drops a
        // Vector2 written into a Vector2i slot on load, so the file the caller
        // was told about is not the file they get (#730).
        const auto named = value["type"].is_string() ? value["type"].get<std::string>()
                                                     : std::string();
        if (declared && !named.empty() && named != declared->name) {
            return Error::invalidArgument(
                "Property \"" + property + "\" is declared " + declared->name + " by " +
                offline::ClassReference::instance().apiVersion() + ", and a " + named +
                " written into it is dropped when Godot loads the file. Send it as " +
                declared->name + ", or leave the type out and it will be written as one.");
        }
        return tresNamedTypeLiteral(value, property, scope);
    }

    // The property's declared type beats the shape of the JSON. `{x, y}` used
    // to become Vector2(..) wherever it appeared, including in the Vector2i
    // slots a TileSet's tile_size and an atlas source's texture_region_size
    // are, and Godot drops the wrong one on load (#730). The components are
    // checked against the declared type here, so a fractional value in an
    // integer vector is refused rather than truncated.
    if (declared && value.is_object() && !value.empty()) {
        if (std::strcmp(declared->name, "Color") == 0 && !value.contains("a")) {
            json with_alpha = value;
            with_alpha["a"] = 1.0;
            return tresComponentLiteral(*declared, with_alpha, property);
        }
        return tresComponentLiteral(*declared, value, property);
    }

    // No named type and nothing declared, so the shape decides, as it always has.
    if (value.size() == 4 && hasNumericKeys(value, {"r", "g", "b", "a"})) {
        return "Color(" + value["r"].dump() + ", " + value["g"].dump() + ", " +
               value["b"].dump() + ", " + value["a"].dump() + ")";
    }
    if (value.size() == 3 && hasNumericKeys(value, {"r", "g", "b"})) {
        return "Color(" + value["r"].dump() + ", " + value["g"].dump() + ", " +
               value["b"].dump() + ", 1)";
    }
    if (hasNumericKeys(value, {"x", "y", "z", "w"})) {
        return "Vector4(" + value["x"].dump() + ", " + value["y"].dump() + ", " +
               value["z"].dump() + ", " + value["w"].dump() + ")";
    }
    if (hasNumericKeys(value, {"x", "y", "z"})) {
        return "Vector3(" + value["x"].dump() + ", " + value["y"].dump() + ", " +
               value["z"].dump() + ")";
    }
    if (hasNumericKeys(value, {"x", "y"})) {
        return "Vector2(" + value["x"].dump() + ", " + value["y"].dump() + ")";
    }

    std::ostringstream out;
    out << "{";
    bool first = true;
    for (auto entry = value.begin(); entry != value.end(); ++entry) {
        auto rendered = tresLiteral(entry.value(), property, scope);
        if (rendered.isErr()) return rendered.error();
        if (!first) out << ", ";
        first = false;
        out << "\"" << escapeTresString(entry.key()) << "\": " << rendered.value();
    }
    out << "}";
    return out.str();
}

// The properties in the order they will be written to the file.
//
// A .tres is order sensitive. Godot applies indexed sub-properties in file
// order and `tracks/0/type` is what creates track 0, so emitting
// `tracks/0/interp` first lands every other field of that track on a track
// that does not exist yet. A JSON object cannot carry an order: nlohmann sorts
// its keys and there is nothing left to preserve by the time the request
// reaches here. An array of {name, value} entries can, and is written exactly
// as it arrives.
// Whether a property name has a numbered element in it, as tracks/0/type does.
//
// This is the one shape sorting is guaranteed to break. `tracks/0/type` is what
// creates track 0 and it sorts after interp, keys and path, so an object always
// applies the other three to a track that does not exist yet. A name without a
// number, such as shader_parameter/albedo, still sorts after the property that
// has to precede it, so it is left alone.
bool hasNumberedPathSegment(const std::string& name) {
    size_t start = 0;
    while (start <= name.size()) {
        const auto end = name.find('/', start);
        const auto length = end == std::string::npos ? name.size() - start : end - start;
        if (length > 0 && start > 0) {
            bool digits = true;
            for (size_t index = start; index < start + length; ++index) {
                if (name[index] < '0' || name[index] > '9') {
                    digits = false;
                    break;
                }
            }
            if (digits) return true;
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return false;
}

Result<std::vector<std::pair<std::string, json>>> orderedProperties(const json& properties) {
    std::vector<std::pair<std::string, json>> ordered;
    if (properties.is_object()) {
        for (auto entry = properties.begin(); entry != properties.end(); ++entry) {
            if (hasNumberedPathSegment(entry.key())) {
                return Error::invalidArgument(
                    "Property \"" + entry.key() +
                    "\" names a numbered element, and Godot applies those in file order while a "
                    "JSON object can only be written sorted. Pass properties as an array of "
                    "{name, value} entries so the order is yours.");
            }
            ordered.emplace_back(entry.key(), entry.value());
        }
        return ordered;
    }
    if (!properties.is_array()) {
        return Error::invalidArgument(
            "properties must be an object, or an array of {name, value} entries when the order "
            "they are written in matters");
    }
    std::set<std::string> seen;
    for (const auto& entry : properties) {
        if (!entry.is_object() || !entry.contains("name") || !entry["name"].is_string() ||
            !entry.contains("value")) {
            return Error::invalidArgument(
                "Each properties entry must be an object with a string name and a value");
        }
        const auto name = entry["name"].get<std::string>();
        if (name.empty()) {
            return Error::invalidArgument("A properties entry has an empty name");
        }
        if (!seen.insert(name).second) {
            return Error::invalidArgument("Property \"" + name +
                                          "\" is named more than once, and only one of the two "
                                          "would reach the file");
        }
        ordered.emplace_back(name, entry["value"]);
    }
    return ordered;
}

} // namespace

// The [sub_resource] blocks a resource carries inside itself.
//
// An array rather than an object, for the same reason properties are: the order
// is load-bearing. Godot resolves a SubResource against the blocks it has
// already read, so the one that is referenced has to be written first, and a
// JSON object cannot carry that.
// Every property name the pinned API declares for a type, walking up its
// ancestors. Empty and `false` when the reference does not carry the type,
// which is the case for a script class, a type from another extension, or a
// reference file that is not installed. Those cannot be checked and must not be
// refused.
// The properties a type has, with the type the class reference declares for
// each. The name half refuses a property the type does not have; the type half
// decides which literal a `{x, y}` is written as, because the JSON cannot say
// and the shape of it guesses wrong for every integer vector (#730).
bool declaredPropertyNames(const std::string& resource_type, std::set<std::string>& names,
                           std::map<std::string, std::string>* types = nullptr) {
    const auto& reference = offline::ClassReference::instance();
    std::string current = resource_type;
    std::set<std::string> visited;
    const json* record = reference.find(current);
    if (!record) return false;
    while (record) {
        if (record->contains("properties") && (*record)["properties"].is_object()) {
            for (auto it = (*record)["properties"].begin();
                 it != (*record)["properties"].end(); ++it) {
                names.insert(it.key());
                // A base class declaring the same name does not override the
                // derived one, which was inserted first.
                if (types && it.value().is_object() && it.value().contains("type") &&
                    it.value()["type"].is_string()) {
                    types->emplace(it.key(), it.value()["type"].get<std::string>());
                }
            }
        }
        const std::string parent = record->value("inherits", std::string());
        if (parent.empty() || !visited.insert(parent).second) break;
        current = parent;
        record = reference.find(current);
    }
    return true;
}

// A name Godot stores but does not declare as a property. The API dump lists
// only the inspector-visible set, so `_data` on a Curve, `sources/0` on a
// TileSet and `tracks/0/type` on an Animation are all legitimate and all
// absent from it. Those are reported rather than refused; a plain identifier
// has no such excuse.
bool isStorageOnlyPropertyName(const std::string& name) {
    return name.empty() || name.front() == '_' || name.find('/') != std::string::npos;
}

// The type each property of this type is declared as. Empty for a type the
// class reference does not carry, which is the same condition
// checkPropertiesAgainstType reports as unchecked.
//
// The whole declared type rather than just the component ones: a slot declared
// float or int or bool has no constructor to choose, and it is exactly those
// slots that took any value at all before (#764).
std::map<std::string, std::string> declaredPropertyTypes(const std::string& resource_type) {
    std::set<std::string> names;
    std::map<std::string, std::string> types;
    if (!declaredPropertyNames(resource_type, names, &types)) return {};
    return types;
}

// The literal the shape of this JSON would have produced on its own, or empty
// when the shape decides nothing. Only used to say whether naming the declared
// type changed the answer, so a caller can see the correction happen.
const char* shapeDerivedComponentName(const json& value) {
    if (!value.is_object()) return nullptr;
    if (value.size() == 4 && hasNumericKeys(value, {"r", "g", "b", "a"})) return "Color";
    if (value.size() == 3 && hasNumericKeys(value, {"r", "g", "b"})) return "Color";
    if (hasNumericKeys(value, {"x", "y", "z", "w"})) return "Vector4";
    if (hasNumericKeys(value, {"x", "y", "z"})) return "Vector3";
    if (hasNumericKeys(value, {"x", "y"})) return "Vector2";
    return nullptr;
}

// What the attached engine said about a resource type, and which engine said
// it. Absent when no session is attached, or when the attached bridge is older
// than the engine.classExists route: the pinned reference is the only list
// available then, and the report says so rather than implying an engine agreed.
struct EngineTypeVerdict {
    // Whether the attached engine was asked and replied. An older bridge has no
    // engine.classExists route, and a bridge that could not answer is not a
    // bridge that said yes.
    bool answered = false;
    bool known = false;
    std::string engine_version;
};

// Refuses property names the type does not have. Godot drops them silently on
// load, so the file was well formed, the caller was told they were written, and
// nothing in the surface would ever show the loss.
Result<json> checkPropertiesAgainstType(
    const std::string& resource_type,
    const std::vector<std::pair<std::string, json>>& properties,
    const std::string& where,
    bool allow_unknown_type,
    const std::optional<EngineTypeVerdict>& engine_verdict) {
    // The engine that will load the file is the one whose class list it has to
    // satisfy. The pinned dump is 4.7 and a 4.5.1 engine has 65 fewer classes,
    // so a type in the dump and absent from that engine wrote a file the engine
    // refused outright -- "Can't create sub resource of type", the whole
    // resource rather than one dropped property -- while the same response
    // carried api_version_matches_attached_engine: false (#766). Measured:
    // DrawableTexture2D and BlitMaterial exist only on 4.7, JointLimitation3D
    // arrived in 4.6.
    if (engine_verdict.has_value() && engine_verdict->answered && !engine_verdict->known &&
        !allow_unknown_type) {
        return Error(
            400,
            where + ": " + resource_type + " is not a class in " +
            engine_verdict->engine_version +
            ", which is the engine this session is attached to. Godot cannot load a "
            "resource whose type it does not know, so the file would fail to load rather "
            "than lose a property. Check the spelling with script_reflect_class. If the "
            "type comes from a GDExtension or a class_name script, which neither the "
            "shipped class reference nor ClassDB lists, pass allow_unknown_type: true.",
            json{{"retry_with", {{"allow_unknown_type", true}}}});
    }
    // Which list decided the type, so `checked: true` says what it was checked
    // against. Only when a session is attached: with none there is no second
    // list to have preferred, and every offline answer keeps the shape it had.
    const auto noteOracle = [&](json report) {
        if (!engine_verdict.has_value()) return report;
        report["type_checked_against"] =
            engine_verdict->answered ? "attached_engine" : "api_reference";
        return report;
    };
    std::set<std::string> declared;
    std::map<std::string, std::string> declared_types;
    if (!declaredPropertyNames(resource_type, declared, &declared_types)) {
        // Godot does not drop one property for a type it does not have; it
        // fails to instantiate the resource at all, so the file this would
        // write cannot be loaded. Refusing it is the same rule the property
        // check already applies, applied to the type (#465). The escape hatch
        // is real -- a class_name script or a GDExtension type is not in the
        // dump either -- so it is named rather than removed.
        // A GDExtension type is in the attached engine's ClassDB and in no
        // dump, so an engine that has the class settles the question the
        // reference could not. Its properties still cannot be checked.
        const bool engine_has_type =
            engine_verdict.has_value() && engine_verdict->answered && engine_verdict->known;
        if (!allow_unknown_type && !engine_has_type) {
            return Error::invalidArgument(
                where + ": " + resource_type +
                " is not a class in " + offline::ClassReference::instance().apiVersion() +
                ". Godot cannot load a resource whose type it does not know, so writing "
                "the file would report a resource that does not exist. Check the spelling "
                "with script_reflect_class. If the type comes from a GDExtension or a "
                "class_name script, which the shipped class reference cannot see, pass "
                "allow_unknown_type: true.");
        }
        return noteOracle(json{{"checked", false},
                               {"reason", "type_not_in_api_reference"},
                               {"allowed_by", engine_has_type ? "attached_engine"
                                                              : "allow_unknown_type"}});
    }
    std::vector<std::string> unknown;
    json unverified = json::array();
    json retyped = json::object();
    for (const auto& [name, value] : properties) {
        // `script` is how a resource gets properties of its own, and it is the
        // one case where names the type does not declare are expected. The API
        // dump does not list it as a property of Object, so it is named here.
        if (name == "script") continue;
        if (declared.count(name)) {
            // The declared type is about to be used for the literal instead of
            // the shape of the JSON. Where the two disagree the caller's file
            // changes, so it is reported rather than corrected in silence.
            const auto declaration = declared_types.find(name);
            if (declaration != declared_types.end() && !asksForANamedType(value)) {
                if (const auto* component = findComponentType(declaration->second)) {
                    const char* guess = shapeDerivedComponentName(value);
                    if (guess && std::strcmp(guess, component->name) != 0) {
                        retyped[name] = component->name;
                    }
                }
            }
            continue;
        }
        if (isStorageOnlyPropertyName(name)) {
            unverified.push_back(name);
            continue;
        }
        unknown.push_back(name);
    }
    if (!unknown.empty()) {
        std::string names;
        for (size_t i = 0; i < unknown.size(); ++i) {
            if (i > 0) names += ", ";
            names += "'" + unknown[i] + "'";
        }
        return Error::invalidArgument(
            where + ": " + resource_type + " does not declare " + names +
            ". Godot drops a property the type does not have when it loads the file, so "
            "writing it would report work that did not happen. Use script_reflect_class to "
            "see what " + resource_type + " declares.");
    }
    json report = {{"checked", true},
                   {"api_version", offline::ClassReference::instance().apiVersion()}};
    if (!unverified.empty()) report["not_declared_but_written"] = std::move(unverified);
    if (!retyped.empty()) report["written_as_declared_type"] = std::move(retyped);
    // The one way past the guard above with a type the attached engine does not
    // have. The property names were still checked against the pinned reference,
    // so `checked: true` is true and would read as agreement without this.
    if (engine_verdict.has_value() && engine_verdict->answered && !engine_verdict->known) {
        report["type_unknown_to_attached_engine"] = true;
    }
    return noteOracle(std::move(report));
}

// What the attached engine has in its ClassDB, for the types this call is about.
// Empty when nothing is attached or the bridge could not answer, which the
// caller reads as "no engine spoke" rather than as "the engine said no".
//
// One round trip for every type in the call. project_audit_assets set the
// precedent for an offline answer the live engine gets to correct; this is the
// same move applied before the file is written rather than after.
std::map<std::string, bool> askAttachedEngineForTypes(
    const std::shared_ptr<ipc::IIpcClient>& ipc, const std::vector<std::string>& type_names) {
    std::map<std::string, bool> known;
    if (!ipc || !ipc->isConnected() || type_names.empty()) return known;
    // The bridge's own cap, and it is the largest shape resource_create can
    // produce: one root type and the 64 sub_resources entries already allow,
    // with duplicates folded out. Falling back here would be the silent
    // reference check this route exists to replace.
    if (type_names.size() > 65) return known;
    auto response = ipc->sendRequest("engine.classExists", json{{"class_names", type_names}},
                                     ipc::kWaitForDefinitiveResponse);
    if (response.isErr() || !response.value().is_object()) return known;
    const auto& body = response.value();
    if (!body.contains("classes") || !body["classes"].is_array()) return known;
    for (const auto& entry : body["classes"]) {
        if (!entry.is_object()) continue;
        const auto name = entry.value("name", std::string{});
        if (name.empty() || !entry.contains("exists") || !entry["exists"].is_boolean()) continue;
        known[name] = entry["exists"].get<bool>();
    }
    return known;
}

struct SubResourceSpec {
    std::string id;
    std::string resource_type;
    std::vector<std::pair<std::string, json>> properties;
};

bool isUsableSubResourceId(const std::string& id) {
    if (id.empty() || id.size() > 128) return false;
    for (unsigned char character : id) {
        if (!std::isalnum(character) && character != '_' && character != '-') return false;
    }
    return true;
}

Result<std::vector<SubResourceSpec>> parseSubResources(const json& args) {
    std::vector<SubResourceSpec> parsed;
    if (!args.is_object() || !args.contains("sub_resources")) return parsed;
    const auto& entries = args["sub_resources"];
    if (!entries.is_array()) {
        return Error::invalidArgument(
            "sub_resources must be an array of {id, resource_type, properties} entries, in the "
            "order they should appear in the file");
    }
    if (entries.size() > 64) {
        return Error::invalidArgument("sub_resources holds at most 64 entries");
    }
    for (const auto& entry : entries) {
        if (!entry.is_object()) {
            return Error::invalidArgument("Each sub_resources entry must be an object");
        }
        SubResourceSpec spec;
        spec.id = entry.value("id", std::string());
        spec.resource_type = entry.value("resource_type", std::string());
        if (!isUsableSubResourceId(spec.id)) {
            return Error::invalidArgument(
                "Each sub_resources entry needs an \"id\" of letters, digits, underscores or "
                "hyphens; it is the name properties use to point at it");
        }
        if (spec.resource_type.empty()) {
            return Error::invalidArgument("sub_resources entry \"" + spec.id +
                                          "\" needs a \"resource_type\", such as TileSetAtlasSource");
        }
        for (const auto& existing : parsed) {
            if (existing.id == spec.id) {
                return Error::invalidArgument("sub_resources declares \"" + spec.id + "\" twice");
            }
        }
        auto properties = orderedProperties(entry.value("properties", json::object()));
        if (properties.isErr()) {
            return Error::invalidArgument("sub_resources entry \"" + spec.id + "\": " +
                                          properties.error().message);
        }
        spec.properties = std::move(properties.value());
        parsed.push_back(std::move(spec));
    }
    return parsed;
}

CallToolResult handleResourceCreate(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    std::string resource_type = args.value("resource_type", "StandardMaterial3D");
    std::string save_path = args.value("save_path", "");
    json properties = args.value("properties", json::object());
    if (args.contains("overwrite") && !args["overwrite"].is_boolean()) {
        return CallToolResult::errorJson(400, "Parameter 'overwrite' must be a boolean.");
    }
    const bool overwrite = args.value("overwrite", false);
    if (args.contains("allow_unknown_type") && !args["allow_unknown_type"].is_boolean()) {
        return CallToolResult::errorJson(400, "Parameter 'allow_unknown_type' must be a boolean.");
    }
    const bool allow_unknown_type = args.value("allow_unknown_type", false);

    if (save_path.empty()) {
        return CallToolResult::errorJson(
            400, "Parameter 'save_path' is required (e.g. res://materials/wood.tres).");
    }

    auto ordered = orderedProperties(properties);
    if (ordered.isErr()) return CallToolResult::fromError(ordered.error());

    auto sub_parsed = parseSubResources(args);
    if (sub_parsed.isErr()) return CallToolResult::fromError(sub_parsed.error());
    const auto& sub_resources = sub_parsed.value();

    // Which engine will load this file, asked before the type guard runs. The
    // guard's own description says a type the engine does not know makes a
    // resource that cannot be loaded, and it was checking that against the
    // pinned 4.7 dump rather than against the attached engine (#766).
    const auto sessions = std::dynamic_pointer_cast<runtime::IRuntimeSessionClient>(ipc);
    const auto attached = sessions ? sessions->observableSession()
                                   : std::optional<runtime::SessionDescriptor>{};
    std::map<std::string, bool> engine_classes;
    if (attached.has_value()) {
        std::vector<std::string> wanted{resource_type};
        for (const auto& sub : sub_resources) {
            if (sub.resource_type.empty()) continue;
            if (std::find(wanted.begin(), wanted.end(), sub.resource_type) == wanted.end()) {
                wanted.push_back(sub.resource_type);
            }
        }
        engine_classes = askAttachedEngineForTypes(ipc, wanted);
    }
    const auto verdictFor = [&](const std::string& type_name) {
        std::optional<EngineTypeVerdict> verdict;
        if (!attached.has_value()) return verdict;
        EngineTypeVerdict answer;
        answer.engine_version = attached->engine_version;
        const auto found = engine_classes.find(type_name);
        answer.answered = found != engine_classes.end();
        answer.known = answer.answered && found->second;
        verdict = answer;
        return verdict;
    };

    // Before anything is rendered or written. A property the type does not
    // declare used to be written into [resource], reported in
    // properties_written, and then dropped by Godot on load with nothing in the
    // surface able to show the loss: resource_inspect reports type, size, uid
    // and dependencies, and no properties.
    auto property_check = checkPropertiesAgainstType(resource_type, ordered.value(),
                                                     "Argument 'properties'",
                                                     allow_unknown_type,
                                                     verdictFor(resource_type));
    if (property_check.isErr()) return CallToolResult::fromError(property_check.error());

    // `checked: true` reads as "verified against your engine", and it was
    // verified against whichever engine the shipped dump was taken from. A
    // property added in 4.7 passes the check and is then dropped by a 4.5.1
    // engine, which is the exact failure the check exists to prevent. The
    // caller gets what script_reflect_class already gives them, so they can
    // weigh the verdict (#466).
    const auto note_engine = [&](json& check) {
        if (!attached.has_value() || !check.value("checked", false)) return;
        versions::annotateApiVersion(check, check.value("api_version", std::string()),
                                     attached->engine_version);
    };
    note_engine(property_check.value());

    json sub_property_checks = json::object();
    for (const auto& sub : sub_resources) {
        auto sub_check = checkPropertiesAgainstType(
            sub.resource_type, sub.properties,
            "Sub-resource '" + sub.id + "' properties", allow_unknown_type,
            verdictFor(sub.resource_type));
        if (sub_check.isErr()) return CallToolResult::fromError(sub_check.error());
        note_engine(sub_check.value());
        sub_property_checks[sub.id] = sub_check.value();
    }

    // The body written below is Godot text-resource markup and nothing else.
    // Writing it to any path the caller names produced a .gd file full of
    // [gd_resource] markup, reported as created, that script_check_syntax in
    // the same session immediately called unparseable. Refuse the target
    // instead of writing a file the engine cannot load.
    {
        const auto dot = save_path.find_last_of('.');
        const auto slash = save_path.find_last_of('/');
        std::string extension =
            (dot == std::string::npos || (slash != std::string::npos && dot < slash))
                ? std::string()
                : save_path.substr(dot);
        for (auto& character : extension) {
            character = static_cast<char>(
                std::tolower(static_cast<unsigned char>(character)));
        }
        if (extension != ".tres" && extension != ".res") {
            return CallToolResult::errorJson(
                400,
                "resource_create writes Godot text-resource markup, so save_path must end in "
                ".tres or .res; received \"" + save_path +
                "\". Use script_create for a GDScript file.");
        }
    }

    // Offline generator for common .tres resources
    namespace fs = std::filesystem;

    // The shared resolver rather than a second copy of the rules. This wrote
    // its own containment check, which caught an escaping path but accepted
    // shapes every other writing tool rejects -- an absolute path landing
    // inside the root, for one -- and then wrote through the raw relative path
    // rather than the resolved one. It also called projectPathFromUtf8 outside
    // its own try, so a save_path that is not valid UTF-8 threw out of the
    // handler instead of returning the error every other tool returns.
    //
    // project_path.hpp says why this function exists: repeating the traversal,
    // UTF-8 and containment rules per writer is how two of them end up
    // disagreeing. This one was the disagreement.
    auto resolved = paths::resolveProjectFileForWrite(save_path);
    if (resolved.isErr()) {
        return CallToolResult::fromError(resolved.error(), "Invalid save_path: ");
    }
    const fs::path target_p = resolved.value();
    // The readers' spelling of the path, and the on-disk case when a file is
    // already there: a conflict against res://RES.tres named a file that did
    // not exist while res://res.tres was the one overwrite would replace
    // (#546, #551).
    const std::string reported_path = paths::resourcePathOf(target_p);

    try {
        std::error_code probe_error;
        if (fs::exists(target_p, probe_error) && !probe_error && !overwrite) {
            return CallToolResult::errorJson(
                409, "Resource already exists; pass overwrite: true to replace it: " + reported_path,
                {{"code", "already_exists"}, {"retry_with", {{"overwrite", true}}}});
        }
        if (target_p.has_parent_path()) {
            fs::create_directories(target_p.parent_path());
        }
    } catch (const std::exception& e) {
        return CallToolResult::errorJson(400, std::string("Path resolution error: ") + e.what());
    }

    // Everything is rendered before anything is written, so a property this
    // writer cannot express refuses the call instead of leaving a half-written
    // resource that reports success.
    //
    // The body is rendered first and the header second, because the header is
    // made of what the body turned out to reference: which files got an
    // [ext_resource] line, and how many entries load_steps has to count.
    ReferenceScope scope;
    std::ostringstream sub_blocks;
    json sub_written = json::array();
    for (const auto& sub : sub_resources) {
        std::ostringstream block;
        block << "\n[sub_resource type=\"" << sub.resource_type << "\" id=\"" << sub.id << "\"]\n";
        json names = json::array();
        const auto sub_declared = declaredPropertyTypes(sub.resource_type);
        for (const auto& [name, value] : sub.properties) {
            const auto declaration = sub_declared.find(name);
            auto literal = tresLiteral(
                value, sub.id + "." + name, &scope,
                declaration == sub_declared.end() ? nullptr : &declaration->second);
            if (literal.isErr()) return CallToolResult::fromError(literal.error());
            block << name << " = " << literal.value() << "\n";
            names.push_back(name);
        }
        // Only now is this id nameable. A sub-resource that referenced itself,
        // or one declared after it, would read as null in Godot.
        scope.visible_sub_ids.push_back(sub.id);
        sub_blocks << block.str();
        sub_written.push_back({{"id", sub.id}, {"resource_type", sub.resource_type},
                               {"properties_written", std::move(names)}});
    }

    std::ostringstream body;
    body << "\n[resource]\n";
    json written_order = json::array();
    const auto root_declared = declaredPropertyTypes(resource_type);
    for (const auto& [name, value] : ordered.value()) {
        const auto declaration = root_declared.find(name);
        auto literal = tresLiteral(value, name, &scope,
                                   declaration == root_declared.end()
                                       ? nullptr
                                       : &declaration->second);
        if (literal.isErr()) return CallToolResult::fromError(literal.error());
        body << name << " = " << literal.value() << "\n";
        written_order.push_back(name);
    }

    std::ostringstream out;
    const size_t load_steps = scope.externals.size() + sub_resources.size() + 1;
    out << "[gd_resource type=\"" << resource_type << "\"";
    // Godot writes load_steps only when there is more than the resource itself,
    // and omitting it where it is 1 is what the editor's own files look like.
    if (load_steps > 1) out << " load_steps=" << load_steps;
    out << " format=3]\n";
    json externals_written = json::array();
    for (const auto& reference : scope.externals) {
        out << "\n[ext_resource type=\"" << reference.type << "\"";
        if (!reference.uid.empty()) out << " uid=\"" << reference.uid << "\"";
        out << " path=\"" << reference.path << "\" id=\"" << reference.id << "\"]\n";
        externals_written.push_back({{"path", reference.path}, {"resource_type", reference.type},
                                     {"id", reference.id}, {"uid", reference.uid}});
    }
    out << sub_blocks.str() << body.str();

    auto written = files::writeFileAtomically(target_p, out.str());
    offline::ResourceIndexer::invalidateSharedIndex();
    if (written.isErr()) {
        return CallToolResult::error("Failed to write resource file to disk: " +
                                     written.error().message);
    }
    return CallToolResult::successJson({
        {"status", "created_offline"},
        {"save_path", reported_path},
        {"resource_type", resource_type},
        // In file order, because that is the order Godot applies them in and
        // the caller has no other way to see what it got.
        {"properties_written", std::move(written_order)},
        {"sub_resources_written", std::move(sub_written)},
        {"external_references", std::move(externals_written)},
        {"load_steps", load_steps},
        // Says whether the names were checked at all, so "written" is not read
        // as "the type has these" when the reference could not answer.
        {"property_check", property_check.value()},
        {"sub_resource_property_checks", std::move(sub_property_checks)}
    });
}

CallToolResult handleResourceInspect(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    std::string resource_path = args.value("resource_path", "");
    if (resource_path.empty()) {
        return CallToolResult::errorJson(400, "Parameter 'resource_path' is required.");
    }

    // Exact match. A prefix match reported res://player.gd.uid or
    // res://player.gdextension for res://player.gd, and the scan order is
    // unsorted, so which sibling came back was down to directory order.
    const auto indexer = offline::ResourceIndexer::sharedIndex(".");
    if (const auto* found = indexer->findExact(resource_path)) {
        return CallToolResult::successJson(found->toJson());
    }

    // "You pointed at a directory, pass a file" and "that path does not
    // exist" lead to different next actions -- fix the argument, or go find the
    // file -- and one message supported neither (#426).
    const auto resolved = paths::resolveProjectFileForWrite(resource_path);
    if (resolved.isOk()) {
        std::error_code directory_error;
        if (std::filesystem::is_directory(resolved.value(), directory_error) && !directory_error) {
            return CallToolResult::errorJson(
                400, resource_path + " is a directory, not a resource. Use "
                                     "project_list_resources to list what is beneath it.");
        }
    }
    return CallToolResult::errorJson(404, "Resource not found: " + resource_path);
}

CallToolResult handleAudioConfigureBus(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    // Live only, and that is the point rather than a gap. Writing the layout
    // file would change what the project loads next time and not what anyone
    // is listening to now, which is the opposite of what someone chasing a
    // silent bus wants.
    if (!ipc || !ipc->isConnected()) {
        // Unreachable in practice: the registry's live-route check answers a
        // live-only tool before the handler runs, and it now carries this
        // tool's offline sibling in the shared refusal (#615). Kept as the
        // handler's own floor, in the same words, for any path that reaches
        // here without going through that check.
        return CallToolResult::error(
            "Godot Editor is offline. Audio bus state lives in the running engine, so launch "
            "Godot to change it. audio_list_buses still reads the project layout offline.");
    }
    auto response = ipc->sendRequest("audio.configureBus", args, ipc::kWaitForDefinitiveResponse);
    if (response.isErr()) {
        return CallToolResult::error("Failed to configure the audio bus: " + response.error().message);
    }
    return CallToolResult::successJson(response.value());
}

CallToolResult handleAudioListBuses(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (!args.is_object() || !args.empty()) {
        return CallToolResult::error("Invalid audio request: this tool takes no arguments");
    }
    // Live first, because a bus a script muted at runtime is exactly the case
    // someone is looking for and the layout file cannot show it. The offline
    // read is a fallback, and it says so in the result rather than letting the
    // caller assume they are looking at live state.
    if (ipc && ipc->isConnected()) {
        auto response = ipc->sendRequest("audio.listBuses", args, ipc::kWaitForDefinitiveResponse);
        if (response.isOk()) return CallToolResult::successJson(response.value());
    }

    auto layout = offline::readAudioBusLayout(".");
    if (layout.isErr()) return CallToolResult::fromError(layout.error());
    auto payload = layout.value();
    payload["execution_mode"] = "offline_fallback";
    payload["is_live_engine"] = false;
    return CallToolResult::successJson(std::move(payload));
}

CallToolResult handleProjectRenameReferences(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    (void)ipc;
    if (!args.is_object()) {
        return CallToolResult::error("Invalid rename request: arguments must be an object");
    }
    offline::ProjectRenameOptions options;
    if (!args.contains("target") || !args["target"].is_string()) {
        return CallToolResult::error("Invalid rename request: target must be a string");
    }
    if (!args.contains("new_name") || !args["new_name"].is_string()) {
        return CallToolResult::error("Invalid rename request: new_name must be a string");
    }
    options.target = args["target"].get<std::string>();
    options.new_name = args["new_name"].get<std::string>();
    if (args.contains("max_impacts")) {
        const auto& value = args["max_impacts"];
        if (!value.is_number_integer() || value.get<int64_t>() < 1 || value.get<int64_t>() > 5000) {
            return CallToolResult::error(
                "Invalid rename request: max_impacts must be an integer from 1 to 5000");
        }
        options.max_impacts = static_cast<size_t>(value.get<int64_t>());
    }

    auto report = offline::renameReferences(".", options);
    if (report.isErr()) {
        // The conflict and truncation refusals carry the evidence a caller needs
        // to act, so they travel with the message rather than being flattened
        // into it.
        const auto& failure = report.error();
        if (failure.data.is_object() && !failure.data.empty()) {
            return CallToolResult::error(
                json{{"error", {{"code", failure.code}, {"message", failure.message},
                                {"data", failure.data}}}}.dump());
        }
        return CallToolResult::error(failure.message);
    }
    auto payload = report.value();
    payload["execution_mode"] = "offline_fallback";
    return CallToolResult::successJson(std::move(payload));
}

CallToolResult handleProjectVerifyChanges(const json& args) {
    auto parsed = offline::parseSpeculativeVerifyRequest(args);
    if (parsed.isErr()) {
        return CallToolResult::error("Invalid verification request: " + parsed.error().message);
    }
    auto verified = offline::verifyChangesInSandbox(parsed.value());
    if (verified.isErr()) {
        return CallToolResult::fromError(verified.error());
    }
    return CallToolResult::successJson(verified.value().toJson());
}

CallToolResult handleProjectApplyChanges(const json& args) {
    auto parsed = offline::parseSpeculativeVerifyRequest(args);
    if (parsed.isErr()) {
        return CallToolResult::error("Invalid apply request: " + parsed.error().message);
    }
    auto applied = offline::applyVerifiedChanges(parsed.value());
    if (applied.isErr()) {
        // The same envelope project_verify_changes uses. This built one by hand
        // and only when the error carried data, so every failure without data
        // came back as a bare string: the same condition reached through
        // verify was wrapped and through apply was not, on a path the surface
        // census does not reach because it is behind the confirmation gate.
        return CallToolResult::fromError(applied.error());
    }
    auto payload = applied.value().toJson();
    payload["execution_mode"] = "offline_fallback";
    // A proposal the check rejected is not an error in this tool. The tool did
    // what it promises, which is to write nothing when the proposal does not
    // hold up, and the report says why.
    auto result = CallToolResult::successJson(std::move(payload));
    result.isError = !applied.value().applied;
    return result;
}

CallToolResult handleProjectAnalyzeImpact(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    (void)ipc;
    if (!args.is_object()) {
        return CallToolResult::error("Invalid impact request: arguments must be an object");
    }
    offline::ProjectImpactOptions options;
    if (!args.contains("target") || !args["target"].is_string()) {
        return CallToolResult::error("Invalid impact request: target must be a string");
    }
    options.target = args["target"].get<std::string>();
    if (args.contains("max_impacts")) {
        const auto& value = args["max_impacts"];
        if (!value.is_number_integer() || value.get<int64_t>() < 1 || value.get<int64_t>() > 5000) {
            return CallToolResult::error(
                "Invalid impact request: max_impacts must be an integer from 1 to 5000");
        }
        options.max_impacts = static_cast<size_t>(value.get<int64_t>());
    }

    auto report = offline::analyzeImpact(".", options);
    if (report.isErr()) return CallToolResult::fromError(report.error());
    auto payload = report.value();
    payload["execution_mode"] = "offline_fallback";
    return CallToolResult::successJson(std::move(payload));
}

CallToolResult handleProjectAuditAssets(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (!args.is_object()) {
        return CallToolResult::error("Invalid audit request: arguments must be an object");
    }
    offline::ProjectAuditOptions options;
    for (const auto& [key, target] : {std::pair<const char*, bool*>{"include_orphans", &options.include_orphans},
                                      {"include_broken_references", &options.include_broken_references},
                                      {"include_dead_signals", &options.include_dead_signals},
                                      {"include_import_health", &options.include_import_health},
                                      {"include_addon_orphans", &options.include_addon_orphans}}) {
        if (!args.contains(key)) continue;
        if (!args[key].is_boolean()) {
            return CallToolResult::error(std::string("Invalid audit request: ") + key +
                                         " must be a boolean");
        }
        *target = args[key].get<bool>();
    }
    if (args.contains("max_findings")) {
        const auto& value = args["max_findings"];
        if (!value.is_number_integer() || value.get<int64_t>() < 1 || value.get<int64_t>() > 5000) {
            return CallToolResult::error(
                "Invalid audit request: max_findings must be an integer from 1 to 5000");
        }
        options.max_findings = static_cast<size_t>(value.get<int64_t>());
    }
    if (!options.include_orphans && !options.include_broken_references &&
        !options.include_dead_signals && !options.include_import_health) {
        return CallToolResult::error(
            "Invalid audit request: at least one of include_orphans, "
            "include_broken_references, include_dead_signals or include_import_health "
            "must stay enabled");
    }

    auto report = offline::auditProject(".", options);
    // The scan is a file scan in every case and says so on every call. The
    // execution mode below describes whether an engine contributed to the
    // findings, which is the question a caller weighing them is actually
    // asking; reference_verification carries the detail.
    report["scan_source"] = "project_files";
    report["execution_mode"] = "offline_fallback";

    // A uid no project file records is reported as a broken reference, because
    // offline that is the only reading available. With an editor attached the
    // engine's own table can say whether it is true, and a reference the engine
    // resolves is not broken -- nor is the file it points at an orphan. Both
    // findings are corrected rather than annotated, because leaving a finding
    // in place with a footnote saying it is wrong is how a tool trains people
    // to skim past it.
    constexpr size_t kMaxVerifications = 256; // the bridge's own batch cap
    std::vector<std::string> candidates;
    std::set<std::string> distinct_findings;
    // UID findings first, then path findings. A fixed order matters because the
    // budget can cut the list, and a caller comparing two runs of an unchanged
    // project should get the same answer both times.
    for (const char* kind : {"unresolved_uid", "missing_file"}) {
        if (!report.contains("broken_references") || !report["broken_references"].is_array()) break;
        for (const auto& entry : report["broken_references"]) {
            if (entry.value("kind", std::string{}) != kind) continue;
            const auto target = entry.value("target", std::string{});
            if (target.empty() || !distinct_findings.insert(target).second) continue;
            if (candidates.size() < kMaxVerifications) candidates.push_back(target);
        }
    }

    json verification{{"mode", candidates.empty() ? "not_needed" : "unavailable"},
                      {"checked", 0},
                      {"truncated", distinct_findings.size() > candidates.size()}};

    if (!candidates.empty() && ipc && ipc->isConnected()) {
        auto response = ipc->sendRequest("project.resolveUids", json{{"queries", candidates}},
                                         ipc::kWaitForDefinitiveResponse);
        if (response.isOk() && response.value().is_object() &&
            response.value().contains("entries") && response.value()["entries"].is_array()) {
            // A uid finding is disproved by the engine resolving it. A path
            // finding is disproved by the engine being able to load it, which
            // is a different question: a path can load with no UID registered,
            // and a UID can be registered for a file that is gone.
            std::map<std::string, std::string> resolved_uids;
            std::map<std::string, bool> loadable_paths;
            for (const auto& entry : response.value()["entries"]) {
                const auto query = entry.value("query", std::string{});
                if (query.empty()) continue;
                if (query.rfind("uid://", 0) == 0) {
                    if (entry.value("found", false) && entry.value("exists", true)) {
                        resolved_uids[query] = entry.value("path", std::string{});
                    }
                } else if (entry.contains("exists") && entry["exists"].is_boolean()) {
                    loadable_paths[query] = entry["exists"].get<bool>();
                }
            }

            json kept = json::array();
            json engine_only = json::array();
            for (auto entry : report["broken_references"]) {
                const auto kind = entry.value("kind", std::string{});
                const auto target = entry.value("target", std::string{});
                if (kind == "unresolved_uid") {
                    const auto resolved = resolved_uids.find(target);
                    if (resolved != resolved_uids.end()) {
                        engine_only.push_back({{"source", entry.value("source", std::string{})},
                                               {"target", target},
                                               {"kind", kind},
                                               {"engine_path", resolved->second}});
                        continue;
                    }
                    if (std::find(candidates.begin(), candidates.end(), target) != candidates.end()) {
                        entry["confirmed_by_engine"] = true;
                    }
                } else if (kind == "missing_file") {
                    const auto loadable = loadable_paths.find(target);
                    if (loadable != loadable_paths.end()) {
                        if (loadable->second) {
                            engine_only.push_back({{"source", entry.value("source", std::string{})},
                                                   {"target", target},
                                                   {"kind", kind},
                                                   {"engine_path", target}});
                            continue;
                        }
                        entry["confirmed_by_engine"] = true;
                    }
                }
                kept.push_back(std::move(entry));
            }
            report["broken_references"] = std::move(kept);
            report["engine_only_references"] = engine_only;

            // A file the engine proved is referenced cannot also be
            // unreferenced. Only uid findings can reach this: a path the scan
            // never indexed is not in the orphan set either.
            size_t orphans_cleared = 0;
            if (!resolved_uids.empty() && report.contains("orphans") && report["orphans"].is_array()) {
                std::set<std::string> referenced;
                for (const auto& [uid, resolved_path] : resolved_uids) {
                    if (!resolved_path.empty()) referenced.insert(resolved_path);
                }
                json kept_orphans = json::array();
                uint64_t reclaimed = 0;
                for (const auto& orphan : report["orphans"]) {
                    if (referenced.count(orphan.value("path", std::string{})) != 0) {
                        reclaimed += orphan.value("file_size", static_cast<uint64_t>(0));
                        ++orphans_cleared;
                        continue;
                    }
                    kept_orphans.push_back(orphan);
                }
                if (orphans_cleared > 0) {
                    report["orphans"] = std::move(kept_orphans);
                    const auto counted = report.value("orphan_bytes", static_cast<uint64_t>(0));
                    report["orphan_bytes"] = counted > reclaimed ? counted - reclaimed : 0;
                }
            }

            size_t confirmed = 0;
            for (const auto& entry : report["broken_references"]) {
                if (entry.value("confirmed_by_engine", false)) ++confirmed;
            }

            report["execution_mode"] = "live";
            verification["mode"] = "live";
            verification["checked"] = candidates.size();
            verification["cleared"] = engine_only.size();
            verification["confirmed"] = confirmed;
            verification["orphans_cleared"] = orphans_cleared;

            if (!engine_only.empty() && report.contains("limitations") &&
                report["limitations"].is_array()) {
                report["limitations"].push_back(
                    "A reference under engine_only_references resolves in this editor but no "
                    "scanned project file accounts for it. The editor's table is not in the "
                    "repository, so a fresh checkout or another machine may report it broken.");
            }
        }
    }
    // "not_needed" means the scan produced nothing an engine could verify, so
    // there was no live work to do and nothing to fall back from. Calling that
    // an offline fallback with an editor attached told a caller to reattach and
    // ask again for an answer reattaching cannot improve (#504). "unavailable"
    // is the genuine fallback: there were findings to check and no engine to
    // check them against.
    if (report.value("execution_mode", std::string{}) != "live" &&
        verification.value("mode", std::string{}) == "not_needed") {
        report["execution_mode"] = "local";
    }
    report["reference_verification"] = std::move(verification);
    return CallToolResult::successJson(std::move(report));
}

namespace {

// What the project files say about a query, next to what the engine said. The
// two disagree exactly when a sidecar is stale, missing, or newer than the
// editor's table, which is the drift a caller is looking for.
std::string uidIndexAgreement(const json& uid_map,
                              const std::map<std::string, std::string>& path_to_uid,
                              const json& engine_entry) {
    const std::string query = engine_entry.value("query", "");
    const bool engine_found = engine_entry.value("found", false);
    std::string index_answer;
    bool index_has = false;
    if (query.rfind("uid://", 0) == 0) {
        if (uid_map.contains(query)) {
            index_has = true;
            index_answer = uid_map.at(query).get<std::string>();
        }
        return index_has ? (engine_found && index_answer == engine_entry.value("path", "")
                                ? "agrees" : "differs")
                         : "absent";
    }
    if (query.rfind("res://", 0) == 0) {
        const auto found = path_to_uid.find(query);
        if (found != path_to_uid.end()) {
            index_has = true;
            index_answer = found->second;
        }
        return index_has ? (engine_found && index_answer == engine_entry.value("uid", "")
                                ? "agrees" : "differs")
                         : "absent";
    }
    return "absent";
}

} // namespace

CallToolResult handleProjectGetUidMap(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (!args.is_object()) {
        return CallToolResult::error("Invalid uid map request: arguments must be an object");
    }
    for (const auto& entry : args.items()) {
        if (entry.key() != "resolve") {
            return CallToolResult::error("Invalid uid map request: unknown parameter " + entry.key());
        }
    }
    std::vector<std::string> queries;
    if (args.contains("resolve")) {
        const auto& value = args["resolve"];
        if (!value.is_array() || value.empty() || value.size() > 256) {
            return CallToolResult::error(
                "Invalid uid map request: resolve must be an array of 1 to 256 strings");
        }
        for (const auto& item : value) {
            if (!item.is_string() || item.get<std::string>().empty()) {
                return CallToolResult::error(
                    "Invalid uid map request: resolve must contain only non-empty strings");
            }
            queries.push_back(item.get<std::string>());
        }
    }

    const auto indexer = offline::ResourceIndexer::sharedIndex(".");
    auto all_res = indexer->query("res://");

    json uid_map = json::object();
    std::map<std::string, std::string> path_to_uid;
    for (const auto& r : all_res) {
        if (!r.uid.empty()) {
            uid_map[r.uid] = r.path;
            path_to_uid[r.path] = r.uid;
        }
    }

    json payload{
        {"total_uids", uid_map.size()},
        {"uid_map", uid_map},
        // The map is always the file scan. ResourceUID resolves an id or a path
        // it is given but exposes no way to enumerate its table through
        // GDExtension, so a live enumeration would be an invented claim.
        {"uid_map_source", "project_files"}
    };

    if (queries.empty()) {
        // Not a fallback. ResourceUID exposes no enumeration through
        // GDExtension, so the map is a file scan whether or not an editor is
        // attached, and there is nothing an engine could add to this call.
        // Saying "offline_fallback" with an editor attached told a caller
        // reading the field as a quality signal to reattach and ask again, for
        // an answer reattaching cannot improve (#504). uid_map_source already
        // says why.
        payload["execution_mode"] = "local";
        payload["is_live_engine"] = false;
        return CallToolResult::successJson(std::move(payload));
    }

    if (ipc && ipc->isConnected()) {
        auto response = ipc->sendRequest("project.resolveUids", json{{"queries", queries}},
                                         ipc::kWaitForDefinitiveResponse);
        if (response.isOk() && response.value().is_object() &&
            response.value().contains("entries") && response.value()["entries"].is_array()) {
            json resolved = json::array();
            for (auto entry : response.value()["entries"]) {
                entry["source"] = "engine";
                entry["index_state"] = uidIndexAgreement(uid_map, path_to_uid, entry);
                resolved.push_back(std::move(entry));
            }
            payload["execution_mode"] = "live";
            payload["is_live_engine"] = true;
            payload["resolved"] = std::move(resolved);
            return CallToolResult::successJson(std::move(payload));
        }
    }

    json resolved = json::array();
    for (const auto& query : queries) {
        json entry{{"query", query}, {"found", false}, {"uid", ""}, {"path", ""},
                   {"source", "index"}};
        if (query.rfind("uid://", 0) == 0) {
            if (uid_map.contains(query)) {
                entry["found"] = true;
                entry["uid"] = query;
                entry["path"] = uid_map.at(query);
            } else {
                // Not the same claim as "does not exist". An imported asset
                // whose .import has not been written yet is unknown here and
                // known to a running editor.
                entry["reason"] = "not_in_project_files";
            }
        } else if (query.rfind("res://", 0) == 0) {
            const auto found = path_to_uid.find(query);
            if (found != path_to_uid.end()) {
                entry["found"] = true;
                entry["uid"] = found->second;
                entry["path"] = query;
            } else {
                entry["reason"] = "not_in_project_files";
            }
        } else {
            entry["reason"] = "unsupported_query";
        }
        resolved.push_back(std::move(entry));
    }
    // A fallback for real: the caller asked for resolutions, the engine is the
    // table that resolves them, and attaching one would improve this answer.
    payload["execution_mode"] = "offline_fallback";
    payload["is_live_engine"] = false;
    payload["resolved"] = std::move(resolved);
    return CallToolResult::successJson(std::move(payload));
}

CallToolResult handleInstantiateAsset(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    std::string asset_path = args.value("asset_path", "");
    std::string parent_path = args.value("parent_path", "/root");

    if (asset_path.empty()) {
        return CallToolResult::error("Parameter 'asset_path' is required.");
    }

    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("asset.instantiate", args);
        if (res.isOk()) {
            return CallToolResult::successJson(res.value());
        }
        return CallToolResult::error("Failed to instantiate asset in Godot: " + res.error().message);
    }

    return CallToolResult::error("Godot Editor is offline. Launch Godot Editor to instantiate assets directly into the scene tree.");
}

CallToolResult handleAssetReimport(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (!args.is_object() || !args.contains("paths") || !args["paths"].is_array() ||
        args["paths"].empty() || args["paths"].size() > 256) {
        return CallToolResult::error("Invalid asset reimport request: paths must be an array of 1 to 256 strings");
    }
    std::set<std::string> unique;
    for (const auto& value : args["paths"]) {
        if (!value.is_string()) {
            return CallToolResult::error("Invalid asset reimport request: paths must contain only strings");
        }
        const auto path = value.get<std::string>();
        const auto remainder = strings::startsWith(path, "res://") ? path.substr(6) : std::string();
        if (path.size() < 7 || path.size() > 1024 || !strings::startsWith(path, "res://") ||
            path.find('\0') != std::string::npos || path.find("..") != std::string::npos ||
            path.find('\\') != std::string::npos || strings::startsWith(path, "res://.godot/") ||
            strings::endsWith(path, ".import") || remainder.empty() || remainder.front() == '/' ||
            remainder.find("//") != std::string::npos || strings::startsWith(remainder, "./") ||
            remainder.find("/./") != std::string::npos || strings::endsWith(remainder, "/.") ||
            remainder.find(':') != std::string::npos) {
            return CallToolResult::error("Invalid asset reimport request: every path must be a normalized project-owned res:// source asset");
        }
        if (!unique.insert(path).second) {
            return CallToolResult::error("Invalid asset reimport request: paths must be unique");
        }
    }
    if (args.contains("timeout_ms") &&
        (!args["timeout_ms"].is_number_integer() || args["timeout_ms"].get<int64_t>() < 1 ||
         args["timeout_ms"].get<int64_t>() > 10000)) {
        return CallToolResult::error("Invalid asset reimport request: timeout_ms must be an integer from 1 to 10000");
    }
    if (!ipc || !ipc->isConnected()) {
        return CallToolResult::error("Godot Editor is offline. Launch Godot to reimport assets.");
    }
    auto response = ipc->sendRequest("asset.reimport", args, ipc::kWaitForDefinitiveResponse);
    // A reimport rewrites .import sidecars and can change uids, so the shared
    // scan is no longer trustworthy whether the call succeeded or not.
    offline::ResourceIndexer::invalidateSharedIndex();
    if (response.isErr()) {
        return CallToolResult::error("Failed to reimport assets: " + response.error().message);
    }
    return CallToolResult::successJson(response.value());
}

} // namespace mcp
} // namespace didi
