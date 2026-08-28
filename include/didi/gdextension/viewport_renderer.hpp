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
        m_active = false;
        return m_restore ? m_restore() : Result<void>::ok();
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
    std::optional<image::RgbaImage> find(const std::string& capture_id);
    size_t size() const { return m_entries.size(); }
    size_t bytes() const { return m_bytes; }

private:
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
