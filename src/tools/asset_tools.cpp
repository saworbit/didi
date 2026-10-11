#include "didi/offline/speculative_verify.hpp"
#include "didi/mcp/mcp_protocol.hpp"
#include "didi/common/cancellation.hpp"
#include "didi/common/ipc_channel.hpp"
#include "didi/common/logger.hpp"
#include "didi/offline/resource_indexer.hpp"
#include "didi/offline/project_audit.hpp"
#include "didi/offline/project_impact.hpp"
#include "didi/offline/project_text_scan.hpp"
#include "didi/offline/audio_bus_layout.hpp"
#include "didi/offline/import_options.hpp"
#include "didi/offline/project_file_lock.hpp"
#include "didi/offline/class_reference.hpp"
#include "didi/common/project_path.hpp"
#include "didi/common/atomic_write.hpp"
#include "didi/common/engine_version.hpp"
#include "didi/runtime/session_client.hpp"
#include "didi/runtime/audio_requests.hpp"
#include "didi/tools/editor_copy_refresh.hpp"
#include <chrono>
#include <optional>
#include <thread>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <map>
#include <regex>
#include <sstream>
#include <set>
#include <string>
#include <vector>
#include "didi/mcp/tool_registration.hpp"

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
    // On every answer, so a whole index reads as whole rather than as silent (Q5).
    out["truncated"] = indexer->truncated();
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

// Which of Godot's two numbers an object spells, or null when it is not one.
//
// JSON has one number and Godot has two, and a client that serialises through
// JavaScript cannot send 0.0 at all: JSON.stringify(0.0) is "0". Where a slot
// declares a type the check picks the literal, but inside an untyped Array or
// Dictionary nothing does, so a key meant as 0.0 was written as the int 0. An
// Animation value track takes its type from its keys, so a track whose first
// key is an int moves nothing (#1003). Only the exact two-key shape counts,
// because a Dictionary with an ordinary "type" field is still a Dictionary.
const char* namedScalarType(const json& value) {
    if (!value.is_object() || value.size() != 2 || !value.contains("value")) return nullptr;
    const auto type = value.find("type");
    if (type == value.end() || !type->is_string()) return nullptr;
    if (*type == "float") return "float";
    if (*type == "int") return "int";
    return nullptr;
}

