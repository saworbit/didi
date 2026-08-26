#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include "types.hpp"
#include "json.hpp"

namespace didi {
namespace ipc {

// Default named pipe name for Windows / domain socket for Unix
#if defined(_WIN32)
inline const char* kDefaultPipeName = "\\\\.\\pipe\\godot_didi_ipc";
#else
inline const char* kDefaultPipeName = "/tmp/godot_didi_ipc.sock";
#endif

// Framing helper: 4-byte length prefix (little-endian) + JSON payload
inline std::vector<uint8_t> frameMessage(const json& message) {
    std::string serialized = message.dump();
    uint32_t len = static_cast<uint32_t>(serialized.size());
    std::vector<uint8_t> buffer(4 + len);
    buffer[0] = static_cast<uint8_t>(len & 0xFF);
    buffer[1] = static_cast<uint8_t>((len >> 8) & 0xFF);
    buffer[2] = static_cast<uint8_t>((len >> 16) & 0xFF);
    buffer[3] = static_cast<uint8_t>((len >> 24) & 0xFF);
    std::memcpy(buffer.data() + 4, serialized.data(), len);
    return buffer;
}

inline std::optional<json> parseFramedMessage(const uint8_t* data, size_t size, size_t& bytes_consumed) {
    if (size < 4) {
        bytes_consumed = 0;
        return std::nullopt;
    }
    uint32_t len = static_cast<uint32_t>(data[0]) |
                   (static_cast<uint32_t>(data[1]) << 8) |
                   (static_cast<uint32_t>(data[2]) << 16) |
                   (static_cast<uint32_t>(data[3]) << 24);
    if (size < 4 + len) {
        bytes_consumed = 0;
        return std::nullopt;
    }

    std::string str(reinterpret_cast<const char*>(data + 4), len);
    bytes_consumed = 4 + len;
    try {
        return json::parse(str);
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace ipc
} // namespace didi
