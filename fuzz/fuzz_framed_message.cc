// Fuzzes the IPC frame decoder: a length prefix an attacker controls, followed
// by a payload that may or may not be there.
//
// This target exists because reading this function closely, while choosing what
// to fuzz, found a buffer over-read in it: `size < 4 + len` was evaluated in
// 32-bit unsigned arithmetic, so a length near UINT32_MAX wrapped to a small
// number, the guard passed, and a four-gigabyte std::string was constructed
// from an eight-byte buffer. An eight-byte input segfaulted. That is fixed, and
// the input that found it is in this target's seed corpus so it can never come
// back quietly.
//
// The only test this function had round-tripped a frame the same code had just
// written -- the one input shape guaranteed not to find that.

#include <cstddef>
#include <cstdint>
#include <optional>

#include "didi/common/protocol.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    size_t consumed = 0;
    auto message = didi::ipc::parseFramedMessage(data, size, consumed);

    // The contract, asserted on every input rather than only on the ones a
    // human thought of: a decoder that reports progress must have made some,
    // and it can never claim to have consumed more than it was given. Either
    // would walk a caller off the end of its own buffer.
    if (message.has_value()) {
        if (consumed == 0 || consumed > size) {
            __builtin_trap();
        }
    } else if (consumed != 0) {
        // Nothing decoded means nothing consumed; otherwise a caller advances
        // past bytes that were never accepted.
        __builtin_trap();
    }
    return 0;
}
