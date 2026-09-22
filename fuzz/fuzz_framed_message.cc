// Fuzzes the IPC frame reader: a four-byte length prefix an attacker controls,
// followed by a payload that arrives in pieces, or late, or not at all.
//
// This target used to drive `didi::ipc::parseFramedMessage`, a decoder that
// took a flat buffer. It found a real buffer over-read there -- the bounds
// check `size < 4 + len` was evaluated in 32-bit unsigned arithmetic, so a
// length near UINT32_MAX wrapped to a small number, the guard passed, and a
// four-gigabyte std::string was constructed from an eight-byte buffer. An
// eight-byte input segfaulted. It was fixed in #344 and the seed that found it
// is still in this target's corpus.
//
// What that decoder was not, by the time it was being fuzzed, was the one the
// server uses. Nothing in src/ or addons/ called it: the shipping transports
// had their own reader, and SECURITY.md's claim that the decoder reading bytes
// Didi did not write is fuzzed named the wrong function (#882). There is now
// one reader, `didi::ipc::readFramePayload`, both transports call it at both
// ends, and this target drives it.
//
// The shape it is driven with matters. A server does not get a flat buffer; it
// gets a length and then a socket that hands over as much as it feels like.
// The reader below therefore serves the payload in slices the fuzzer chooses,
// including none at all, which is the slowloris case the growth policy exists
// for.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "didi/common/protocol.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < didi::ipc::kFrameLengthPrefixBytes) {
        return 0;
    }

    const uint8_t header[didi::ipc::kFrameLengthPrefixBytes] = {data[0], data[1], data[2], data[3]};
    const uint32_t claimed = didi::ipc::decodeFrameLength(header);

    // Whatever follows the header is what "arrived". The fuzzer controls both
    // the claim and the arrival, which is the pair that matters: the interesting
    // inputs are the ones where they disagree.
    const uint8_t* arrived = data + didi::ipc::kFrameLengthPrefixBytes;
    const size_t available = size - didi::ipc::kFrameLengthPrefixBytes;

    size_t served = 0;
    size_t largest_request = 0;
    std::vector<char> payload;

    const auto outcome = didi::ipc::readFramePayload(
        claimed, didi::ipc::kMaximumFrameBytes, payload,
        [&](char* destination, uint32_t bytes) {
            if (bytes > largest_request) largest_request = bytes;
            // A short read is a failed read: the production callers either fill
            // the whole request or report failure, and this must not be more
            // generous than they are.
            if (available - served < bytes) return false;
            for (uint32_t index = 0; index < bytes; ++index) {
                destination[index] = static_cast<char>(arrived[served + index]);
            }
            served += bytes;
            return true;
        });

    // Never asked for more than one chunk at a time. This is the property the
    // growth policy exists for: a peer may claim 128 MiB, and a claim buys a
    // read rather than a buffer.
    if (largest_request > didi::ipc::kFrameReadChunkBytes) {
        __builtin_trap();
    }

    // Never committed more than the bytes that justified it, plus the chunk it
    // was in the middle of asking for, plus the doubling. Doubling is
    // deliberate -- growing 128 MiB one chunk at a time would reallocate two
    // thousand times -- and it is what makes the bound 2x rather than exact.
    // The point of the bound is the same either way: a claim of 128 MiB that
    // never arrives peaks at 128 KiB, not at the claim.
    const size_t committed_ceiling = 2 * (served + didi::ipc::kFrameReadChunkBytes);
    if (payload.capacity() > committed_ceiling) {
        __builtin_trap();
    }

    if (outcome == didi::ipc::FrameReadOutcome::completed) {
        // A completed frame is exactly the claim, and every byte of it came
        // from the reader.
        if (payload.size() != claimed || served != claimed) {
            __builtin_trap();
        }
    } else if (outcome == didi::ipc::FrameReadOutcome::rejected) {
        // Rejected before anything was read, so nothing was allocated either.
        if (served != 0 || !payload.empty()) {
            __builtin_trap();
        }
    } else {
        // A read that failed leaves the buffer where the last whole chunk left
        // it, never past what arrived.
        if (served > available || payload.size() > claimed) {
            __builtin_trap();
        }
    }

    // Deliberately not asserted: that a payload which completed is valid JSON.
    // readFramePayload does not parse. It hands the caller bytes, and the
    // caller's json::parse is fuzzed by fuzz_jsonrpc_request. Asserting it here
    // would report every non-JSON frame as a defect in a function that never
    // claimed to look at the bytes.
    return 0;
}
