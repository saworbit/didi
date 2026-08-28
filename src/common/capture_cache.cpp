#include "didi/gdextension/viewport_renderer.hpp"

#include <algorithm>

namespace didi::godot {
namespace {

bool validCaptureId(const std::string& value) {
    return value.size() == 32 &&
           std::all_of(value.begin(), value.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

} // namespace

Result<void> CaptureCache::store(const std::string& capture_id,
                                 const image::RgbaImage& pixels) {
    if (!validCaptureId(capture_id)) {
        return Error::invalidArgument("capture_id must contain exactly 32 lowercase hexadecimal characters");
    }
    const auto expected = image::checkedRgbaSize(pixels.width, pixels.height);
    if (expected.isErr()) return expected.error();
    if (pixels.rgba.size() != expected.value()) {
        return Error::invalidArgument("Capture RGBA storage does not match its dimensions");
    }
    if (m_maxEntries == 0 || expected.value() > m_maxBytes) {
        return Error::invalidArgument("Capture exceeds the configured cache budget");
    }
    if (const auto existing = m_entries.find(capture_id); existing != m_entries.end()) {
        m_bytes -= existing->second.pixels.rgba.size();
        m_entries.erase(existing);
    }
    while (!m_entries.empty() &&
           (m_entries.size() >= m_maxEntries || m_bytes + expected.value() > m_maxBytes)) {
        const auto oldest = std::min_element(m_entries.begin(), m_entries.end(), [](const auto& left, const auto& right) {
            return left.second.last_used < right.second.last_used;
        });
        m_bytes -= oldest->second.pixels.rgba.size();
        m_entries.erase(oldest);
    }
    m_entries.emplace(capture_id, Entry{pixels, ++m_clock});
    m_bytes += expected.value();
    return Result<void>::ok();
}

std::optional<image::RgbaImage> CaptureCache::find(const std::string& capture_id) {
    if (!validCaptureId(capture_id)) return std::nullopt;
    const auto found = m_entries.find(capture_id);
    if (found == m_entries.end()) return std::nullopt;
    found->second.last_used = ++m_clock;
    return found->second.pixels;
}

} // namespace didi::godot
