#include "didi/mcp/schema_validation.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace didi {
namespace mcp {

namespace {

// The published schemas are one or two levels deep. A bound keeps a
// pathological schema from walking the stack down.
constexpr int kMaxDepth = 8;

std::string typeNameOf(const json& value) {
    if (value.is_null()) return "null";
    if (value.is_boolean()) return "a boolean";
    if (value.is_number_integer()) return "an integer";
    if (value.is_number()) return "a number";
    if (value.is_string()) return "a string";
    if (value.is_array()) return "an array";
    return "an object";
}

std::string articled(std::string_view type) {
    if (type == "integer" || type == "array" || type == "object") {
        return "an " + std::string(type);
    }
    if (type == "null") return "null";
    return "a " + std::string(type);
}

bool matchesType(std::string_view type, const json& value) {
    if (type == "string") return value.is_string();
    if (type == "integer") return value.is_number_integer();
    if (type == "number") return value.is_number();
    if (type == "boolean") return value.is_boolean();
    if (type == "array") return value.is_array();
    if (type == "object") return value.is_object();
    if (type == "null") return value.is_null();
    // An unknown type keyword is not a reason to refuse a call.
    return true;
}

std::string joinNames(const std::vector<std::string>& names) {
    std::string joined;
    for (size_t i = 0; i < names.size(); ++i) {
        if (i > 0) joined += ", ";
        joined += names[i];
    }
    return joined;
}

std::string quoted(const std::string& name) {
    return "'" + name + "'";
}

std::optional<std::string> checkValue(const json& schema, const json& value,
                                      const std::string& where, int depth);

std::optional<std::string> checkType(const json& schema, const json& value,
                                     const std::string& where) {
    const auto type = schema.find("type");
    if (type == schema.end()) return std::nullopt;
    if (type->is_string()) {
        const auto wanted = type->get<std::string>();
        if (matchesType(wanted, value)) return std::nullopt;
        return where + " must be " + articled(wanted) + ", not " + typeNameOf(value) + ".";
    }
    if (type->is_array() && !type->empty()) {
        std::vector<std::string> wanted;
        for (const auto& entry : *type) {
            if (!entry.is_string()) continue;
            if (matchesType(entry.get<std::string>(), value)) return std::nullopt;
            wanted.push_back(entry.get<std::string>());
        }
        if (wanted.empty()) return std::nullopt;
        return where + " must be one of these types: " + joinNames(wanted) + ". It is " +
               typeNameOf(value) + ".";
    }
    return std::nullopt;
}

std::optional<std::string> checkBounds(const json& schema, const json& value,
                                       const std::string& where) {
    if (value.is_string()) {
        const auto length = value.get_ref<const std::string&>().size();
        if (schema.contains("minLength") && schema["minLength"].is_number_integer() &&
            length < schema["minLength"].get<size_t>()) {
            return where + " must be at least " + schema["minLength"].dump() +
                   " characters long.";
        }
        if (schema.contains("maxLength") && schema["maxLength"].is_number_integer() &&
            length > schema["maxLength"].get<size_t>()) {
            return where + " must be at most " + schema["maxLength"].dump() +
                   " characters long.";
        }
    }
    if (value.is_number()) {
        const double number = value.get<double>();
        if (schema.contains("minimum") && schema["minimum"].is_number() &&
            number < schema["minimum"].get<double>()) {
            return where + " must be at least " + schema["minimum"].dump() + ".";
        }
        if (schema.contains("maximum") && schema["maximum"].is_number() &&
            number > schema["maximum"].get<double>()) {
            return where + " must be at most " + schema["maximum"].dump() + ".";
        }
    }
    if (value.is_array()) {
        if (schema.contains("minItems") && schema["minItems"].is_number_integer() &&
            value.size() < schema["minItems"].get<size_t>()) {
            return where + " must have at least " + schema["minItems"].dump() + " entries.";
        }
        if (schema.contains("maxItems") && schema["maxItems"].is_number_integer() &&
            value.size() > schema["maxItems"].get<size_t>()) {
            return where + " must have at most " + schema["maxItems"].dump() + " entries.";
        }
    }
    return std::nullopt;
}

std::optional<std::string> checkObject(const json& schema, const json& value,
                                       const std::string& where, int depth) {
    const bool named = !where.empty();
    const auto properties = schema.find("properties");
    const bool has_properties = properties != schema.end() && properties->is_object();

    if (schema.contains("required") && schema["required"].is_array()) {
        for (const auto& field : schema["required"]) {
            if (!field.is_string()) continue;
            const auto name = field.get<std::string>();
            if (value.contains(name)) continue;
            return named ? where + " is missing required property " + quoted(name) + "."
                         : "Missing required argument " + quoted(name) + ".";
        }
    }

    const auto additional = schema.find("additionalProperties");
    const bool closed = additional != schema.end() && additional->is_boolean() &&
                        !additional->get<bool>();
    if (closed && has_properties) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (properties->contains(it.key())) continue;
            std::vector<std::string> accepted;
            for (auto known = properties->begin(); known != properties->end(); ++known) {
                accepted.push_back(known.key());
            }
            std::sort(accepted.begin(), accepted.end());
            const std::string list = accepted.empty()
                                         ? "It takes no arguments."
                                         : "This tool accepts: " + joinNames(accepted) + ".";
            return named ? where + " has an unknown property " + quoted(it.key()) + "."
                         : "Unknown argument " + quoted(it.key()) + ". " + list;
        }
    }

    if (!has_properties || depth >= kMaxDepth) return std::nullopt;
    for (auto it = value.begin(); it != value.end(); ++it) {
        const auto declared = properties->find(it.key());
        if (declared == properties->end() || !declared->is_object()) continue;
        const std::string child =
            named ? where + "." + it.key() : "Argument " + quoted(it.key());
        if (auto problem = checkValue(*declared, it.value(), child, depth + 1)) {
            return problem;
        }
    }
    return std::nullopt;
}

std::optional<std::string> checkValue(const json& schema, const json& value,
                                      const std::string& where, int depth) {
    if (!schema.is_object()) return std::nullopt;
    if (auto problem = checkType(schema, value, where)) return problem;

    const auto allowed = schema.find("enum");
    if (allowed != schema.end() && allowed->is_array() && !allowed->empty()) {
        const bool found = std::any_of(allowed->begin(), allowed->end(),
                                       [&](const json& option) { return option == value; });
        if (!found) {
            return where + " must be one of: " + allowed->dump() + ".";
        }
    }

    if (auto problem = checkBounds(schema, value, where)) return problem;

    if (value.is_array() && depth < kMaxDepth) {
        const auto items = schema.find("items");
        if (items != schema.end() && items->is_object()) {
            for (size_t index = 0; index < value.size(); ++index) {
                const std::string child = where + " entry " + std::to_string(index);
                if (auto problem = checkValue(*items, value[index], child, depth + 1)) {
                    return problem;
                }
            }
        }
    }

    if (value.is_object()) return checkObject(schema, value, where, depth);
    return std::nullopt;
}

} // namespace

std::optional<std::string> validateAgainstSchema(const json& schema, const json& arguments) {
    if (!schema.is_object() || !arguments.is_object()) return std::nullopt;
    return checkObject(schema, arguments, "", 0);
}

} // namespace mcp
} // namespace didi
