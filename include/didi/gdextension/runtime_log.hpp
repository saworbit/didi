#pragma once

#include "didi/common/types.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>

namespace didi::godot {

class RuntimeLogRing {
public:
    static constexpr size_t kDefaultCapacity = 2000;
    static constexpr size_t kMaxMessageBytes = 16 * 1024;
    static constexpr size_t kMaxDetailsBytes = 64 * 1024;

    explicit RuntimeLogRing(size_t capacity = kDefaultCapacity, uint64_t first_sequence = 1);

    // Read records always carry `details` as either null (absent) or an object.
    Result<uint64_t> append(std::string_view level, std::string_view source, std::string_view message,
                            const json& details = json());
    Result<json> read(uint64_t cursor, size_t limit, std::string_view minimum_level) const;

    static bool isValidLevel(std::string_view level);

    // The sequence the next appended record will carry. A watch takes this
    // before it starts so an error it counts is one this run produced, not one
    // already sitting on the ring.
    uint64_t nextSequence() const;

    // How many retained records at or above `minimum_level` carry a sequence at
    // or after `cursor`. Counting rather than reading keeps a per-frame check
    // from copying the payloads it is not going to look at.
    size_t countFrom(uint64_t cursor, std::string_view minimum_level) const;

private:
    struct Record {
        uint64_t sequence;
        int64_t timestamp_ms;
        std::string level;
        std::string source;
        std::string message;
        json details;
    };

    static int levelRank(std::string_view level);
    static json boundedDetails(const json& details);

    size_t m_capacity;
    uint64_t m_firstSequence{1};
    uint64_t m_nextSequence{1};
    bool m_exhausted{false};
    std::deque<Record> m_records;
    mutable std::mutex m_mutex;
};

} // namespace didi::godot
