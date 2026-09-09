// Fuzzes base64 decoding, which runs on every captured frame and every
// resource payload that arrives encoded.
//
// The decoder is permissive by construction: it skips characters outside the
// alphabet rather than rejecting them, and stops at the first '='. That is a
// reasonable choice for a transport that has already been authenticated, but
// it means the decoder never refuses anything, so its behaviour on hostile
// input is decided entirely by its arithmetic rather than by any validation.
// The bit accumulator is an int shifted left six at a time, which is the kind
// of loop worth pointing a fuzzer at.

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "didi/common/base64.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string_view input(reinterpret_cast<const char*>(data), size);
    const std::vector<uint8_t> decoded = didi::base64::decode(input);

    // Four input characters carry at most three output bytes, and characters
    // outside the alphabet contribute none. Exceeding that means the decoder
    // invented data, which downstream code would then treat as image or
    // resource content.
    if (decoded.size() > (size / 4 + 1) * 3) {
        __builtin_trap();
    }

    // Round trip in the direction that must hold: whatever the decoder chose
    // to accept, re-encoding it and decoding again must be stable. A decoder
    // whose output depends on where it started is one that can be walked into
    // disagreeing with itself across a retry.
    const std::string reencoded = didi::base64::encode(decoded);
    const std::vector<uint8_t> again = didi::base64::decode(reencoded);
    if (again != decoded) {
        __builtin_trap();
    }
    return 0;
}
