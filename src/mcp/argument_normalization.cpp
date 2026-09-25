#include "didi/mcp/argument_normalization.hpp"
#include "didi/mcp/schema_validation.hpp"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>

namespace didi::mcp {
namespace {
constexpr std::size_t kMaxNodes = 4096;
constexpr std::size_t kMaxBytes = 65536;
constexpr unsigned kMaxDepth = 8;
constexpr std::int64_t kExactDoubleInteger = 9007199254740992LL;

Error refusal(std::string field, const char* reason, const char* expected) {
    // All strings originate in the fixed policy below, never in caller input.
    return Error(400, "Arguments could not be normalized using safe-v1.",
                 {{"code", "invalid_arguments"}, {"retryable", false},
                  {"field", std::move(field)}, {"reason", reason},
                  {"expected", expected}});
}

struct Budget {
    std::size_t nodes = kMaxNodes;
    std::size_t bytes = kMaxBytes;
    bool charge(std::size_t count) {
        if (count > bytes) return false;
        bytes -= count;
        return true;
    }
    bool text(std::size_t count) {
        // Conservative serialized size, including worst-case JSON escaping.
        if (!charge(2) || count > bytes / 6) return false;
        return charge(count * 6);
    }
    bool visit(const json& value, unsigned depth = 0) {
        if (depth > kMaxDepth || nodes == 0) return false;
        --nodes;
        if (!charge(2)) return false;
        if (value.is_string()) return text(value.get_ref<const std::string&>().size());
        if (value.is_number_float() && !std::isfinite(value.get<double>())) return false;
        if (value.is_primitive()) return !value.is_binary() && !value.is_discarded() && charge(32);
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (value.is_object() && !text(it.key().size())) return false;
            if (!visit(it.value(), depth + 1)) return false;
        }
        return true;
    }
};

std::optional<Error> integer(json& value, const std::string& path) {
    if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        std::int64_t parsed = 0;
        const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
        if (text.empty() || result.ec != std::errc{} || result.ptr != text.data() + text.size())
            return refusal(path, "expected_integer", "complete signed 64-bit decimal integer");
        value = parsed;
    }
    if (!value.is_number_integer() ||
        (value.is_number_unsigned() && value.get<std::uint64_t>() >
         static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())))
        return refusal(path, "expected_integer", "signed 64-bit integer");
    return std::nullopt;
}

std::optional<Error> component(json& value, const std::string& path) {
    double parsed = 0;
    if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed,
                                             std::chars_format::general);
        if (text.empty() || result.ec != std::errc{} || result.ptr != text.data() + text.size())
            return refusal(path, "expected_finite_number", "complete finite decimal number");
        // Keep strings strictly inside the exact-integer interval. A decimal
        // integer just outside it can round *to* 2^53, including exponent
        // spellings, so checking only > 2^53 after parsing is insufficient.
        if (std::isfinite(parsed) && std::abs(parsed) >= static_cast<double>(kExactDoubleInteger))
            return refusal(path, "inexact_integer", "numeric string with magnitude below 2^53");
    } else if (value.is_number()) {
        // JSON integers outside the exact binary64 interval must not be rounded.
        if ((value.is_number_unsigned() && value.get<std::uint64_t>() > kExactDoubleInteger) ||
            (value.is_number_integer() && !value.is_number_unsigned() &&
             (value.get<std::int64_t>() < -kExactDoubleInteger || value.get<std::int64_t>() > kExactDoubleInteger)))
            return refusal(path, "inexact_integer", "integer within [-2^53, 2^53]");
        parsed = value.get<double>();
    } else {
        return refusal(path, "expected_finite_number", "finite number");
    }
    if (!std::isfinite(parsed)) return refusal(path, "expected_finite_number", "finite number");
    value = parsed;
    return std::nullopt;
}
} // namespace

bool argumentNormalizationEnabled() {
#ifdef DIDI_ELASTIC_INGRESS
    return true;
#else
    return false;
#endif
}

bool supportsArgumentNormalization(std::string_view tool) {
    return tool == "scene_get_hierarchy" || tool == "ui_list_controls" || tool == "ui_hit_test";
}

Result<json> normalizeToolArguments(std::string_view tool, const json& schema, const json& arguments) {
    if (!supportsArgumentNormalization(tool))
        return refusal("/name", "unsupported_tool", "an advertised read-only normalization tool");
    Budget budget;
    if (!budget.visit(arguments))
        return refusal("/arguments", "input_budget_exceeded", "depth <= 8, nodes <= 4096, conservative JSON bytes <= 65536");
    if (!arguments.is_object()) return refusal("/arguments", "expected_object", "object");

    json normalized = arguments;
    const auto normalize_integer = [&](const char* name) -> std::optional<Error> {
        auto value = normalized.find(name);
        if (value == normalized.end()) return std::nullopt;
        return integer(*value, std::string("/arguments/") + name);
    };
    if (tool == "scene_get_hierarchy") {
        for (const char* name : {"max_depth", "max_nodes"}) {
            if (auto error = normalize_integer(name)) return *error;
        }
    } else {
        if (auto error = normalize_integer("max_results")) return *error;
    }
    if (tool == "ui_hit_test" && normalized.contains("point")) {
        auto& point = normalized["point"];
        if (point.is_array()) {
            if (point.size() != 2) return refusal("/arguments/point", "invalid_vector_shape", "exactly two components");
            for (std::size_t i = 0; i < 2; ++i) {
                if (auto error = component(point[i], "/arguments/point/" + std::to_string(i))) return *error;
            }
            point = json{{"x", point[0]}, {"y", point[1]}};
        } else if (point.is_object() && point.size() == 2 && point.contains("x") && point.contains("y")) {
            for (const char* name : {"x", "y"}) {
                if (auto error = component(point[name], std::string("/arguments/point/") + name)) return *error;
            }
        } else {
            return refusal("/arguments/point", "invalid_vector_shape", "[x,y] or an object containing only x and y");
        }
    }
    // Keep the existing validator authoritative. Its prose may contain supplied
    // keys or values; use a fixed diagnostic instead of echoing those values.
    if (validateAgainstSchema(schema, normalized))
        return refusal("/arguments", "schema_validation_failed", "the tool's published inputSchema");
    return normalized;
}
} // namespace didi::mcp
