#pragma once

#include "didi/common/json.hpp"

#include <cstdint>
#include <limits>
#include <optional>

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

} // namespace didi
