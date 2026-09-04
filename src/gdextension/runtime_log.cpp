#include "didi/gdextension/runtime_log.hpp"

#include <algorithm>
#include <chrono>
#include <limits>

namespace didi::godot {

namespace {

std::string truncateUtf8(std::string_view value, size_t maximum_bytes) {
    std::string result;
    result.reserve(std::min(value.size(), maximum_bytes));
    for (size_t index = 0; index < value.size();) {
        const auto first = static_cast<unsigned char>(value[index]);
        size_t width = 1;
        bool valid = false;
        uint32_t codepoint = 0;
        if ((first & 0x80) == 0) {
            width = 1;
            valid = true;
            codepoint = first;
        } else if ((first & 0xE0) == 0xC0 && first >= 0xC2) {
            width = 2;
            valid = true;
            codepoint = first & 0x1F;
        } else if ((first & 0xF0) == 0xE0) {
            width = 3;
            valid = true;
            codepoint = first & 0x0F;
        } else if ((first & 0xF8) == 0xF0 && first <= 0xF4) {
            width = 4;
            valid = true;
            codepoint = first & 0x07;
        }

        valid = valid && index + width <= value.size();
        for (size_t offset = 1; valid && offset < width; ++offset) {
            const auto next = static_cast<unsigned char>(value[index + offset]);
            valid = (next & 0xC0) == 0x80;
            codepoint = (codepoint << 6) | (next & 0x3F);
        }
        valid = valid && (width == 1 || (width == 2 && codepoint >= 0x80) ||
                          (width == 3 && codepoint >= 0x800) ||
                          (width == 4 && codepoint >= 0x10000));
        valid = valid && !(codepoint >= 0xD800 && codepoint <= 0xDFFF) && codepoint <= 0x10FFFF;
        if (!valid) {
            if (result.size() + 1 > maximum_bytes) break;
            result.push_back('?');
            ++index;
            continue;
        }
        if (result.size() + width > maximum_bytes) break;
        result.append(value.substr(index, width));
        index += width;
    }
    return result;
}

} // namespace

RuntimeLogRing::RuntimeLogRing(size_t capacity, uint64_t first_sequence)
    : m_capacity(std::max<size_t>(1, capacity)),
      m_firstSequence(first_sequence == 0 ? 1 : first_sequence),
      m_nextSequence(first_sequence == 0 ? 1 : first_sequence) {}

bool RuntimeLogRing::isValidLevel(std::string_view level) {
    return level == "debug" || level == "info" || level == "warning" || level == "error";
}

int RuntimeLogRing::levelRank(std::string_view level) {
    if (level == "debug") return 0;
    if (level == "info") return 1;
    if (level == "warning") return 2;
    if (level == "error") return 3;
    return -1;
}

json RuntimeLogRing::boundedDetails(const json& details) {
    if (details.is_null()) return nullptr;
    const json normalized = details.is_object() ? details : json{{"value", details}};
    std::string serialized;
    try {
        serialized = normalized.dump();
    } catch (const json::exception&) {
        return {{"truncated", true}, {"preview", "Invalid UTF-8 details omitted."}};
    }
    if (serialized.size() <= kMaxDetailsBytes) return normalized;

    json bounded = {{"truncated", true}, {"preview", truncateUtf8(serialized, kMaxDetailsBytes)}};
    auto& preview = bounded["preview"].get_ref<std::string&>();
    while (!preview.empty() && bounded.dump().size() > kMaxDetailsBytes) {
        preview = truncateUtf8(preview, preview.size() - 1);
    }
    return bounded;
}

Result<uint64_t> RuntimeLogRing::append(std::string_view level, std::string_view source, std::string_view message,
                                        const json& details) {
    if (!isValidLevel(level)) return Error::invalidArgument("Runtime log level must be debug, info, warning, or error");
    const auto now = std::chrono::system_clock::now();
    const auto timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    std::string bounded_message = truncateUtf8(message, kMaxMessageBytes);

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_exhausted) return Error(507, "Runtime log sequence is exhausted");
    const uint64_t sequence = m_nextSequence;
    if (sequence == std::numeric_limits<uint64_t>::max()) m_exhausted = true;
    else ++m_nextSequence;
    m_records.push_back({sequence, timestamp_ms, truncateUtf8(level, 16), truncateUtf8(source, 1024),
                         std::move(bounded_message), boundedDetails(details)});
    while (m_records.size() > m_capacity) {
        m_records.pop_front();
    }
    return sequence;
}

Result<json> RuntimeLogRing::read(uint64_t cursor, size_t limit, std::string_view minimum_level) const {
    if (limit < 1 || limit > 500) return Error::invalidArgument("Runtime log limit must be an integer from 1 to 500");
    if (!isValidLevel(minimum_level)) return Error::invalidArgument("Runtime log minimum_level must be debug, info, warning, or error");
    std::lock_guard<std::mutex> lock(m_mutex);

    const uint64_t oldest = m_records.empty() ? m_nextSequence : m_records.front().sequence;
    // Cursor 0 is the documented way to ask for everything the ring still
    // holds, not a position before a discarded record. Comparing it against the
    // oldest sequence reported a retention gap on the very first read of every
    // session, which is the opposite of what the flag is for. Read it as the
    // ring's own first sequence and the answer is true again: a gap only when
    // records really were evicted.
    const uint64_t requested = cursor == 0 ? m_firstSequence : cursor;
    const bool dropped = !m_records.empty() && requested < oldest;
    const uint64_t start = requested < oldest ? oldest : requested;
    uint64_t next_cursor = start;
    json records = json::array();
    const int minimum_rank = levelRank(minimum_level);

    for (const auto& record : m_records) {
        if (record.sequence < start) continue;
        next_cursor = record.sequence == std::numeric_limits<uint64_t>::max()
            ? std::numeric_limits<uint64_t>::max() : record.sequence + 1;
        if (levelRank(record.level) < minimum_rank) continue;

        json value = {
            {"sequence", record.sequence},
            {"timestamp_ms", record.timestamp_ms},
            {"level", record.level},
            {"source", record.source},
            {"message", record.message},
            {"details", record.details}
        };
        records.push_back(std::move(value));
        if (records.size() >= limit) break;
    }

    return json{
        {"records", std::move(records)},
        {"next_cursor", next_cursor},
        {"oldest_cursor", oldest},
        {"dropped_before_cursor", dropped},
        {"exhausted", m_exhausted}
    };
}

} // namespace didi::godot
