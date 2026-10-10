#pragma once

// The process-local cache behind viewport captures: frames by capture_id,
// least recently used first out, within an entry and byte budget.

#include "didi/common/image_diff.hpp"
#include "didi/common/types.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace didi::image {

class CaptureCache {
public:
    explicit CaptureCache(size_t max_entries = 8,
                          size_t max_bytes = 64u * 1024u * 1024u)
        : m_maxEntries(max_entries), m_maxBytes(max_bytes) {}

    Result<void> store(const std::string& capture_id, const RgbaImage& pixels);
    // Moves the frame in. A 2048x2048 capture is 16 MiB, so the copy the
    // const-ref overload makes is worth avoiding on the freshly captured path.
    Result<void> store(const std::string& capture_id, RgbaImage&& pixels);

    // Borrows the cached frame instead of copying it, and marks it as used.
    // The pointer is valid until the next store() on this cache, which can
    // evict; finish reading before storing anything new.
    const RgbaImage* find(const std::string& capture_id);

    // Existence only. Does not copy and does not touch the LRU order.
    bool contains(const std::string& capture_id) const;
    size_t size() const { return m_entries.size(); }
    size_t bytes() const { return m_bytes; }

private:
    // Both store overloads land here, so the budget, eviction and replacement
    // rules exist once.
    Result<void> storeFrame(const std::string& capture_id, RgbaImage&& pixels);

    struct Entry {
        RgbaImage pixels;
        uint64_t last_used{0};
    };

    size_t m_maxEntries;
    size_t m_maxBytes;
    size_t m_bytes{0};
    uint64_t m_clock{0};
    std::unordered_map<std::string, Entry> m_entries;
};

} // namespace didi::image
