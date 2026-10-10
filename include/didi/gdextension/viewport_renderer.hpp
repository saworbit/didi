#pragma once

#include "didi/common/types.hpp"
#include "didi/common/json.hpp"
#include "didi/common/capture_cache.hpp"
#include "didi/common/image_diff.hpp"
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
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

class ViewportRenderer {
public:
    static ViewportRenderer& instance();

    json captureViewport(const json& params, const std::string& session_kind = "editor");
    json diffViewport(const json& params, const std::string& session_kind = "editor");
    json capturePasses(const json& params, const std::string& session_kind = "editor");

    std::string encodeImageToPngBase64(const uint8_t* rgba_data, int width, int height);

private:
    ViewportRenderer() = default;
    image::CaptureCache m_captureCache;
};

} // namespace godot
} // namespace didi
