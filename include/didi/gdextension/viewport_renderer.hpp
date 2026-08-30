#pragma once

#include "didi/common/types.hpp"
#include "didi/common/json.hpp"
#include "didi/common/image_diff.hpp"
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace didi {
namespace godot {

class RestorationGuard {
public:
    explicit RestorationGuard(std::function<Result<void>()> restore)
        : m_restore(std::move(restore)) {}
    RestorationGuard(const RestorationGuard&) = delete;
    RestorationGuard& operator=(const RestorationGuard&) = delete;
    ~RestorationGuard() {
        if (m_active && m_restore) (void)m_restore();
    }

    Result<void> restoreNow() {
        if (!m_active) return Result<void>::ok();
        if (!m_restore) {
            m_active = false;
            return Result<void>::ok();
        }
        auto restored = m_restore();
        // Stay armed unless the restore actually succeeded. Disarming first
        // meant a failed restore was never retried, so isolation left the
        // hidden nodes hidden for good while the caller was handed an error
        // that read as if nothing had been touched.
        if (restored.isOk()) m_active = false;
        return restored;
    }
    void dismiss() { m_active = false; }

private:
    std::function<Result<void>()> m_restore;
    bool m_active{true};
};

class CaptureCache {
public:
    explicit CaptureCache(size_t max_entries = 8,
                          size_t max_bytes = 64u * 1024u * 1024u)
        : m_maxEntries(max_entries), m_maxBytes(max_bytes) {}

    Result<void> store(const std::string& capture_id, const image::RgbaImage& pixels);
    // Moves the frame in. A 2048x2048 capture is 16 MiB, so the copy the
    // const-ref overload makes is worth avoiding on the freshly captured path.
    Result<void> store(const std::string& capture_id, image::RgbaImage&& pixels);

    // Borrows the cached frame instead of copying it, and marks it as used.
    // The pointer is valid until the next store() on this cache, which can
    // evict; finish reading before storing anything new.
    const image::RgbaImage* find(const std::string& capture_id);

    // Existence only. Does not copy and does not touch the LRU order.
    bool contains(const std::string& capture_id) const;
    size_t size() const { return m_entries.size(); }
    size_t bytes() const { return m_bytes; }

private:
    // Both store overloads land here, so the budget, eviction and replacement
    // rules exist once.
    Result<void> storeFrame(const std::string& capture_id, image::RgbaImage&& pixels);

    struct Entry {
        image::RgbaImage pixels;
        uint64_t last_used{0};
    };

    size_t m_maxEntries;
    size_t m_maxBytes;
    size_t m_bytes{0};
    uint64_t m_clock{0};
    std::unordered_map<std::string, Entry> m_entries;
};

class ViewportRenderer {
public:
    static ViewportRenderer& instance();

    json captureViewport(const json& params);
    json diffViewport(const json& params);

    std::string encodeImageToPngBase64(const uint8_t* rgba_data, int width, int height);

private:
    ViewportRenderer() = default;
    CaptureCache m_captureCache;
};

} // namespace godot
} // namespace didi
