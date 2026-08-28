#pragma once

#include "didi/common/types.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace didi::security {

Result<std::vector<uint8_t>> secureRandomBytes(size_t count);
Result<std::string> secureRandomHex(size_t byte_count);

} // namespace didi::security
