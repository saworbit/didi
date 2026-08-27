#include "didi/gdextension/runtime_log.hpp"

#include <algorithm>
#include <chrono>

namespace didi::godot {

namespace {

std::string truncateUtf8(std::string_view value, size_t maximum_bytes) {
    std::string result;
    result.reserve(std::min(value.size(), maximum_bytes));
    for (size_t index = 0; index < value.size();) {
        const auto first = static_cast<unsigned char>(value[index]);
        size_t width = 1;
        bool valid = false;
        if ((first & 0x80) == 0) {
            width = 1;
            valid = true;
        } else if ((first & 0xE0) == 0xC0 && first >= 0xC2) {
            width = 2;
            valid = true;
        } else if ((first & 0xF0) == 0xE0) {
            width = 3;
            valid = true;
        } else if ((first & 0xF8) == 0xF0 && first <= 0xF4) {
            width = 4;
            valid = true;
        }

        valid = valid && index + width <= value.size();
        for (size_t offset = 1; valid && offset < width; ++offset) {
            valid = (static_cast<unsigned char>(value[index + offset]) & 0xC0) == 0x80;
        }
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

RuntimeLogRing::RuntimeLogRing(size_t capacity)
    : m_capacity(std::max<size_t>(1, capacity)) {}

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
    if (details.is_null() || details.empty()) return json();
    std::string serialized;
    try {
        serialized = details.dump();
    } catch (const json::exception&) {
        return {{"truncated", true}, {"preview", "Invalid UTF-8 details omitted."}};
    }
    if (serialized.size() <= kMaxDetailsBytes) return details;

    json bounded = {{"truncated", true}, {"preview", truncateUtf8(serialized, kMaxDetailsBytes)}};
    auto& preview = bounded["preview"].get_ref<std::string&>();
    while (!preview.empty() && bounded.dump().size() > kMaxDetailsBytes) {
        preview = truncateUtf8(preview, preview.size() - 1);
    }
    return bounded;
}

void RuntimeLogRing::append(std::string_view level, std::string_view source, std::string_view message,
                            const json& details) {
    const auto now = std::chrono::system_clock::now();
    const auto timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    std::string bounded_message = truncateUtf8(message, kMaxMessageBytes);

    std::lock_guard<std::mutex> lock(m_mutex);
    m_records.push_back({m_nextSequence++, timestamp_ms, truncateUtf8(level, 16), truncateUtf8(source, 1024),
                         std::move(bounded_message), boundedDetails(details)});
    while (m_records.size() > m_capacity) {
        m_records.pop_front();
    }
}

json RuntimeLogRing::read(uint64_t cursor, size_t limit, std::string_view minimum_level) const {
    std::lock_guard<std::mutex> lock(m_mutex);

    const uint64_t oldest = m_records.empty() ? m_nextSequence : m_records.front().sequence;
    const bool dropped = !m_records.empty() && cursor < oldest;
    const uint64_t start = cursor == 0 || cursor < oldest ? oldest : cursor;
    uint64_t next_cursor = start;
    json records = json::array();
    const int minimum_rank = levelRank(minimum_level);

    for (const auto& record : m_records) {
        if (record.sequence < start) continue;
        next_cursor = record.sequence + 1;
        if (levelRank(record.level) < minimum_rank) continue;

        json value = {
            {"sequence", record.sequence},
            {"timestamp_ms", record.timestamp_ms},
            {"level", record.level},
            {"source", record.source},
            {"message", record.message}
        };
        if (!record.details.is_null() && !record.details.empty()) value["details"] = record.details;
        records.push_back(std::move(value));
        if (records.size() >= limit) break;
    }

    return {
        {"records", std::move(records)},
        {"next_cursor", next_cursor},
        {"oldest_cursor", oldest},
        {"dropped_before_cursor", dropped}
    };
}

} // namespace didi::godot
