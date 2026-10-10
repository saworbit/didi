#pragma once

#include "didi/common/json.hpp"
#include "didi/common/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace didi {
namespace runtime {

// What bounds a running game's frame, judged from where each frame's time went.
//
// The Performance monitors cannot say it on their own. TIME_PROCESS covers the
// draw call, and the draw call is where the main thread waits for vsync and for
// a busy GPU: an idle game with vsync on reads 18.5 ms of process time for
// 0.02 ms of work (measured on 4.5.1, 4.6.2 and 4.7.2). So the bridge splits
// each frame by timestamps the addon's frame timer records on the engine's
// physics_frame, process_frame and frame_pre_draw signals, and takes the
// rendering cost from the root viewport's measured render times. Everything
// here is a pure function of those numbers.

// Where the parts of one frame began, in microseconds on the engine's
// Time.get_ticks_usec clock, as the addon's didi_frame_timer.gd records them.
// -1 is a part that did not run in the frame.
struct FrameMarks {
    int64_t physics_start{-1};
    int64_t physics_ticks{0};
    int64_t process_start{-1};
    int64_t draw_start{-1};
};

// One whole frame, from the end of the previous frame callback to the end of
// this one, in milliseconds. physics + process + draw + outside is frame.
struct FrameTiming {
    double frame_ms{0.0};
    // From the first physics tick's signal to the process step: every tick's
    // _physics_process and physics server step.
    double physics_ms{0.0};
    // From the process step to the draw: _process, timers, and waiting for
    // the rendering server to sync.
    double process_ms{0.0};
    // The draw call's wall time: rendering on the CPU plus any wait for the
    // GPU or for vsync. Zero for a frame that drew nothing.
    double draw_ms{0.0};
    // Before the first part of the frame: input, a frame delay, and the first
    // tick's physics sync.
    double outside_ms{0.0};
    // The engine's own measurements of the draw. Zero for an undrawn frame.
    double render_cpu_ms{0.0};
    double gpu_ms{0.0};
    int64_t physics_ticks{0};
    bool drawn{false};
};

// The frame that ended at end_usec, or nullopt for marks that do not belong to
// it: a part that began before the previous frame ended, out of order, or a
// frame with no process step at all.
std::optional<FrameTiming> frameTimingFromMarks(const FrameMarks& marks,
                                                int64_t previous_end_usec,
                                                int64_t end_usec,
                                                double render_cpu_ms,
                                                double gpu_ms);

struct FrameBudgetInputs {
    // DisplayServer.window_can_draw. A headless game cannot, and reports vsync
    // as enabled regardless, so vsync and refresh mean nothing without this.
    bool can_draw{false};
    int64_t vsync_mode{0};
    // DisplayServer.screen_get_refresh_rate, -1 when there is no screen.
    double refresh_hz{-1.0};
    int64_t max_fps{0};
    // RenderingServer.is_on_render_thread asked from the main thread answered
    // false. Godot calls the mode experimental, and the editor never runs it.
    bool separate_render_thread{false};
};

// How long a frame may take, and why that long.
struct FrameBudget {
    double ms{0.0};
    // vsync, max_fps, display_refresh, or assumed_60_fps when the game has no
    // screen and no frame cap.
    std::string basis;
    // With a separate render thread the main thread waits for it before the
    // frame's first part rather than inside the draw (probed on 4.7.2), so
    // outside_ms is waiting too.
    bool separate_render_thread{false};
};

FrameBudget frameBudget(const FrameBudgetInputs& inputs);

// The frames of one window. An uncapped game can run thousands of frames in a
// five-second read, so past capacity the log keeps every second frame and
// halves how often it records, which keeps the whole window rather than its
// start.
class FrameTimingLog {
public:
    static constexpr size_t kDefaultCapacity = 2048;
    // How many of the slowest frames are kept aside from the stride.
    static constexpr size_t kSlowestKept = 16;

    FrameTimingLog() : FrameTimingLog(kDefaultCapacity) {}
    explicit FrameTimingLog(size_t capacity);

    void add(const FrameTiming& frame);
    // A frame callback whose marks did not describe the frame that ended.
    void skip() { ++m_skipped; }

    const std::vector<FrameTiming>& frames() const { return m_frames; }
    // The slowest frames seen, slowest first, whichever the stride kept. A
    // hitch is one frame, and halving the log would drop it half the time.
    std::vector<FrameTiming> slowest() const;
    int64_t seen() const { return m_seen; }
    int64_t skipped() const { return m_skipped; }

private:
    size_t m_capacity;
    int64_t m_stride{1};
    int64_t m_seen{0};
    int64_t m_skipped{0};
    std::vector<FrameTiming> m_frames;
    // A heap with the fastest of the slowest on top.
    std::vector<FrameTiming> m_slowest;
};

// The verdict object runtime_read_profiler returns beside its samples:
// bound (cpu, gpu, physics, contested, none or unknown), confidence, frames,
// the budget and its basis, median_ms per part, physics_ticks_per_frame,
// gpu_measured, slow_frames, and next, the thing to look at. The bound is
// judged on medians, so one hitch does not decide it; slow_frames counts the
// frames over twice the budget and judges the worst three on their own parts.
json judgePerformance(const FrameTimingLog& log, const FrameBudget& budget);

// The verdict when no frame could be timed at all.
json unavailableVerdict(const std::string& reason);

// The self-check: after the read, stall the game's process step by a known
// amount, read again, and confirm the verdict names it. The stall is twice the
// largest of the budget and the baseline's frame and GPU times, so it outweighs
// whatever bound the frame before, and it is capped so a slow game is not held
// for long.
double selfCheckStallMs(const json& baseline_verdict);
int selfCheckWindowMs(double stall_ms);
json judgeSelfCheck(double stall_ms, const json& stalled_verdict);

} // namespace runtime
} // namespace didi
