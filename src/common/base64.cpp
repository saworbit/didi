#include "didi/common/base64.hpp"

namespace didi {
namespace base64 {

static const char kEncodeTable[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string encode(const uint8_t* data, size_t length) {
    std::string out;
    out.reserve(((length + 2) / 3) * 4);

    size_t i = 0;
    while (i < length) {
        size_t remaining = length - i;
        uint32_t a = data[i++];
        uint32_t b = (remaining > 1) ? data[i++] : 0;
        uint32_t c = (remaining > 2) ? data[i++] : 0;

        uint32_t triple = (a << 16) | (b << 8) | c;

        out.push_back(kEncodeTable[(triple >> 18) & 0x3F]);
        out.push_back(kEncodeTable[(triple >> 12) & 0x3F]);
        out.push_back(remaining > 1 ? kEncodeTable[(triple >> 6) & 0x3F] : '=');
        out.push_back(remaining > 2 ? kEncodeTable[triple & 0x3F] : '=');
    }

    return out;
}

std::string encode(std::string_view data) {
    return encode(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

std::string encode(const std::vector<uint8_t>& data) {
    return encode(data.data(), data.size());
}

std::vector<uint8_t> decode(std::string_view input) {
    std::vector<uint8_t> out;
    if (input.empty()) return out;

    std::vector<int> T(256, -1);
    for (int i = 0; i < 64; i++) T[static_cast<uint8_t>(kEncodeTable[i])] = i;

    int val = 0, valb = -8;
    for (char c : input) {
        if (c == '=') break;
        if (T[static_cast<uint8_t>(c)] == -1) continue;
        val = (val << 6) + T[static_cast<uint8_t>(c)];
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<uint8_t>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

} // namespace base64
} // namespace didi
