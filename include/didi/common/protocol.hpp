#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include "types.hpp"
#include "json.hpp"

namespace didi {
namespace ipc {

#if defined(_WIN32)
inline const char* kDefaultPipeName = "\\\\.\\pipe\\godot_didi_ipc";
#else
inline const char* kDefaultPipeName = "/tmp/godot_didi_ipc.sock";
#endif

inline std::string resolvePipeName(const std::string& explicit_name = "") {
    if (!explicit_name.empty() && explicit_name != kDefaultPipeName) {
        return explicit_name;
    }
    const char* env_pipe = std::getenv("DIDI_PIPE_NAME");
    if (env_pipe && std::strlen(env_pipe) > 0) {
        return env_pipe;
    }
    return !explicit_name.empty() ? explicit_name : kDefaultPipeName;
}

// Framing: a 4-byte little-endian length prefix, then that many bytes of JSON.
//
// Everything below is the one implementation of that rule. It used to exist
// five times -- a writer and a decoder here, plus a client and a server read
// on each of the two transports in ipc_channel_win32.cpp -- and the copies
// drifted apart from each other in every way a copy can (#880, #881, #882).
// The length prefix is written by whoever connected, before anything has been
// authorised, so a rule that holds in four places out of five is not a rule.
constexpr size_t kFrameLengthPrefixBytes = 4;

// The largest frame either side will read. This bounds what a frame may
// legitimately be, not what an unauthenticated claim costs; the claim costs
// one chunk, which is what readFramePayload is for.
constexpr uint32_t kMaximumFrameBytes = 128U * 1024U * 1024U;

// How much of a frame a reader commits ahead of the bytes that justify it. A
// claim of kMaximumFrameBytes followed by silence costs one chunk rather than
// 128 MiB. 64 KiB is the pipe buffer size, so a real frame still arrives in
// whole buffers.
constexpr uint32_t kFrameReadChunkBytes = 64U * 1024U;

inline std::vector<uint8_t> frameMessage(const json& message) {
    std::string serialized = message.dump();
    uint32_t len = static_cast<uint32_t>(serialized.size());
    std::vector<uint8_t> buffer(kFrameLengthPrefixBytes + len);
    buffer[0] = static_cast<uint8_t>(len & 0xFF);
    buffer[1] = static_cast<uint8_t>((len >> 8) & 0xFF);
    buffer[2] = static_cast<uint8_t>((len >> 16) & 0xFF);
    buffer[3] = static_cast<uint8_t>((len >> 24) & 0xFF);
    std::memcpy(buffer.data() + kFrameLengthPrefixBytes, serialized.data(), len);
    return buffer;
}

inline uint32_t decodeFrameLength(const uint8_t (&header)[kFrameLengthPrefixBytes]) {
    return static_cast<uint32_t>(header[0]) |
           (static_cast<uint32_t>(header[1]) << 8) |
           (static_cast<uint32_t>(header[2]) << 16) |
           (static_cast<uint32_t>(header[3]) << 24);
}

// Makes room for the next chunk of a frame that is still arriving, without
// letting the buffer get ahead of the frame by more than the growth needs.
// Doubling on its own overshoots -- growing 128 MiB in 64 KiB steps ends with
// 193 MiB of capacity -- which would have made a frame somebody actually sends
// cost half as much again as allocating it outright. Capacity is capped at the
// frame, so a completed frame peaks exactly where it used to and a frame that
// is claimed and never sent peaks at one chunk.
inline void growFrameBuffer(std::vector<char>& buffer, size_t filled, size_t want,
                            size_t frame_bytes) {
    const size_t needed = filled + want;
    if (buffer.capacity() < needed) {
        size_t next = buffer.capacity() < needed / 2 ? needed : buffer.capacity() * 2;
        if (next > frame_bytes) next = frame_bytes;
        buffer.reserve(next);
    }
    buffer.resize(needed);
}

enum class FrameReadOutcome {
    // The payload arrived whole and `payload` holds exactly the claimed bytes.
    completed,
    // The length prefix was zero or larger than the caller's maximum. Nothing
    // was read and nothing was allocated.
    rejected,
    // The caller's read said no. `payload` holds what had arrived, which is a
    // chunk boundary rather than the claim.
    read_failed,
};

// Reads the payload a length prefix claims, growing the buffer as the bytes
// arrive rather than allocating from the claim.
//
// `read_chunk(char* destination, uint32_t bytes)` returns true when it filled
// exactly that many bytes. It is called at most kFrameReadChunkBytes at a
// time, and never before the buffer behind `destination` is that large, so a
// peer that claims 128 MiB and then goes quiet costs one chunk and not the
// claim. Whatever deadline the caller wants over the whole payload belongs
// inside `read_chunk`: this function has no clock and counts no time.
template <typename ReadChunk>
FrameReadOutcome readFramePayload(uint32_t frame_bytes,
                                  uint32_t maximum_bytes,
                                  std::vector<char>& payload,
                                  ReadChunk&& read_chunk) {
    if (frame_bytes == 0 || frame_bytes > maximum_bytes) {
        return FrameReadOutcome::rejected;
    }
    for (uint32_t filled = 0; filled < frame_bytes;) {
        const uint32_t want = frame_bytes - filled < kFrameReadChunkBytes
            ? frame_bytes - filled
            : kFrameReadChunkBytes;
        growFrameBuffer(payload, filled, want, frame_bytes);
        if (!read_chunk(payload.data() + filled, want)) {
            return FrameReadOutcome::read_failed;
        }
        filled += want;
    }
    return FrameReadOutcome::completed;
}

} // namespace ipc
} // namespace didi