// The literal for a number whose type the caller named. A float always carries
// a fractional part, which is what makes Godot read it as one, and an int is
// refused rather than truncated when it is not whole.
Result<std::string> namedScalarLiteral(const char* type, const json& number,
                                       const std::string& property) {
    const double value = number.get<double>();
    if (std::strcmp(type, "float") == 0) return json(value).dump();
    if (number.is_number_integer()) return number.dump();
    if (!std::isfinite(value) || value != std::floor(value) || std::fabs(value) >= 9.2e18) {
        return Error::invalidArgument("Property \"" + property + "\" declares type int, and " +
                                      number.dump() + " is not a whole number.");
    }
    return json(static_cast<std::int64_t>(value)).dump();
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
    if (const char* scalar = namedScalarType(value)) {
        const auto& number = value.at("value");
        if (!number.is_number()) {
            return Error::invalidArgument("Property \"" + property + "\" declares type " + scalar +
                                          ", which needs its number under \"value\"");
        }
        // A declared slot still rules on the number, with its own refusal.
        if (declared_type && !declaredTypeAccepts(*declared_type, number)) {
            return tresLiteral(number, property, scope, declared_type);
        }
        return namedScalarLiteral(scalar, number, property);
    }
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
            "than lose a property. Check the spelling with script_reflect_class. A "
            "class_name is found when a project script declares it, with no flag. If the "
            "type comes from a GDExtension this engine has not loaded, pass "
            "allow_unknown_type: true.",
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
        // is real -- a GDExtension type is not in the dump either -- so it is
        // named rather than removed. A class_name script is found on its own.
        // A GDExtension type is in the attached engine's ClassDB and in no
        // dump, so an engine that has the class settles the question the
        // reference could not. Its properties still cannot be checked.
        const bool engine_has_type =
            engine_verdict.has_value() && engine_verdict->answered && engine_verdict->known;
        if (!allow_unknown_type && !engine_has_type) {
            return Error(
                400,
                where + ": " + resource_type +
                " is not a class in " + offline::ClassReference::instance().apiVersion() +
                ". Godot cannot load a resource whose type it does not know, so writing "
                "the file would report a resource that does not exist. Check the spelling "
                "with script_reflect_class. A class_name is found when a project script "
                "declares it, with no flag. If the type comes from a GDExtension, which the "
                "shipped class reference cannot see, pass allow_unknown_type: true.",
                json{{"retry_with", {{"allow_unknown_type", true}}}});
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

// The type a .tres header declares, read the way the engine reads it: the
// [gd_resource type="..."] that opens the file. Empty when there is none.
std::string storedResourceType(const std::string& text) {
    const std::string opening = "[gd_resource type=\"";
    if (text.rfind(opening, 0) != 0) return {};
    const auto end = text.find('"', opening.size());
    if (end == std::string::npos) return {};
    return text.substr(opening.size(), end - opening.size());
}

// The script_class="..." on that same header line, which names the class_name
// a scripted resource was saved as. Empty when there is none.
std::string storedScriptClass(const std::string& text) {
    const auto line_end = text.find('\n');
    const auto header = text.substr(0, line_end);
    const std::string key = " script_class=\"";
    const auto start = header.find(key);
    if (start == std::string::npos) return {};
    const auto end = header.find('"', start + key.size());
    if (end == std::string::npos) return {};
    return header.substr(start + key.size(), end - start - key.size());
}

// A type a class_name script declares. ClassDB never lists one, so Godot cannot
// make it from a header that names it: [gd_resource type="ObservedItem"] loads
// as a MissingResource, and a game fails to load it at all. Godot writes such a
// resource as its engine base, names the class in script_class, and sets the
// script, and that is what this is for (#1125).
struct ScriptClass {
    std::string name;
    std::string script_path;
    std::string engine_base;
    // Every top-level var along the script chain, which the loader sets through
    // the script just as it sets an engine property.
    std::set<std::string> members;
};

// The class_name a project script declares, read from the scripts on disk,
// with the chain it extends followed to an engine class. nullopt when no
// script declares the name; an error when one does and its chain cannot be
// followed, or ends on a class that is not a Resource.
Result<std::optional<ScriptClass>> findProjectScriptClass(const std::string& name) {
    static const std::regex class_name_line(R"re(^class_name\s+([A-Za-z_][A-Za-z0-9_]*))re");
    static const std::regex extends_line(
        R"re(^(?:class_name\s+\S+\s+)?extends\s+("[^"]*"|[A-Za-z_][A-Za-z0-9_]*))re");
    static const std::regex member_line(
        R"re(^(?:@[A-Za-z_][A-Za-z0-9_]*(?:\([^)]*\))?\s+)*var\s+([A-Za-z_][A-Za-z0-9_]*))re");
    const auto scan = offline::scanProjectText(".", offline::ScanIndex::fresh);
    std::map<std::string, const std::string*> scripts;
    std::map<std::string, std::string> declared;
    for (const auto& source : scan.sources) {
        if (!strings::endsWith(source.path, ".gd")) continue;
        scripts[source.path] = &source.contents;
        std::istringstream lines(source.contents);
        std::string line;
        while (std::getline(lines, line)) {
            std::smatch match;
            if (std::regex_search(line, match, class_name_line)) {
                declared.emplace(match[1].str(), source.path);
                break;
            }
        }
    }
    const auto found = declared.find(name);
    if (found == declared.end()) return std::optional<ScriptClass>{};

    ScriptClass script_class;
    script_class.name = name;
    script_class.script_path = found->second;
    std::string path = found->second;
    std::set<std::string> visited;
    while (visited.insert(path).second) {
        const auto script = scripts.find(path);
        if (script == scripts.end()) {
            return Error::invalidArgument(name + " is declared by " + script_class.script_path +
                                          ", which extends " + path +
                                          ", and that script is not in this project.");
        }
        std::string extends;
        std::istringstream lines(*script->second);
        std::string line;
        while (std::getline(lines, line)) {
            std::smatch match;
            if (std::regex_search(line, match, member_line)) {
                script_class.members.insert(match[1].str());
            } else if (extends.empty() && std::regex_search(line, match, extends_line)) {
                extends = match[1].str();
            }
        }
        if (!extends.empty() && extends.front() == '"') {
            path = extends.substr(1, extends.size() - 2);
            continue;
        }
        const auto named = declared.find(extends);
        if (named != declared.end()) {
            path = named->second;
            continue;
        }
        // No extends line means RefCounted, which is what Godot assumes.
        script_class.engine_base = extends.empty() ? "RefCounted" : extends;
        break;
    }
    if (script_class.engine_base.empty()) {
        return Error::invalidArgument(name + " is declared by " + script_class.script_path +
                                      ", whose extends chain loops back on itself.");
    }
    const auto& reference = offline::ClassReference::instance();
    std::string ancestor = script_class.engine_base;
    std::set<std::string> walked;
    while (!ancestor.empty() && ancestor != "Resource" && walked.insert(ancestor).second) {
        const json* record = reference.find(ancestor);
        ancestor = record ? record->value("inherits", std::string()) : std::string();
    }
    if (ancestor != "Resource") {
        return Error::invalidArgument(
            name + " is declared by " + script_class.script_path + ", which extends " +
            script_class.engine_base + ". A .tres holds a Resource, and " +
            script_class.engine_base + " is not one. Use a script that extends Resource.");
    }
    return std::optional<ScriptClass>{std::move(script_class)};
}

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

    // A type no engine list names may be a class_name a project script
    // declares. Written as its engine base with the script set, it loads as
    // that class; written under its own name it loaded as a MissingResource
    // while this answered success (#1125).
    const auto engine_named = engine_classes.find(resource_type);
    const bool engine_lacks_type = engine_named != engine_classes.end() && !engine_named->second;
    // A sub-resource's type is looked up the same way, one level down (#1131).
    // Each name is looked up once, since a lookup reads every script in the project.
    std::map<std::string, std::optional<ScriptClass>> script_classes_found;
    const auto projectScriptClass = [&](const std::string& type_name) -> Result<std::optional<ScriptClass>> {
        const auto named = engine_classes.find(type_name);
        if ((named != engine_classes.end() && named->second) ||
            offline::ClassReference::instance().find(type_name)) {
            return std::optional<ScriptClass>{};
        }
        const auto known = script_classes_found.find(type_name);
        if (known != script_classes_found.end()) return known->second;
        auto found = findProjectScriptClass(type_name);
        if (found.isErr()) return found;
        // The script is about to be resolved through the shared index, which
        // may predate it; the search has just read the tree fresh.
        if (found.value()) offline::ResourceIndexer::invalidateSharedIndex();
        script_classes_found.emplace(type_name, found.value());
        return found;
    };
    std::optional<ScriptClass> script_class;
    {
        auto found = projectScriptClass(resource_type);
        if (found.isErr()) return CallToolResult::fromError(found.error(), "Argument 'resource_type': ");
        script_class = std::move(found.value());
    }
    const std::string header_type = script_class ? script_class->engine_base : resource_type;
    std::vector<std::pair<std::string, json>> to_check;
    std::vector<std::pair<std::string, json>> to_write;
    json declared_by_script = json::array();
    if (script_class) {
        to_write.emplace_back("script", json{{"type", "ExtResource"},
                                             {"path", script_class->script_path},
                                             {"resource_type", "Script"}});
    }
    for (const auto& [name, value] : ordered.value()) {
        if (script_class && name == "script") {
            const bool same_script = value.is_object() && value.value("type", "") == "ExtResource" &&
                                     value.value("path", "") == script_class->script_path;
            if (!same_script) {
                return CallToolResult::errorJson(
                    400, "Argument 'properties': " + resource_type + " is the class_name of " +
                             script_class->script_path + ", and that script is set on the resource "
                             "already. Leave script out, or name that script.",
                    {{"code", "invalid_arguments"}, {"field", "properties"}});
            }
            continue;
        }
        to_write.emplace_back(name, value);
        if (script_class && script_class->members.count(name)) {
            declared_by_script.push_back(name);
            continue;
        }
        to_check.emplace_back(name, value);
    }

    // Before anything is rendered or written. A property the type does not
    // declare used to be written into [resource], reported in
    // properties_written, and then dropped by Godot on load with nothing in the
    // surface able to show the loss: resource_inspect reports type, size, uid
    // and dependencies, and no properties.
    auto property_check = checkPropertiesAgainstType(header_type, to_check,
                                                     "Argument 'properties'",
                                                     allow_unknown_type,
                                                     verdictFor(header_type));
    if (property_check.isErr()) return CallToolResult::fromError(property_check.error());
    if (script_class) {
        property_check.value()["script_class"] = script_class->name;
        property_check.value()["script"] = script_class->script_path;
        property_check.value()["declared_by_script"] = std::move(declared_by_script);
    }

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
    std::map<std::string, ScriptClass> sub_script_classes;
    for (const auto& sub : sub_resources) {
        auto sub_class = projectScriptClass(sub.resource_type);
        if (sub_class.isErr()) {
            return CallToolResult::fromError(sub_class.error(),
                                             "sub_resources entry \"" + sub.id + "\": ");
        }
        std::vector<std::pair<std::string, json>> sub_to_check;
        json sub_declared_by_script = json::array();
        for (const auto& [name, value] : sub.properties) {
            if (sub_class.value() && name == "script") {
                const bool same_script = value.is_object() && value.value("type", "") == "ExtResource" &&
                                         value.value("path", "") == sub_class.value()->script_path;
                if (!same_script) {
                    return CallToolResult::errorJson(
                        400, "sub_resources entry \"" + sub.id + "\": " + sub.resource_type +
                                 " is the class_name of " + sub_class.value()->script_path +
                                 ", and that script is set on the sub-resource already. Leave script "
                                 "out, or name that script.",
                        {{"code", "invalid_arguments"}, {"field", "sub_resources"}});
                }
                continue;
            }
            if (sub_class.value() && sub_class.value()->members.count(name)) {
                sub_declared_by_script.push_back(name);
                continue;
            }
            sub_to_check.emplace_back(name, value);
        }
        const std::string sub_header =
            sub_class.value() ? sub_class.value()->engine_base : sub.resource_type;
        auto sub_check = checkPropertiesAgainstType(
            sub_header, sub_to_check,
            "Sub-resource '" + sub.id + "' properties", allow_unknown_type,
            verdictFor(sub_header));
        if (sub_check.isErr()) return CallToolResult::fromError(sub_check.error());
        note_engine(sub_check.value());
        if (sub_class.value()) {
            sub_check.value()["script_class"] = sub_class.value()->name;
            sub_check.value()["script"] = sub_class.value()->script_path;
            sub_check.value()["declared_by_script"] = std::move(sub_declared_by_script);
            sub_script_classes.emplace(sub.id, *sub_class.value());
        }
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
        // .res is Godot's binary resource format, and the loader reads it as
        // binary whatever it holds. Text markup saved as .res is refused with
        // "Unrecognized binary resource file", and the editor's filesystem
        // scan repeats that on every start. Vibe session seventeen found every
        // .res this tool had ever written was one of those, in the console of
        // an editor that had been restarted.
        if (extension == ".res") {
            const std::string as_tres = save_path.substr(0, save_path.size() - 4) + ".tres";
            return CallToolResult::errorJson(
                400,
                "resource_create writes Godot's text resource format, which Godot reads from "
                "a .tres file. A .res file is Godot's binary format, so text written into one "
                "does not load, and the editor reports it as an unrecognized binary resource "
                "file on every start. Use " + as_tres + ".",
                json{{"retry_with", {{"save_path", as_tres}}}});
        }
        if (extension != ".tres") {
            return CallToolResult::errorJson(
                400,
                "resource_create writes Godot text-resource markup, so save_path must end in "
                ".tres; received \"" + save_path +
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
        // A class_name type is written as Godot writes it: its engine base,
        // with the script set before anything the script declares (#1131).
        const auto scripted = sub_script_classes.find(sub.id);
        const ScriptClass* sub_class = scripted == sub_script_classes.end() ? nullptr : &scripted->second;
        const std::string sub_header = sub_class ? sub_class->engine_base : sub.resource_type;
        std::ostringstream block;
        block << "\n[sub_resource type=\"" << sub_header << "\" id=\"" << sub.id << "\"]\n";
        json names = json::array();
        const auto sub_declared = declaredPropertyTypes(sub_header);
        std::vector<std::pair<std::string, json>> sub_to_write;
        if (sub_class) {
            sub_to_write.emplace_back("script", json{{"type", "ExtResource"},
                                                     {"path", sub_class->script_path},
                                                     {"resource_type", "Script"}});
        }
        for (const auto& [name, value] : sub.properties) {
            if (sub_class && name == "script") continue;
            sub_to_write.emplace_back(name, value);
        }
        for (const auto& [name, value] : sub_to_write) {
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
        json written_sub = {{"id", sub.id}, {"resource_type", sub.resource_type},
                            {"properties_written", std::move(names)}};
        if (sub_class) written_sub["engine_type"] = sub_header;
        sub_written.push_back(std::move(written_sub));
    }

    std::ostringstream body;
    body << "\n[resource]\n";
    json written_order = json::array();
    const auto root_declared = declaredPropertyTypes(header_type);
    for (const auto& [name, value] : to_write) {
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
    out << "[gd_resource type=\"" << header_type << "\"";
    if (script_class) out << " script_class=\"" << script_class->name << "\"";
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
        return CallToolResult::fromError(written.error(), "Failed to write resource file to disk: ");
    }
    // What landed, read back from disk, so the answer reports the file rather
    // than the text this call rendered (#1019). The type is the one the header
    // declares, which is what the engine instantiates when it loads the file.
    std::string stored;
    {
        std::ifstream input(target_p, std::ios::binary);
        stored.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }
    const auto stored_type = storedResourceType(stored);
    if (stored_type.empty()) {
        return CallToolResult::errorJson(
            500, "resource_create wrote " + reported_path +
                     " and could not read a resource header back from it, so it cannot say "
                     "what the file holds.");
    }
    // An editor that has this file loaded keeps its own copy, and nothing an
    // unattended editor does re-reads it: not editor_reload_project, not the
    // editor's own filesystem scan. So every live reader went on answering from
    // the old copy after an overwrite -- anim_list_tracks listed a library's old
    // animations, and anim_add_library added the old copy (vibe session
    // seventeen). The caller overwrote the file on purpose, with a token, so the
    // editor's copy is reloaded from it in place, which is what the editor does
    // itself when it notices a change on disk. Absent when no editor answered.
    const auto editor_copy = refreshEditorCopies(ipc, {reported_path});
    // A scripted resource is the class its header names in script_class, which
    // is what the engine makes it; the type beside it is the engine base.
    const auto stored_script_class = storedScriptClass(stored);
    json created = {
        {"status", "created_offline"},
        {"save_path", reported_path},
        {"resource_type", stored_script_class.empty() ? stored_type : stored_script_class},
        {"file_bytes", stored.size()},
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
    };
    if (!stored_script_class.empty()) created["engine_type"] = stored_type;
    // allow_unknown_type past an engine that said it has no such class, and no
    // script declaring it: the file is written as asked, and the answer says
    // what the engine will make of it rather than naming the type as if it
    // would load (#1125).
    if (engine_lacks_type && !script_class) {
        created["limitation"] =
            "The attached engine has no class named " + resource_type +
            " and no project script declares it as a class_name, so the editor loads this file "
            "as a MissingResource that only keeps its data, and a game cannot load it at all.";
    }
    reportEditorCopy(created, editor_copy);
    return CallToolResult::successJson(std::move(created));
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
        json described = found->toJson();
        // An imported asset's import options, read from the sidecar the editor
        // wrote. Whether a track loops is one of them, and nothing else on the
        // surface could say (#958).
        if (auto resolved = paths::resolveProjectFile(resource_path); resolved.isOk()) {
            auto sidecar = resolved.value();
            sidecar += ".import";
            std::error_code error;
            if (std::filesystem::is_regular_file(sidecar, error) && !error) {
                if (auto text = offline::readImportSidecarFile(sidecar); text.isOk()) {
                    described["import"] = offline::describeImportSidecar(text.value());
                } else {
                    // A sidecar that is there and could not be read left no
                    // trace, which read exactly like an asset with no import
                    // options at all.
                    described["import_error"] = text.error().message;
                }
            }
        }
        return CallToolResult::successJson(std::move(described));
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

namespace {

// Whether the layout file the editor saves to is there and read-only. The
// editor's autosave replaces the file through a temporary copy, and on Windows
// that fails on a read-only file with "Safe save failed" in the editor's
// output and nothing in the file: measured on 4.7.2 in vibe session nineteen,
// where audio_add_bus reported that the editor writes the layout "on its own
// schedule" for a save that had already failed. A workspace that checks files
// out read-only until they are edited, as Perforce does, is in this state
// until someone opens the file for edit.
bool layoutFileIsReadOnly(const std::string& layout_path) {
    if (layout_path.rfind("res://", 0) != 0) return false;
    std::error_code error;
    const auto file = std::filesystem::current_path(error) /
                      paths::projectPathFromUtf8(layout_path.substr(6));
    if (error) return false;
    const auto status = std::filesystem::status(file, error);
    if (error || !std::filesystem::is_regular_file(status)) return false;
    return (status.permissions() & std::filesystem::perms::owner_write) ==
           std::filesystem::perms::none;
}

} // namespace

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
        return CallToolResult::notConnected(
            "Godot Editor is offline. Audio bus state lives in the running engine, so launch "
            "Godot to change it. audio_list_buses still reads the project layout offline.");
    }
    auto response = ipc->sendRequest("audio.configureBus", args, ipc::kWaitForDefinitiveResponse);
    if (response.isErr()) {
        return CallToolResult::fromError(response.error(), "Failed to configure the audio bus: ");
    }
    auto payload = response.value();
    if (payload.is_object() && payload.value("persisted_by_editor", false) &&
        layoutFileIsReadOnly(payload.value("layout_path", std::string()))) {
        payload["layout_read_only"] = true;
#ifdef _WIN32
        // Measured on Windows, where the save fails. A POSIX rename can replace
        // a read-only file, so there the flag is reported and the claim kept.
        payload["persisted_by_editor"] = false;
        payload["limitation"] =
            "This sets the running engine's audio bus state. The editor's own bus-layout "
            "autosave would write " + payload.value("layout_path", std::string()) +
            ", but the file is read-only, so the save fails (the editor prints \"Safe save "
            "failed\", which runtime_read_output shows) and the change stays in the running "
            "editor only. Make the file writable, then change any bus and the editor writes "
            "the layout again. revert_with restores the previous values.";
#endif
    }
    return CallToolResult::successJson(std::move(payload));
}

CallToolResult handleAudioAddBus(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    // The rules the bridge applies, checked before anything is sent, so a name
    // is refused in the same words whether or not an editor ever sees it.
    auto parsed = runtime::parseAudioAddBusRequest(args);
    if (parsed.isErr()) return CallToolResult::fromError(parsed.error());
    if (!ipc || !ipc->isConnected()) {
        // Unreachable in practice, as for audio_configure_bus: the registry's
        // live-route check answers first and names audio_list_buses.
        return CallToolResult::notConnected(
            "Godot Editor is offline. A bus is added to the layout the editor holds, which the "
            "editor then writes to the project, so open the project in the editor to add one. "
            "audio_list_buses still reads the project layout offline.");
    }
    auto response = ipc->sendRequest("audio.addBus", args, ipc::kWaitForDefinitiveResponse);
    if (response.isErr()) return CallToolResult::fromError(response.error(), "Failed to add the audio bus: ");
    auto payload = response.value();

    // "Persisted by the editor" is a claim about a file this process can read,
    // so it is read rather than repeated. The editor's autosave wrote the
    // layout 0.8 to 0.9 s after a bus was added on 4.5.1, 4.6.2 and 4.7.2, to
    // the path the project names. A match needs the name at the index the bus
    // was given, so a stale file that already names the bus elsewhere does not
    // count as the write.
    //
    // Five seconds, not two. On a CI runner drawing the editor in software,
    // with every editor frame taking about 0.6 s, the first write of a new
    // layout file on 4.5.1 took longer than two seconds, and the tool reported
    // a bus the editor went on to write as unwritten. The wait ends as soon
    // as the bus is in the file, so the headroom costs nothing when the
    // editor is quick.
    //
    // The file read is the one the bridge names, which is the layout the editor
    // opened the project with, not whatever audio/buses/default_bus_layout says
    // now: an editor keeps writing the file it opened until it restarts.
    const auto name = payload.value("name", std::string());
    const auto index = payload.value("bus", int64_t{-1});
    const auto layout_path = payload.value("layout_path", std::string("res://default_bus_layout.tres"));
    bool written = false;
    // On Windows a read-only file is a save that has already failed, so there
    // is nothing to wait for; elsewhere a rename can still replace it.
#ifdef _WIN32
    const bool wait_for_write = !layoutFileIsReadOnly(layout_path);
#else
    const bool wait_for_write = true;
#endif
    const auto deadline = std::chrono::steady_clock::now() +
                          (wait_for_write ? std::chrono::seconds(5) : std::chrono::seconds(0));
    while (true) {
        auto layout = offline::readAudioBusLayoutFile(".", layout_path);
        if (layout.isOk() && layout.value().value("layout_loads", false)) {
            for (const auto& bus : layout.value().value("buses", json::array())) {
                if (bus.value("index", int64_t{-2}) == index && bus.value("name", std::string()) == name) {
                    written = true;
                }
            }
        }
        if (written || std::chrono::steady_clock::now() >= deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    payload["layout_written"] = written;
    if (!written && layoutFileIsReadOnly(layout_path)) {
        payload["layout_read_only"] = true;
        payload["layout_note"] =
            layout_path + " is read-only, so the editor's autosave cannot replace it: on "
            "Windows it prints \"Safe save failed\", which runtime_read_output shows, and the bus "
            "stays in the running editor only. Make the file writable, where version control "
            "has it locked by checking it out, then change any bus and the editor writes the "
            "layout again.";
    } else if (!written) {
        payload["layout_note"] =
            "The editor had not written this bus to " + layout_path +
            " within five seconds, where it usually takes one. The bus is in the running editor "
            "either way, and a slow editor may still write it. If the save failed, the editor "
            "said why in its output, which runtime_read_output shows.";
    }
    return CallToolResult::successJson(std::move(payload));
}

CallToolResult handleAudioListBuses(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (!args.is_object() || !args.empty()) {
        return CallToolResult::errorJson(400, "Invalid audio request: this tool takes no arguments");
    }
    // Live first, because a bus a script muted at runtime is exactly the case
    // someone is looking for and the layout file cannot show it. The offline
    // read is a fallback, and it says so in the result rather than letting the
    // caller assume they are looking at live state.
    std::optional<Error> live_failure;
    if (ipc && ipc->isConnected()) {
        auto response = ipc->sendRequest("audio.listBuses", args, ipc::kWaitForDefinitiveResponse);
        if (response.isOk()) {
            // The engine's bus list has no cap, so a live answer is whole (Q5).
            auto live = response.value();
            if (live.is_object() && !live.contains("truncated")) live["truncated"] = false;
            return CallToolResult::successJson(std::move(live));
        }
        live_failure = response.error();
    }

    auto layout = offline::readAudioBusLayout(".");
    if (layout.isErr()) return CallToolResult::fromError(layout.error());
    auto payload = layout.value();
    payload["execution_mode"] = "offline_fallback";
    payload["is_live_engine"] = false;
    if (!payload.contains("truncated")) payload["truncated"] = false;
    // An attached engine that failed the read is not the same answer as no
    // engine. Every game session failed it until vibe session nineteen, and the
    // file came back looking like the choice of a caller with nothing attached.
    if (live_failure) {
        payload["live_error"] = {{"code", live_failure->code}, {"message", live_failure->message}};
        payload["live_error_note"] =
            "An engine is attached and its read failed, so this is the project's layout file "
            "rather than the running engine's buses. A bus a script changed at runtime is not "
            "in it.";
    }
    return CallToolResult::successJson(std::move(payload));
}

CallToolResult handleProjectRenameReferences(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (!args.is_object()) {
        return CallToolResult::errorJson(400, "Invalid rename request: arguments must be an object");
    }
    offline::ProjectRenameOptions options;
    if (!args.contains("target") || !args["target"].is_string()) {
        return CallToolResult::errorJson(400, "Invalid rename request: target must be a string");
    }
    if (!args.contains("new_name") || !args["new_name"].is_string()) {
        return CallToolResult::errorJson(400, "Invalid rename request: new_name must be a string");
    }
    options.target = args["target"].get<std::string>();
    options.new_name = args["new_name"].get<std::string>();
    if (args.contains("max_impacts")) {
        const auto& value = args["max_impacts"];
        if (!value.is_number_integer() || value.get<int64_t>() < 1 || value.get<int64_t>() > 5000) {
            return CallToolResult::errorJson(
                400, "Invalid rename request: max_impacts must be an integer from 1 to 5000");
        }
        options.max_impacts = static_cast<size_t>(value.get<int64_t>());
    }
    if (args.contains("discard_unsaved") && !args["discard_unsaved"].is_boolean()) {
        return CallToolResult::errorJson(
            400, "Invalid rename request: discard_unsaved must be a boolean");
    }
    const bool discard_unsaved = args.value("discard_unsaved", false);
    // A scene open in the editor is reloaded from the rewritten file after
    // the write, or the next save puts the old tree back over it. A tab that
    // may hold unsaved changes stops the call here instead, before anything
    // is written, unless the caller said to discard them (#1068).
    options.before_write = [&](const std::vector<std::string>& rewritten) {
        return refuseUnsavedOpenScenes(ipc, rewritten, discard_unsaved);
    };

    auto report = offline::renameReferences(".", options);
    if (report.isErr()) {
        // The conflict and truncation refusals carry the evidence a caller needs
        // to act, so they travel with the message rather than being flattened
        // into it.
        return CallToolResult::fromError(report.error());
    }
    auto payload = report.value();
    payload["execution_mode"] = "offline_fallback";
    std::vector<std::string> rewritten;
    for (const auto& file : payload.value("updated_files", json::array())) {
        if (file.is_object() && file.contains("path") && file["path"].is_string()) {
            rewritten.push_back(file["path"].get<std::string>());
        }
    }
    reportEditorCopies(payload, refreshEditorCopies(ipc, rewritten, discard_unsaved));
    return CallToolResult::successJson(std::move(payload));
}

static std::vector<std::string> proposalResourcePaths(const offline::SpeculativeVerifyRequest& request) {
    // Spelled as the editor spells the file, the way the refresh after the
    // write asks. A file not there yet cannot be open.
    std::vector<std::string> paths;
    for (const auto& change : request.changes) {
        auto resolved = paths::resolveProjectFile(change.path);
        if (resolved.isOk()) paths.push_back(paths::resourcePathOf(resolved.value()));
    }
    return paths;
}

CallToolResult handleProjectVerifyChanges(const json& args) {
    auto parsed = offline::parseSpeculativeVerifyRequest(args);
    if (parsed.isErr()) {
        return CallToolResult::fromError(parsed.error(), "Invalid verification request: ");
    }
    auto verified = offline::verifyChangesInSandbox(parsed.value());
    if (verified.isErr()) {
        return CallToolResult::fromError(verified.error());
    }
    return CallToolResult::successJson(verified.value().toJson());
}

CallToolResult handleProjectApplyChanges(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    // discard_unsaved is this tool's alone; the proposal is the shape
    // project_verify_changes takes, and its parser refuses anything else.
    json proposal = args;
    bool discard_unsaved = false;
    if (proposal.is_object() && proposal.contains("discard_unsaved")) {
        if (!proposal["discard_unsaved"].is_boolean()) {
            return CallToolResult::errorJson(
                400, "Invalid apply request: discard_unsaved must be a boolean");
        }
        discard_unsaved = proposal["discard_unsaved"].get<bool>();
        proposal.erase("discard_unsaved");
    }
    auto parsed = offline::parseSpeculativeVerifyRequest(proposal);
    if (parsed.isErr()) {
        return CallToolResult::fromError(parsed.error(), "Invalid apply request: ");
    }
    // Before the verification run, which can take minutes, and so before
    // anything is written: a scene open in the editor that may hold unsaved
    // changes stops the call, because its tab is reloaded from the new file
    // after the write (#1068).
    if (auto refused = refuseUnsavedOpenScenes(ipc, proposalResourcePaths(parsed.value()),
                                               discard_unsaved)) {
        return CallToolResult::fromError(*refused);
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
    if (applied.value().applied) {
        // The caller's spelling reaches the file, and the editor keys its copy
        // by the file's own, so each is asked for as the readers spell it.
        std::vector<std::string> written;
        for (const auto& path : applied.value().written) {
            auto resolved = paths::resolveProjectFile(path);
            if (resolved.isOk()) written.push_back(paths::resourcePathOf(resolved.value()));
        }
        reportEditorCopies(payload, refreshEditorCopies(ipc, written, discard_unsaved));
        return CallToolResult::successJson(std::move(payload));
    }
    // A proposal the check rejected writes nothing, which is what the tool
    // promises, and the report says why. It is still marked as an error so a
    // caller cannot read it as a success with a footnote, and like every
    // failure it carries an envelope naming what fixes it (Q6). The report
    // stays where it was, beside the envelope, the way a live failure keeps its
    // session beside its own.
    payload["error"] = {
        {"code", 422},
        {"message", "The proposal did not pass verification, so nothing was written. The report "
                    "beside this error says which check failed."},
        {"data", {{"code", "verification_failed"}, {"field", "changes"}, {"retryable", false}}}};
    auto result = CallToolResult::successJson(std::move(payload));
    result.isError = true;
    return result;
}

CallToolResult handleProjectAnalyzeImpact(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    (void)ipc;
    if (!args.is_object()) {
        return CallToolResult::errorJson(400, "Invalid impact request: arguments must be an object");
    }
    offline::ProjectImpactOptions options;
    if (!args.contains("target") || !args["target"].is_string()) {
        return CallToolResult::errorJson(400, "Invalid impact request: target must be a string");
    }
    options.target = args["target"].get<std::string>();
    if (args.contains("max_impacts")) {
        const auto& value = args["max_impacts"];
        if (!value.is_number_integer() || value.get<int64_t>() < 1 || value.get<int64_t>() > 5000) {
            return CallToolResult::errorJson(
                400, "Invalid impact request: max_impacts must be an integer from 1 to 5000");
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
        return CallToolResult::errorJson(400, "Invalid audit request: arguments must be an object");
    }
    offline::ProjectAuditOptions options;
    for (const auto& [key, target] : {std::pair<const char*, bool*>{"include_orphans", &options.include_orphans},
                                      {"include_broken_references", &options.include_broken_references},
                                      {"include_dead_signals", &options.include_dead_signals},
                                      {"include_broken_connections", &options.include_broken_connections},
                                      {"include_import_health", &options.include_import_health},
                                      {"include_addon_orphans", &options.include_addon_orphans}}) {
        if (!args.contains(key)) continue;
        if (!args[key].is_boolean()) {
            return CallToolResult::errorJson(
                400, std::string("Invalid audit request: ") + key + " must be a boolean");
        }
        *target = args[key].get<bool>();
    }
    if (args.contains("max_findings")) {
        const auto& value = args["max_findings"];
        if (!value.is_number_integer() || value.get<int64_t>() < 1 || value.get<int64_t>() > 5000) {
            return CallToolResult::errorJson(
                400, "Invalid audit request: max_findings must be an integer from 1 to 5000");
        }
        options.max_findings = static_cast<size_t>(value.get<int64_t>());
    }
    if (!options.include_orphans && !options.include_broken_references &&
        !options.include_dead_signals && !options.include_broken_connections &&
        !options.include_import_health) {
        return CallToolResult::errorJson(
            400, "Invalid audit request: at least one of include_orphans, "
            "include_broken_references, include_dead_signals, include_broken_connections or "
            "include_import_health must stay enabled");
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
        return CallToolResult::errorJson(400, "Invalid uid map request: arguments must be an object");
    }
    for (const auto& entry : args.items()) {
        if (entry.key() != "resolve") {
            return CallToolResult::errorJson(400, "Invalid uid map request: unknown parameter " + entry.key());
        }
    }
    std::vector<std::string> queries;
    if (args.contains("resolve")) {
        const auto& value = args["resolve"];
        if (!value.is_array() || value.empty() || value.size() > 256) {
            return CallToolResult::errorJson(
                400, "Invalid uid map request: resolve must be an array of 1 to 256 strings");
        }
        for (const auto& item : value) {
            if (!item.is_string() || item.get<std::string>().empty()) {
                return CallToolResult::errorJson(
                    400, "Invalid uid map request: resolve must contain only non-empty strings");
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
        // The same capped index project_list_resources reads, which said so
        // while this did not (Q5).
        {"truncated", indexer->truncated()},
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
        return CallToolResult::errorJson(400, "Parameter 'asset_path' is required.");
    }

    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("asset.instantiate", args);
        if (res.isOk()) {
            return CallToolResult::successJson(res.value());
        }
        return CallToolResult::fromError(res.error(), "Failed to instantiate asset in Godot: ");
    }

    return CallToolResult::notConnected("Godot Editor is offline. Launch Godot Editor to instantiate assets directly into the scene tree.");
}

namespace {

// asset_reimport as a job (Q8 in docs/BUILD_QUEUE.md, #996). The longest wait
// any tool takes, project_export's; and the wait when a job names none, which
// on a software-rendered editor is a scan of a couple of hundred new scripts.
constexpr int64_t kMaxReimportJobMs = 900000;
constexpr int64_t kDefaultReimportJobMs = 300000;
constexpr auto kReimportPollInterval = std::chrono::milliseconds(250);

// A bridge answer that carries its own error, as the failure it is, the way
// the IPC client turns a failed request into one.
Result<json> bridgeAnswer(const json& answer) {
    if (answer.is_object() && answer.contains("error") && answer["error"].is_object()) {
        const auto& error = answer["error"];
        return Error(error.value("code", 500),
                     error.value("message", std::string("The bridge reported a failure")),
                     error.value("data", json()));
    }
    return answer;
}

// Starts a detached reimport and reads it until the bridge has its answer.
//
// The bridge answers asset.reimport at once with an id, and the reimport's own
// answer -- the same one a synchronous call gets -- once the editor has done
// the work, which for a scan of many new scripts is long past the fifteen
// seconds any one command is waited for. asset.reimportStatus is answered off
// the editor's main thread, so it answers while the editor applies the scan.
Result<json> reimportAsJob(const json& args, const std::shared_ptr<ipc::IIpcClient>& ipc) {
    const int64_t timeout_ms = args.value("timeout_ms", kDefaultReimportJobMs);
    // The call's own route, held for the whole job: a session selected
    // meanwhile is an editor that never heard of this reimport.
    const auto lease = runtime::acquireRuntimeRouteLease(ipc);
    if (!lease.has_value()) {
        return Error::notConnected("Godot Editor is offline. Launch Godot to reimport assets.");
    }
    const json request = {{"paths", args["paths"]},
                          // A bridge older than detaching ignores
                          // detach_timeout_ms and answers within this.
                          {"timeout_ms", std::min<int64_t>(timeout_ms, 10000)},
                          {"detach_timeout_ms", timeout_ms}};
    // Started the way a synchronous call is sent, so a failure to start reads
    // and retires the route exactly as it would there.
    auto accepted = ipc->sendRequest("asset.reimport", request, ipc::kWaitForDefinitiveResponse);
    if (accepted.isErr()) return accepted.error();
    const auto& started = accepted.value();
    if (!started.is_object() || !started.contains("reimport_id") || !started["reimport_id"].is_string()) {
        // Answered in full, by a bridge that ran it the old way.
        return started;
    }
    const auto id = started["reimport_id"].get<std::string>();
    const auto named = [&id](Result<json> answer) -> Result<json> {
        if (answer.isOk()) {
            if (answer.value().is_object()) answer.value()["reimport_id"] = id;
            return answer;
        }
        auto error = answer.error();
        if (error.data.is_null()) error.data = json::object();
        if (error.data.is_object()) error.data["reimport_id"] = id;
        return error;
    };
    // The bridge answers on its own deadline. This one is for a bridge that
    // stopped answering the reads altogether.
    const auto give_up = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms) +
                         std::chrono::seconds(60);
    std::optional<Error> last_failure;
    while (!cancellationRequested()) {
        std::this_thread::sleep_for(kReimportPollInterval);
        // Straight down the lease, so a read that waited behind another
        // command is asked again rather than taken for a hung engine.
        auto read = runtime::sendLiveRouteRequest(*lease, "asset.reimportStatus",
                                                  {{"reimport_id", id}},
                                                  ipc::kWaitForDefinitiveResponse, true);
        if (read.response.isErr()) {
            // A read lost on the way says nothing about the reimport, so the
            // next one asks again. A refusal from the bridge itself is the
            // answer: an editor that restarted keeps none of its reimports.
            if (!ipc::transportFailureState(read.response.error()).has_value()) {
                return named(read.response.error());
            }
            last_failure = read.response.error();
        } else if (read.response.value().value("state", std::string()) == "finished") {
            return named(bridgeAnswer(read.response.value().value("answer", json::object())));
        }
        if (std::chrono::steady_clock::now() >= give_up) {
            return Error(504,
                         "The editor stopped answering reads of reimport " + id +
                             " before it had an answer, so whether the assets were reimported is "
                             "unknown." +
                             (last_failure.has_value() ? " The last read failed: " + last_failure->message
                                                       : std::string()),
                         {{"code", "reimport_status_unanswered"},
                          {"outcome", "unknown_outcome"},
                          {"reimport_id", id},
                          {"retryable", true}});
        }
    }
    // The job keeps no answer once cancelled. The editor's work carries on,
    // and the bridge stops waiting on it when nobody reads it.
    return Error(409, "The reimport job was cancelled before it had an answer.",
                 {{"code", "job_cancelled"}, {"reimport_id", id}, {"retryable", false}});
}

}  // namespace

CallToolResult handleAssetReimport(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (!args.is_object() || !args.contains("paths") || !args["paths"].is_array() ||
        args["paths"].empty() || args["paths"].size() > 256) {
        return CallToolResult::errorJson(400, "Invalid asset reimport request: paths must be an array of 1 to 256 strings");
    }
    std::set<std::string> unique;
    for (const auto& value : args["paths"]) {
        if (!value.is_string()) {
            return CallToolResult::errorJson(400, "Invalid asset reimport request: paths must contain only strings");
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
            return CallToolResult::errorJson(400, "Invalid asset reimport request: every path must be a normalized project-owned res:// source asset");
        }
        if (!unique.insert(path).second) {
            return CallToolResult::errorJson(400, "Invalid asset reimport request: paths must be unique");
        }
    }
    // Ten seconds is what fits inside the bridge's fifteen-second wait for any
    // one command. A job waits on a thread of its own, so its reimport answers
    // at once with an id and is read until it finishes (Q8, #996).
    const bool as_job = runningAsJob();
    const int64_t max_timeout_ms = as_job ? kMaxReimportJobMs : 10000;
    if (args.contains("timeout_ms") &&
        (!args["timeout_ms"].is_number_integer() || args["timeout_ms"].get<int64_t>() < 1 ||
         args["timeout_ms"].get<int64_t>() > max_timeout_ms)) {
        if (!as_job && args["timeout_ms"].is_number_integer() &&
            args["timeout_ms"].get<int64_t>() > 10000 &&
            args["timeout_ms"].get<int64_t>() <= kMaxReimportJobMs) {
            return CallToolResult::errorJson(
                400,
                "timeout_ms above 10000 needs request_id. Without one the call is answered within "
                "the bridge's wait for a single command; with one it runs as a job, which waits "
                "up to 900000.",
                {{"code", "reimport_needs_job"}, {"field", "request_id"}});
        }
        return CallToolResult::errorJson(
            400, "Invalid asset reimport request: timeout_ms must be an integer from 1 to " +
                     std::to_string(max_timeout_ms));
    }
    if (!ipc || !ipc->isConnected()) {
        return CallToolResult::notConnected("Godot Editor is offline. Launch Godot to reimport assets.");
    }
    if (as_job) {
        auto answered = reimportAsJob(args, ipc);
        offline::ResourceIndexer::invalidateSharedIndex();
        if (answered.isErr()) {
            return CallToolResult::fromError(answered.error(), "Failed to reimport assets: ");
        }
        return CallToolResult::successJson(answered.value());
    }
    auto response = ipc->sendRequest("asset.reimport", args, ipc::kWaitForDefinitiveResponse);
    // A reimport rewrites .import sidecars and can change uids, so the shared
    // scan is no longer trustworthy whether the call succeeded or not.
    offline::ResourceIndexer::invalidateSharedIndex();
    if (response.isErr()) {
        auto error = response.error();
        // The editor carries on with the work after a timeout, so the advice
        // is to wait it out as a job, not to send the same call again (#1159).
        if (error.data.is_object() && error.data.value("code", std::string()) == "reimport_idle_timeout") {
            error.message += ". The editor is still working on it, so do not send another reimport "
                             "now; give the next call a request_id, which runs it as a job that "
                             "waits up to 900000 ms";
        }
        return CallToolResult::fromError(error, "Failed to reimport assets: ");
    }
    return CallToolResult::successJson(response.value());
}

namespace {

// A bridge answer that is a refusal, whichever way it arrived: as a failed
// request, or as an answer carrying an error object.
std::optional<Error> bridgeFailure(const Result<json>& response) {
    if (response.isErr()) return response.error();
    const auto& payload = response.value();
    if (payload.is_object() && payload.contains("error") && payload["error"].is_object()) {
        const auto& error = payload["error"];
        return Error(error.value("code", 500),
                     error.value("message", std::string("The editor refused the request")),
                     error.contains("data") ? error["data"] : json());
    }
    return std::nullopt;
}

// Every line the engine printed across the calls one answer is made of.
void gatherDiagnostics(json& into, const Result<json>& response) {
    if (response.isErr() || !response.value().is_object()) return;
    const auto& payload = response.value();
    const json* lines = nullptr;
    if (payload.contains("engine_diagnostics")) lines = &payload["engine_diagnostics"];
    else if (payload.contains("error") && payload["error"].is_object() &&
             payload["error"].value("data", json::object()).contains("engine_diagnostics")) {
        lines = &payload["error"]["data"]["engine_diagnostics"];
    }
    if (!lines || !lines->is_array()) return;
    for (const auto& line : *lines) into.push_back(line);
}

std::optional<offline::ImportedStreamFacts> streamFacts(const json& stream) {
    if (!stream.is_object()) return std::nullopt;
    offline::ImportedStreamFacts facts;
    if (stream.contains("length_seconds") && stream["length_seconds"].is_number()) {
        facts.length_seconds = stream["length_seconds"].get<double>();
    }
    const auto properties = stream.value("properties", json::object());
    if (properties.is_object() && properties.contains("mix_rate") &&
        properties["mix_rate"].is_number_integer()) {
        facts.mix_rate = properties["mix_rate"].get<int64_t>();
    }
    return facts;
}

// What the preview and the call both work from: the asset as it is spelled on
// disk, its sidecar as Godot's parser reads it, and the lines that would
// change, checked against the stream when an editor can say what it is.
struct ImportPlan {
    std::filesystem::path root;
    std::filesystem::path sidecar_path;
    std::string asset_path;
    std::string lock_name;
    json options;
    offline::ImportSidecar sidecar;
    std::vector<offline::ImportOptionChange> changes;
    json stream;
};

Result<ImportPlan> planImport(const json& args, const std::shared_ptr<ipc::IIpcClient>& ipc) {
    auto request = offline::parseImportConfigureRequest(args);
    if (request.isErr()) return request.error();
    const auto refuse = [](int code, const std::string& message, json data) {
        data["parameter"] = "asset_path";
        if (!data.contains("retryable")) data["retryable"] = false;
        return Error(code, message, std::move(data));
    };
    ImportPlan plan;
    plan.options = request.value().options;
    std::error_code error;
    plan.root = std::filesystem::weakly_canonical(std::filesystem::current_path(), error);
    if (error) return Error::internal("The project root cannot be resolved");

    auto resolved = paths::resolveProjectFile(request.value().asset_path);
    if (resolved.isErr()) {
        return refuse(resolved.error().code == 404 ? 404 : 400,
                      "asset_path " + request.value().asset_path + ": " + resolved.error().message + ".",
                      json{{"code", resolved.error().code == 404 ? "not_found" : "invalid_arguments"}});
    }
    // The spelling on disk, because a wrong letter case opens on Windows and
    // names a file an export will not find, as anim_add_library found.
    std::error_code canonical_error;
    const auto on_disk = std::filesystem::canonical(resolved.value(), canonical_error);
    const auto asset = canonical_error ? resolved.value() : on_disk;
    plan.asset_path = paths::resourcePathOf(asset);
    std::string requested = request.value().asset_path;
    if (!strings::startsWith(requested, "res://")) requested = "res://" + requested;
    const auto folded = [](std::string text) {
        std::transform(text.begin(), text.end(), text.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    };
    if (requested != plan.asset_path && folded(requested) == folded(plan.asset_path)) {
        return refuse(400,
                      "asset_path is spelled " + requested + " and the file is " + plan.asset_path +
                          ". Windows opened it anyway, and an exported game would not.",
                      json{{"code", "invalid_arguments"},
                           {"retry_with", {{"asset_path", plan.asset_path}}}});
    }
    plan.sidecar_path = asset;
    plan.sidecar_path += ".import";
    if (!std::filesystem::is_regular_file(plan.sidecar_path, error) || error) {
        return refuse(404,
                      plan.asset_path + " has no .import sidecar, so the editor has not imported "
                      "it. asset_reimport imports an asset the editor has not seen; call it first.",
                      json{{"code", "no_import_metadata"}});
    }
    plan.lock_name = plan.asset_path.substr(6) + ".import";
    auto text = offline::readImportSidecarFile(plan.sidecar_path);
    if (text.isErr()) return text.error();
    auto sidecar = offline::readImportSidecar(text.value());
    if (sidecar.isErr()) return sidecar.error();
    plan.sidecar = std::move(sidecar.value());

    // Everything that needs no engine is checked first, so a texture or a key
    // the importer does not have is refused without asking the editor.
    auto checked = offline::planImportChanges(plan.sidecar, plan.options, std::nullopt);
    if (checked.isErr()) return checked.error();
    plan.changes = std::move(checked.value());
    if (ipc && ipc->isConnected()) {
        auto read = ipc->sendRequest("asset.readImportedStream", json{{"path", plan.asset_path}}, 10000);
        if (auto failed = bridgeFailure(read)) return *failed;
        plan.stream = read.value();
        auto bounded = offline::planImportChanges(plan.sidecar, plan.options, streamFacts(plan.stream));
        if (bounded.isErr()) return bounded.error();
        plan.changes = std::move(bounded.value());
    }
    return plan;
}

json changesAsValues(const std::vector<offline::ImportOptionChange>& changes, bool previous) {
    json values = json::object();
    for (const auto& change : changes) values[change.key] = previous ? change.previous : change.value;
    return values;
}

json streamSummary(const json& stream) {
    if (!stream.is_object()) return json();
    json summary = {{"class", stream.value("class", std::string())},
                    {"properties", stream.value("properties", json::object())}};
    if (stream.contains("length_seconds")) summary["length_seconds"] = stream["length_seconds"];
    return summary;
}

} // namespace

Result<json> previewAssetConfigureImport(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    auto planned = planImport(args, ipc);
    if (planned.isErr()) return planned.error();
    const auto& plan = planned.value();
    json before = {{"asset_path", plan.asset_path},
                   {"importer", plan.sidecar.importer},
                   {"uid", plan.sidecar.uid},
                   {"options", changesAsValues(plan.changes, true)},
                   {"planned_options", changesAsValues(plan.changes, false)}};
    if (plan.stream.is_object()) {
        before["stream"] = streamSummary(plan.stream);
    } else {
        // With no editor the bounds that depend on the track were not checked,
        // and the preview says so rather than implying they passed.
        before["stream_read"] = false;
    }
    return json{{"before", std::move(before)}, {"subject", {{"asset_path", plan.asset_path}}}};
}

// Changes an imported asset's import options, reimports it, and proves the
// change held (#958). The rules and the measurements behind them are in the
// asset_configure_import amendment in docs/SURFACE_AMENDMENTS.md.
CallToolResult handleAssetConfigureImport(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (!ipc || !ipc->isConnected()) {
        // Unreachable in practice: the registry's live-route check answers
        // first and names resource_inspect.
        return CallToolResult::notConnected(
            "Godot Editor is offline. An import option is changed, reimported and checked by the "
            "editor, so open the project in the editor to change one. resource_inspect reads an "
            "asset's import options offline.");
    }
    auto planned = planImport(args, ipc);
    if (planned.isErr()) return CallToolResult::fromError(planned.error());
    auto plan = std::move(planned.value());

    // Held from the read that plans the edit to the write, so another writer's
    // change to the same sidecar is either wholly before this one or wholly
    // after it (#929).
    std::string previous_text;
    {
        auto lock = offline::lockProjectFile(plan.root, plan.lock_name);
        if (lock.isErr()) return CallToolResult::fromError(lock.error());
        auto text = offline::readImportSidecarFile(plan.sidecar_path);
        if (text.isErr()) return CallToolResult::fromError(text.error());
        auto fresh = offline::readImportSidecar(text.value());
        if (fresh.isErr()) return CallToolResult::fromError(fresh.error());
        auto changes = offline::planImportChanges(fresh.value(), plan.options, streamFacts(plan.stream));
        if (changes.isErr()) return CallToolResult::fromError(changes.error());
        auto edited = offline::applyImportChanges(fresh.value(), changes.value());
        if (edited.isErr()) return CallToolResult::fromError(edited.error());
        previous_text = fresh.value().text;
        plan.sidecar = std::move(fresh.value());
        plan.changes = std::move(changes.value());
        auto written = files::writeFileAtomically(plan.sidecar_path, edited.value());
        if (written.isErr()) {
            return CallToolResult::fromError(written.error(), "Failed to write the .import sidecar: ");
        }
    }

    json diagnostics = json::array();
    const json reimport_request = {{"paths", json::array({plan.asset_path})}};
    // Puts the sidecar back as it was and imports it again, so a change that
    // did not hold leaves the asset as it found it.
    const auto restore = [&]() {
        {
            auto lock = offline::lockProjectFile(plan.root, plan.lock_name);
            if (lock.isErr()) return false;
            if (files::writeFileAtomically(plan.sidecar_path, previous_text).isErr()) return false;
        }
        auto again = ipc->sendRequest("asset.reimport", reimport_request, ipc::kWaitForDefinitiveResponse);
        offline::ResourceIndexer::invalidateSharedIndex();
        gatherDiagnostics(diagnostics, again);
        return !bridgeFailure(again).has_value();
    };

    auto reimported = ipc->sendRequest("asset.reimport", reimport_request, ipc::kWaitForDefinitiveResponse);
    offline::ResourceIndexer::invalidateSharedIndex();
    gatherDiagnostics(diagnostics, reimported);
    if (auto failed = bridgeFailure(reimported)) {
        const bool restored = restore();
        json data = failed->data.is_object() ? failed->data : json::object();
        data["asset_path"] = plan.asset_path;
        data["rolled_back"] = restored;
        data["engine_diagnostics"] = diagnostics;
        return CallToolResult::fromError(
            Error(failed->code,
                  "The options were written and the reimport failed: " + failed->message +
                      (restored ? " The previous sidecar is back and reimported."
                                : " Putting the previous sidecar back failed too."),
                  data));
    }

    // Checked against the file and the stream rather than taken from the
    // reimport's answer, because the engine refuses nothing: it stores a value
    // it cannot use and loads it without a word.
    std::vector<std::string> divergences;
    auto after_text = offline::readImportSidecarFile(plan.sidecar_path);
    auto after = after_text.isOk() ? offline::readImportSidecar(after_text.value())
                                   : Result<offline::ImportSidecar>(after_text.error());
    if (after.isErr()) {
        divergences.push_back("the sidecar does not read back after the reimport: " + after.error().message);
    } else {
        for (auto& line : offline::sidecarDivergences(after.value(), plan.changes, plan.sidecar.uid)) {
            divergences.push_back(std::move(line));
        }
    }
    auto stream_after = ipc->sendRequest("asset.readImportedStream", json{{"path", plan.asset_path}}, 10000);
    gatherDiagnostics(diagnostics, stream_after);
    if (auto failed = bridgeFailure(stream_after)) {
        divergences.push_back("the stream does not load after the reimport: " + failed->message);
    } else {
        for (auto& line : offline::streamDivergences(stream_after.value(), plan.changes)) {
            divergences.push_back(std::move(line));
        }
    }
    if (!divergences.empty()) {
        const bool restored = restore();
        std::string summary;
        for (size_t i = 0; i < divergences.size(); ++i) summary += (i ? "; " : "") + divergences[i];
        return CallToolResult::errorJson(
            422,
            "The import options did not hold after the reimport: " + summary + "." +
                (restored ? " The previous sidecar is back and reimported."
                          : " Putting the previous sidecar back failed too."),
            {{"code", "import_change_not_held"},
             {"asset_path", plan.asset_path},
             {"divergences", divergences},
             {"rolled_back", restored},
             {"engine_diagnostics", diagnostics},
             {"retryable", false}});
    }

    json payload = {
        {"status", "configured"},
        {"asset_path", plan.asset_path},
        {"importer", plan.sidecar.importer},
        {"uid", plan.sidecar.uid},
        {"previous", changesAsValues(plan.changes, true)},
        {"options", changesAsValues(plan.changes, false)},
        {"reimported", true},
        {"verified", true},
        {"stream", streamSummary(stream_after.value())},
        {"undo_redo_registered", false},
        {"way_back", "asset_configure_import on the same asset with options set to previous"},
        {"engine_diagnostics", diagnostics},
        {"execution_mode", "live"}};
    return CallToolResult::successJson(std::move(payload));
}

namespace {

using namespace output_schema;

json projectListResourcesOutputSchema() {
    return object_schema(
        {{"execution_mode", string_type},
         {"resources", array_of(object_schema({{"path", string_type},
                                               {"filename", string_type},
                                               {"type", string_type},
                                               {"uid", string_type},
                                               {"file_size", integer_type},
                                               {"dependencies", {{"type", "array"}}}},
                                              {"path"}))},
         {"total_found", integer_type},
         {"truncated", boolean_type}},
        {"execution_mode", "resources"});
}

json resourceInspectOutputSchema() {
    return object_schema({{"path", string_type},
                          {"filename", string_type},
                          {"type", string_type},
                          {"uid", string_type},
                          {"file_size", integer_type},
                          {"dependencies", {{"type", "array"}}},
                          // The import sidecar's options (#958), or why a
                          // sidecar that is there could not be read.
                          {"import", {{"type", "object"}}},
                          {"import_error", string_type}},
                         {"execution_mode", "path"});
}

}  // namespace

// The tools whose handlers this file holds. registerAllDefaultTools calls
// each domain's in turn (#1256).
void ToolRegistry::registerAssetTools() {
    {
        ToolDefinition t;
        t.name = "asset_reimport";
        t.description = "Reimports project source assets and waits for the editor to go idle. A batch with a new file scans first and answers once the scan is applied, so a new script's class is registered. A refused import names the failed paths; the engine's reason is under engine_diagnostics.";
        // Above 10000 only as a job (Q8): the handler refuses it otherwise.
        t.inputSchema = {{"type", "object"}, {"properties", {
            {"paths", {{"type", "array"}, {"minItems", 1}, {"maxItems", 256}, {"uniqueItems", true},
                       {"items", {{"type", "string"}, {"minLength", 7}, {"maxLength", 1024}}}}},
            {"timeout_ms", {{"type", "integer"}, {"default", 10000}, {"minimum", 1}, {"maximum", 900000}}},
            {"request_id", {{"type", "string"}, {"minLength", 8}}}
        }}, {"required", {"paths"}}};
        t.handler = [this](const json& args) { return handleAssetReimport(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "asset_configure_import";
        t.description =
            "Changes an imported asset's import options in its .import file, reimports it in the "
            "attached editor, and checks what the engine then loads. This is how a music track "
            "is made to loop: loop: true for an OGG or MP3, edit/loop_mode 2 (Forward) for a "
            "WAV. It sets the loop options of WAV, OGG and MP3 imports only, and refuses any "
            "other importer or option. Godot checks none of these values, so the tool does: a "
            "value of the wrong type, a loop mode Godot has no name for, an offset or a loop "
            "window outside the track. If the reimported asset does not load with what was "
            "asked, the previous file is put back and reimported. resource_inspect reports an "
            "asset's current import options.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"asset_path", {{"type", "string"}, {"minLength", 1}, {"maxLength", 1024}}},
                {"options", {
                    {"type", "object"},
                    {"properties", {
                        {"loop", {{"type", "boolean"},
                                  {"description", "OGG and MP3: whether the track loops."}}},
                        {"loop_offset", {{"type", "number"}, {"minimum", 0},
                                         {"description", "OGG and MP3: the second the track loops back "
                                                         "to, from 0 up to its length."}}},
                        {"edit/loop_mode", {{"type", json::array({"integer", "string"})},
                                            {"description",
                                             "WAV: 0 Detect From WAV, 1 Disabled, 2 Forward, 3 "
                                             "Ping-Pong, 4 Backward, as the number or the name."}}},
                        {"edit/loop_begin", {{"type", "integer"}, {"minimum", 0},
                                             {"description", "WAV: the first frame of the loop, "
                                                             "under loop modes 2 to 4."}}},
                        {"edit/loop_end", {{"type", "integer"}, {"minimum", -1},
                                           {"description", "WAV: the frame the loop ends at, or -1 "
                                                           "for the last one, under loop modes 2 to 4."}}}
                    }}
                }}
            }},
            {"required", json::array({"asset_path", "options"})},
            {"additionalProperties", false}
        };
        t.handler = [this](const json& args) { return handleAssetConfigureImport(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "project_verify_changes";
        t.description = "Checks a set of proposed file contents together in an isolated copy of the project, so a script that preloads a sibling sees the proposed sibling, and nothing reaches the working tree whether the proposal is good or not.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"changes", {
                    {"type", "array"}, {"minItems", 1}, {"maxItems", 64},
                    {"description", "The proposal. Each file is written into the isolated copy before anything is checked, so the set is checked as a set."},
                    {"items", {
                        {"type", "object"},
                        {"properties", {
                            {"path", {{"type", "string"}, {"description", "A res:// path inside the project. The same containment rules script_create applies."}}},
                            {"content", {{"type", "string"}, {"description", "The whole proposed contents of that file, at most 1 MiB."}}}
                        }},
                        {"required", json::array({"path", "content"})},
                        {"additionalProperties", false}
                    }}
                }},
                {"run_scene", {{"type", "string"},
                               {"description", "Optional. A .tscn or .scn inside the project to open in the copy once the proposal is written, so the check is more than a parse. The copy has no import cache, so the first run also imports what the scene touches."}}},
                {"run_frames", {{"type", "integer"}, {"minimum", 1}, {"maximum", 6000}, {"default", 120},
                                {"description", "Iterations to let the scene run before Godot quits by itself. Only meaningful with run_scene."}}},
                {"timeout_seconds", {{"type", "integer"}, {"minimum", 1}, {"maximum", 600}, {"default", 120}}}
            }},
            {"required", json::array({"changes"})},
            {"additionalProperties", false}
        };
        t.handler = [](const json& args) { return handleProjectVerifyChanges(args); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "project_apply_changes";
        t.description = "Checks a proposal in an isolated copy of the project and, only if it passes, writes it into the working tree in one staged pass. A proposal that does not pass is reported and nothing is written.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"changes", {
                    {"type", "array"}, {"minItems", 1}, {"maxItems", 64},
                    {"description", "The proposal. The same shape project_verify_changes takes, checked the same way before any of it reaches the working tree."},
                    {"items", {
                        {"type", "object"},
                        {"properties", {
                            {"path", {{"type", "string"}, {"description", "A res:// path inside the project. The same containment rules script_create applies."}}},
                            {"content", {{"type", "string"}, {"description", "The whole proposed contents of that file, at most 1 MiB."}}}
                        }},
                        {"required", json::array({"path", "content"})},
                        {"additionalProperties", false}
                    }}
                }},
                {"run_scene", {{"type", "string"},
                               {"description", "Optional. A .tscn or .scn to open in the copy before deciding, so a scene that fails to load stops the write."}}},
                {"run_frames", {{"type", "integer"}, {"minimum", 1}, {"maximum", 6000}, {"default", 120},
                                {"description", "Iterations to let the scene run before Godot quits by itself. Only meaningful with run_scene."}}},
                {"timeout_seconds", {{"type", "integer"}, {"minimum", 1}, {"maximum", 600}, {"default", 120}}},
                {"discard_unsaved", {{"type", "boolean"},
                                     {"description", "Allow reloading an open tab that may hold unsaved edits."}}}
            }},
            {"required", json::array({"changes"})},
            {"additionalProperties", false}
        };
        t.handler = [this](const json& args) { return handleProjectApplyChanges(args, m_sourceIpcClient); };
        registerTool(std::move(t));
    }

    {
        ToolDefinition t;
        t.name = "resource_create";
        t.description = "Writes textual .tres content under the project root. Every value is rendered as a Godot literal or the call is refused naming the property, so a resource is never reported as written when part of it was thrown away. With an editor attached, the editor's copy of the file is reloaded from what was written.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"resource_type", {{"type", "string"}, {"default", "StandardMaterial3D"}}},
                {"save_path", {{"type", "string"}, {"description", "Target res:// path ending in .tres. A .res is Godot's binary format and is refused."}}},
                {"properties", {
                    {"type", json::array({"object", "array"})},
                    {"description",
                     "An object, whose keys are written in sorted order, or an array of "
                     "{name, value} entries written in the order given. Use the array when order "
                     "matters: Godot applies indexed sub-properties in file order, so tracks/0/type "
                     "has to come before the rest of track 0. A value is a string, number, boolean, "
                     "array, or an object. An object with x/y, x/y/z, x/y/z/w or r/g/b(/a) numbers "
                     "becomes whichever Vector or Color the property is declared as, and the "
                     "matching one by shape when the class reference does not carry the property; "
                     "any other object is a Dictionary. So tile_size on a TileSet takes {x, y} and "
                     "is written Vector2i, and a fractional component in an integer vector is "
                     "refused rather than truncated. To "
                     "choose the type yourself, give the object a \"type\": Vector2i, Vector3i, "
                     "Vector4i, Quaternion and Color take their components; NodePath and StringName "
                     "take their text under \"value\"; the packed arrays take their elements under "
                     "\"values\", and the composite ones -- PackedVector2Array, PackedVector3Array, "
                     "PackedVector4Array and PackedColorArray -- take either an element per entry or "
                     "the components already flattened, which is Godot's own format. The declared "
                     "type also decides what kind of value the slot takes: int a whole number, float "
                     "a number, bool true or false, String, StringName and NodePath a string, Array "
                     "and the packed arrays an array, and Color either components or a \"#rrggbbaa\" "
                     "string. Anything else is refused, because Godot keeps the property's default "
                     "for a value it cannot convert and says nothing. A property can also point at "
                     "another resource: {\"type\": "
                     "\"ExtResource\", \"path\": \"res://art/tiles.png\"} references a file in "
                     "the project, and {\"type\": \"SubResource\", \"id\": \"Atlas_1\"} "
                     "references an entry of sub_resources declared above it. A type this cannot "
                     "write is refused rather than guessed at."},
                    {"oneOf", json::array({
                        json{{"type", "object"}},
                        json{{"type", "array"},
                             {"items", {{"type", "object"},
                                        {"properties", {{"name", {{"type", "string"},
                                                                  {"minLength", 1}}},
                                                        {"value", json::object()}}},
                                        {"required", json::array({"name", "value"})}}}}
                    })}
                }},
                {"sub_resources", {
                    {"description",
                     "The [sub_resource] blocks this resource carries inside itself, in the order "
                     "they should appear. Each entry is {id, resource_type, properties}, and its "
                     "properties follow exactly the same rules as the top-level ones. A property "
                     "anywhere in the file names one with {\"type\": \"SubResource\", \"id\": "
                     "...}, and only an id declared earlier can be named, because Godot resolves a "
                     "SubResource against what it has already read. load_steps is computed from "
                     "these and the external references; do not pass it."},
                    {"type", "array"},
                    {"maxItems", 64},
                    {"items", {{"type", "object"},
                               {"properties", {{"id", {{"type", "string"}, {"minLength", 1}}},
                                               {"resource_type", {{"type", "string"}, {"minLength", 1}}},
                                               {"properties", json::object()}}},
                               {"required", json::array({"id", "resource_type"})}}}
                }},
                {"overwrite", {{"type", "boolean"}, {"default", false}}},
                {"allow_unknown_type", {{"type", "boolean"}, {"default", false},
                                        {"description",
                                         "Write a resource_type that no engine list and no project "
                                         "class_name names. Off by default, because Godot cannot "
                                         "load a resource whose type it does not know and a "
                                         "misspelling is the common case. Turn it on for a "
                                         "GDExtension type with no session attached; an attached "
                                         "engine's ClassDB already lists one. The result then reports "
                                         "property_check as unchecked, or "
                                         "type_unknown_to_attached_engine and a limitation when the "
                                         "engine answered and does not have the type."}}}
            }},
            {"required", {"save_path"}}
        };
        // The source client, not the lease dispatch wrapper. This tool sends no
        // request; it reads the selected session descriptor to say whether the
        // pinned class reference its property_check used matches the attached
        // engine, and the wrapper is not a session client, so the two fields were
        // never emitted (#735). Same reason script_reflect_class takes it.
        t.handler = [this](const json& args) { return handleResourceCreate(args, m_sourceIpcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "resource_inspect";
        t.description = "Returns offline indexed file metadata, detected type, UID, and parsed dependencies for a resource path.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"resource_path", {{"type", "string"}}}
            }},
            {"required", {"resource_path"}}
        };
        t.handler = [this](const json& args) { return handleResourceInspect(args, m_ipcClient); };
        t.outputSchema = resourceInspectOutputSchema();
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "project_list_resources";
        t.description = "Scans res:// for assets filtered by type (e.g., .glb, .png, .tres).";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"search_path", {{"type", "string"}, {"default", "res://"}}},
                {"type_filter", {{"type", "string"}}},
                {"fuzzy_query", {{"type", "string"}}},
                {"include_uid", {{"type", "boolean"}, {"default", true}}}
            }}
        };
        t.handler = [this](const json& args) { return handleQueryProjectResources(args, m_ipcClient); };
        t.outputSchema = projectListResourcesOutputSchema();
        registerTool(t);

        // Alias
        t.name = "query_project_resources";
        t.handler = [this](const json& args) { return handleQueryProjectResources(args, m_ipcClient); };
        registerTool(t);
    }
    {
        ToolDefinition t;
        t.name = "project_get_uid_map";
        t.description = "Resolves uid:// and res:// references to each other, and returns the UID map scanned from the project files.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"resolve", {{"type", "array"},
                             {"items", {{"type", "string"}}},
                             {"minItems", 1}, {"maxItems", 256},
                             {"description", "uid:// or res:// values to resolve. A connected editor answers them from ResourceUID and each result reports whether the project files agree (index_state). Omit to get the scanned map alone."}}}
            }},
            {"additionalProperties", false}
        };
        t.handler = [this](const json& args) { return handleProjectGetUidMap(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "project_audit_assets";
        t.description = "Audits the project for unreferenced assets, references that resolve to nothing, declared signals nothing uses, scene connections to methods nothing declares, and unhealthy Godot import metadata. Reports evidence, not verdicts. A file scan in every case; a connected editor additionally verifies unresolved uid:// findings against ResourceUID and clears the ones it disproves.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"include_orphans", {{"type", "boolean"}, {"default", true},
                                     {"description", "Assets that nothing references. Asset types only: scenes and scripts are excluded because nothing has to reference the level you open by hand."}}},
                {"include_broken_references", {{"type", "boolean"}, {"default", true},
                                               {"description", "res:// paths and uid:// references that resolve to no file in the project."}}},
                {"include_dead_signals", {{"type", "boolean"}, {"default", true},
                                          {"description", "Signals declared in GDScript that no file emits, connects to, or wires in a scene."}}},
                {"include_broken_connections", {{"type", "boolean"}, {"default", true},
                                                {"description", "Scene [connection] entries whose method the receiving node's script, the scripts it extends and its engine class do not declare. Judged only where every step resolves."}}},
                {"include_import_health", {{"type", "boolean"}, {"default", true},
                                           {"description", "Existing Godot .import metadata with missing sources or outputs, malformed/unsafe paths, or source files newer than their outputs."}}},
                {"include_addon_orphans", {{"type", "boolean"}, {"default", false},
                                           {"description", "Count files under res://addons/ as orphans. Off by default: addons are third-party code a developer did not write and is not responsible for tidying. excluded_addon_orphans reports how many were left out."}}},
                {"max_findings", {{"type", "integer"}, {"minimum", 1}, {"maximum", 5000}, {"default", 500}}}
            }},
            {"additionalProperties", false}
        };
        t.handler = [this](const json& args) { return handleProjectAuditAssets(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "project_analyze_impact";
        t.description = "Traces every place a symbol, signal, resource path, or static node path is named, including scene connections and animation tracks that a text search finds but cannot explain.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"target", {{"type", "string"}, {"minLength", 1}, {"maxLength", 256},
                            {"description", "A canonical res:// path, lowercase-alphanumeric uid:// value, static Godot node path such as Player/Sprite, /root, or Hand/Sword/%Hilt, or a single Godot identifier such as a variable, function, or signal name."}}},
                {"max_impacts", {{"type", "integer"}, {"minimum", 1}, {"maximum", 5000}, {"default", 500}}}
            }},
            {"required", json::array({"target"})},
            {"additionalProperties", false}
        };
        t.handler = [this](const json& args) { return handleProjectAnalyzeImpact(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "project_rename_references";
        t.description = "Renames a symbol in the scene connections and animation tracks that serialize it, atomically across every file, and reports the references it deliberately does not touch, the project.godot [autoload] key among them.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"target", {{"type", "string"}, {"minLength", 1}, {"maxLength", 256},
                            {"description", "The Godot identifier to rename: a variable, function or signal name. A res:// path or a node path is a different operation and is refused."}}},
                {"new_name", {{"type", "string"}, {"minLength", 1}, {"maxLength", 256},
                              {"description", "The identifier to rename it to. Refused if a scene connection or animation track already uses it, because that would merge two different symbols."}}},
                {"max_impacts", {{"type", "integer"}, {"minimum", 1}, {"maximum", 5000}, {"default", 500}}},
                {"discard_unsaved", {{"type", "boolean"},
                                     {"description", "Allow reloading an open tab that may hold unsaved edits."}}}
            }},
            {"required", json::array({"target", "new_name"})},
            {"additionalProperties", false}
        };
        t.handler = [this](const json& args) { return handleProjectRenameReferences(args, m_sourceIpcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "audio_list_buses";
        t.description = "Lists the audio buses with volume, mute, solo, bypass, routing, and effect chains. Live when the editor is attached, from the project bus layout otherwise.";
        t.inputSchema = {{"type", "object"}, {"properties", json::object()},
                         {"additionalProperties", false}};
        t.handler = [this](const json& args) { return handleAudioListBuses(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "audio_configure_bus";
        t.description = "Sets an audio bus volume, mute or solo on the running engine, and returns the values it replaced.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                // The type beside the oneOf is for a host that reads only
                // `type`, which would send index 1 as "1", a bus name. Claude
                // Code reads the oneOf too and sent the integer without it.
                {"bus", {{"type", json::array({"string", "integer"})},
                         {"description", "The bus name or its index."},
                         {"oneOf", json::array({json{{"type", "string"}, {"minLength", 1}, {"maxLength", 128}},
                                                json{{"type", "integer"}, {"minimum", 0}}})}}},
                {"volume_db", {{"type", "number"}, {"minimum", -80}, {"maximum", 24}}},
                {"mute", {{"type", "boolean"}}},
                {"solo", {{"type", "boolean"}}}
            }},
            {"required", json::array({"bus"})},
            {"additionalProperties", false}
        };
        t.handler = [this](const json& args) { return handleAudioConfigureBus(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "audio_add_bus";
        t.description =
            "Adds one audio bus to the end of the layout in the attached editor, names it and "
            "routes it, so a game can have the Music and SFX buses a settings menu sets; a fresh "
            "project has only Master. It only adds: a name in use, or one differing only in "
            "letter case, is refused, and so is a send to a bus that does not exist, which Godot "
            "would silently route to Master. The editor writes the project's bus layout file "
            "itself shortly afterwards, and the result says whether it has. Add the bus before "
            "setting an AudioStreamPlayer's bus to it.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"name", {{"type", "string"}, {"minLength", 1}, {"maxLength", 256}}},
                {"send", {{"type", "string"}, {"minLength", 1}, {"maxLength", 256},
                          {"default", "Master"}}},
                {"volume_db", {{"type", "number"}, {"minimum", -80}, {"maximum", 24}}},
                {"mute", {{"type", "boolean"}}},
                {"solo", {{"type", "boolean"}}}
            }},
            {"required", json::array({"name"})},
            {"additionalProperties", false}
        };
        t.handler = [this](const json& args) { return handleAudioAddBus(args, m_ipcClient); };
        // The name, send and value rules need no engine, so a caller with no
        // editor open hears about a bad name now rather than after opening one.
        t.argumentCheck = [](const json& args) -> std::optional<Error> {
            auto parsed = runtime::parseAudioAddBusRequest(args);
            if (parsed.isErr()) return parsed.error();
            return std::nullopt;
        };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "instantiate_asset";
        t.description = "Creates an instance of a resource or scene and parents it with automatic collision assignment.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"asset_path", {{"type", "string"}}},
                {"parent_path", {{"type", "string"}, {"default", "/root"}}},
                {"transform", {{"type", "object"}}},
                {"collision_mode", {{"type", "string"}, {"default", "none"}}}
            }},
            {"required", {"asset_path"}}
        };
        t.handler = [this](const json& args) { return handleInstantiateAsset(args, m_ipcClient); };
        registerTool(std::move(t));
    }
}

} // namespace mcp
} // namespace didi
