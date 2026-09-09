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

    // The safety property, asserted on every input rather than only on the
    // ones a human thought of: a caller must never be advanced past the end of
    // the buffer it owns.
    if (consumed > size) {
        __builtin_trap();
    }

    // A message that decoded must have occupied some bytes.
    if (message.has_value() && consumed == 0) {
        __builtin_trap();
    }

    // Deliberately not asserted: that returning no message implies consuming
    // nothing. The first version of this target claimed exactly that and the
    // fuzzer rejected it on the first run, using a seed committed alongside
    // it -- four zero bytes, a complete header describing an empty payload.
    // The decoder sets bytes_consumed before it tries to parse, so a frame
    // whose payload is not JSON reports the bytes it occupied and then returns
    // nothing.
    //
    // That is a defensible contract rather than a defect: it lets a caller
    // skip a malformed frame instead of re-reading it forever, and the two
    // "no message" cases stay distinguishable because the incomplete ones set
    // bytes_consumed to zero explicitly. It is worth knowing that the
    // distinction is load-bearing and undocumented, though -- a caller that
    // treats every nullopt as "wait for more data" will spin on a bad payload.
    // There are no production callers today; the shipping IPC paths have their
    // own reader.
    return 0;
}
