#pragma once

// types.hpp, not json.hpp: the alias these helpers are written against is
// didi::json, so including the vendored header alone leaves this file unable to
// compile when it is the first include in a translation unit.
#include "didi/common/types.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>

namespace didi {

// nlohmann::json reports unsigned values as integers too, and get<int64_t>()
// narrows them with a cast. Read the unsigned representation first so values
// above INT64_MAX cannot wrap into valid negative coordinates or sentinels.
inline std::optional<int64_t> jsonInt64(const json& value) {
    if (value.is_number_unsigned()) {
        const auto number = value.get<uint64_t>();
        if (number > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            return std::nullopt;
        }
        return static_cast<int64_t>(number);
    }
    if (!value.is_number_integer()) return std::nullopt;
    return value.get<int64_t>();
}

inline std::optional<int64_t> boundedJsonInteger(const json& value,
                                                 int64_t minimum,
                                                 int64_t maximum) {
    const auto number = jsonInt64(value);
    if (!number.has_value() || *number < minimum || *number > maximum) {
        return std::nullopt;
    }
    return number;
}

// What a single tool response may serialize to, and the headroom left for the
// envelope the registry stamps around it.
//
// One figure for every reader that composes a bounded answer, so the two levers
// stay distinguishable: a caller who wants a small answer asks for one, with
// max_symbols or max_nodes or max_results. This is the other lever, the one for
// not losing the whole response. 8 MiB is the figure scene_get_hierarchy
// already published, and the readers that grew a cap later use it too rather
// than each picking their own (#574, #575).
constexpr size_t kMaxToolResponseBytes = 8 * 1024 * 1024;
constexpr size_t kToolResponseEnvelopeReserveBytes = 256 * 1024;

// The budget the body of a response has, once the envelope is allowed for.
constexpr size_t toolResponseBodyBudget() {
    return kMaxToolResponseBytes - kToolResponseEnvelopeReserveBytes;
}

// Replaces every string longer than `threshold` with a note saying how many
// bytes stood there, and reports how many it replaced.
//
// An elision that says nothing is worse than the size it saves: the preview is
// the artifact a person approves, so a value that was dropped has to say it was
// dropped and how big it was. Recurses into arrays and objects, because the
// oversized value is as likely to be one entry of a path list as a top-level
// argument.
inline size_t elideLargeStrings(json& value, size_t threshold) {
    size_t elided = 0;
    if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        if (text.size() > threshold) {
            value = "[elided: " + std::to_string(text.size()) + " bytes]";
            ++elided;
        }
        return elided;
    }
    if (value.is_array()) {
        for (auto& entry : value) elided += elideLargeStrings(entry, threshold);
        return elided;
    }
    if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            elided += elideLargeStrings(it.value(), threshold);
        }
    }
    return elided;
}

} // namespace didi
