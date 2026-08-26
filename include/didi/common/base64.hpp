#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <cstdint>

namespace didi {
namespace base64 {

std::string encode(const uint8_t* data, size_t length);
std::string encode(std::string_view data);
std::string encode(const std::vector<uint8_t>& data);

std::vector<uint8_t> decode(std::string_view input);

} // namespace base64
} // namespace didi
