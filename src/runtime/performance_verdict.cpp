#include "didi/runtime/performance_verdict.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace didi {
namespace runtime {

namespace {

// Fewer whole frames than this is not a window, whatever they say.
constexpr size_t kMinimumFrames = 3;
// Ten frames is where a median stops being decided by one frame.
constexpr size_t kConfidentFrames = 10;
// A frame within a tenth of its budget keeps to it. Frame pacing jitters by
// about that much on its own.
constexpr double kOverBudget = 1.10;
// One side has to lead the other by a quarter to be the bound. Cutting a side
// that leads by less moves the frame by less than the other side then costs.
constexpr double kLeadRatio = 1.25;
constexpr double kStrongLeadRatio = 1.5;
// Without a GPU time, CPU work has to account for half the frame before it is
// called the bound; the rest could be the GPU.
constexpr double kExplainsFrame = 0.5;
constexpr double kExplainsWell = 0.6;
constexpr double kExplainsSome = 0.4;
// A part of the budget this small leaves room to spare.
constexpr double kComfortable = 0.75;
// A frame over this many budgets is a slow frame, and this many of them are
// described one by one.
constexpr double kSlowFrameBudgets = 2.0;
constexpr size_t kSlowFramesDescribed = 3;

constexpr double kStallFloorMs = 20.0;
constexpr double kStallCapMs = 250.0;
constexpr int kSelfCheckMinimumWindowMs = 500;
constexpr int kSelfCheckMaximumWindowMs = 2500;
constexpr double kStallSeen = 0.9;

double millisecondsBetween(int64_t from_usec, int64_t to_usec) {
    return static_cast<double>(to_usec - from_usec) / 1000.0;
}

double rounded(double value) {
    return std::round(value * 1000.0) / 1000.0;
}

double median(std::vector<double> values) {
    if (values.empty()) return 0.0;
    const size_t middle = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(middle), values.end());
    const double upper = values[middle];
    if (values.size() % 2 == 1) return upper;
    const double lower = *std::max_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(middle));
    return (lower + upper) / 2.0;
}

std::string ms(double value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.1f", value);
    return buffer;
}

double measured(double value) {
    return std::isfinite(value) && value > 0.0 ? value : 0.0;
}

double numberOr(const json& object, const char* key, double fallback) {
    if (!object.is_object()) return fallback;
    const auto found = object.find(key);
    return found != object.end() && found->is_number() ? found->get<double>() : fallback;
}

// Puts the faster frame on top of a heap, so the heap holds the slowest.
bool fasterOnTop(const FrameTiming& a, const FrameTiming& b) { return a.frame_ms > b.frame_ms; }

// One slow frame, judged on its own parts by the rule the verdict applies to
// the medians.
json slowFrame(const FrameTiming& frame, bool gpu_measured) {
    const double cpu = frame.physics_ms + frame.process_ms + frame.render_cpu_ms;
    const double gpu = gpu_measured && frame.drawn ? frame.gpu_ms : 0.0;
    const char* cpu_side = frame.physics_ms >= frame.process_ms + frame.render_cpu_ms ? "physics" : "cpu";
    const char* bound = "unknown";
    if (gpu > 0.0) {
        if (std::max(cpu, gpu) >= frame.frame_ms * kExplainsSome) {
            if (gpu >= cpu) {
                bound = gpu / std::max(cpu, 0.001) >= kLeadRatio ? "gpu" : "contested";
            } else {
                bound = cpu / std::max(gpu, 0.001) >= kLeadRatio ? cpu_side : "contested";
            }
        }
    } else if (cpu >= frame.frame_ms * kExplainsFrame) {
        bound = cpu_side;
    }
    return {
        {"frame_ms", rounded(frame.frame_ms)},
        {"bound", bound},
        {"physics_ms", rounded(frame.physics_ms)},
        {"process_ms", rounded(frame.process_ms)},
        {"render_cpu_ms", rounded(frame.render_cpu_ms)},
        {"gpu_ms", gpu_measured && frame.drawn ? json(rounded(frame.gpu_ms)) : json(nullptr)},
        {"draw_ms", rounded(frame.draw_ms)},
        {"outside_ms", rounded(frame.outside_ms)},
        {"physics_ticks", frame.physics_ticks},
    };
}

// The frames over twice the budget, which the median verdict lets pass on
// purpose: a stutter is the other half of why a game is slow (#1230).
json slowFrames(const FrameTimingLog& log, const FrameBudget& budget, bool gpu_measured) {
    const double threshold = kSlowFrameBudgets * budget.ms;
    const auto slowest = log.slowest();
    size_t over = 0;
    json worst = json::array();
    for (const auto& frame : slowest) {
        if (frame.frame_ms <= threshold) break;
        ++over;
        if (worst.size() < kSlowFramesDescribed) worst.push_back(slowFrame(frame, gpu_measured));
    }
    json slow = {{"threshold_ms", rounded(threshold)}, {"count", over}, {"worst", std::move(worst)}};
    // Every frame kept aside was over, so more may have been.
    if (over == FrameTimingLog::kSlowestKept && log.seen() > static_cast<int64_t>(over)) {
        slow["at_least"] = true;
    }
    return slow;
}

} // namespace

std::optional<FrameTiming> frameTimingFromMarks(const FrameMarks& marks,
                                                int64_t previous_end_usec,
                                                int64_t end_usec,
                                                double render_cpu_ms,
                                                double gpu_ms) {
    if (previous_end_usec < 0 || end_usec <= previous_end_usec) return std::nullopt;
    // Every frame has a process step. Marks whose process step began before
    // the previous frame ended are that frame's, left over because the timer
    // saw nothing since.
    if (marks.process_start < previous_end_usec || marks.process_start > end_usec) {
        return std::nullopt;
    }
    const bool physics = marks.physics_start >= 0;
    if (physics && (marks.physics_start < previous_end_usec ||
                    marks.physics_start > marks.process_start)) {
        return std::nullopt;
    }
    const bool drawn = marks.draw_start >= 0;
    if (drawn && (marks.draw_start < marks.process_start || marks.draw_start > end_usec)) {
        return std::nullopt;
    }

    FrameTiming timing;
    timing.frame_ms = millisecondsBetween(previous_end_usec, end_usec);
    timing.outside_ms = millisecondsBetween(previous_end_usec,
                                            physics ? marks.physics_start : marks.process_start);
    timing.physics_ms = physics ? millisecondsBetween(marks.physics_start, marks.process_start) : 0.0;
    timing.process_ms = millisecondsBetween(marks.process_start, drawn ? marks.draw_start : end_usec);
    timing.draw_ms = drawn ? millisecondsBetween(marks.draw_start, end_usec) : 0.0;
    timing.physics_ticks = physics ? std::max<int64_t>(1, marks.physics_ticks) : 0;
    timing.drawn = drawn;
    // The viewport's measurements describe the last draw, which is this
    // frame's only if it drew.
    if (drawn) {
        timing.render_cpu_ms = measured(render_cpu_ms);
        timing.gpu_ms = measured(gpu_ms);
    }
    return timing;
}

FrameBudget frameBudget(const FrameBudgetInputs& inputs) {
    FrameBudget budget;
    if (inputs.can_draw && inputs.refresh_hz > 0.0) {
        budget.ms = 1000.0 / inputs.refresh_hz;
        budget.basis = inputs.vsync_mode != 0 ? "vsync" : "display_refresh";
    }
    // A cap below the refresh rate is the slower of the two limits, so it is
    // the one the frame is held to.
    if (inputs.max_fps > 0) {
        const double capped = 1000.0 / static_cast<double>(inputs.max_fps);
        if (capped > budget.ms) {
            budget.ms = capped;
            budget.basis = "max_fps";
        }
    }
    if (budget.basis.empty()) {
        budget.ms = 1000.0 / 60.0;
        budget.basis = "assumed_60_fps";
    }
    budget.separate_render_thread = inputs.separate_render_thread;
    return budget;
}

FrameTimingLog::FrameTimingLog(size_t capacity) : m_capacity(std::max<size_t>(capacity, 2)) {
    m_frames.reserve(m_capacity);
}

void FrameTimingLog::add(const FrameTiming& frame) {
    if (m_slowest.size() < kSlowestKept) {
        m_slowest.push_back(frame);
        std::push_heap(m_slowest.begin(), m_slowest.end(), fasterOnTop);
    } else if (frame.frame_ms > m_slowest.front().frame_ms) {
        std::pop_heap(m_slowest.begin(), m_slowest.end(), fasterOnTop);
        m_slowest.back() = frame;
        std::push_heap(m_slowest.begin(), m_slowest.end(), fasterOnTop);
    }
    const int64_t index = m_seen++;
    if (index % m_stride != 0) return;
    if (m_frames.size() >= m_capacity) {
        // Keep every second frame and record half as often from here, so the
        // log still spans the window it was given.
        size_t kept = 0;
        for (size_t at = 0; at < m_frames.size(); at += 2) m_frames[kept++] = m_frames[at];
        m_frames.resize(kept);
        m_stride *= 2;
        if (index % m_stride != 0) return;
    }
    m_frames.push_back(frame);
}

std::vector<FrameTiming> FrameTimingLog::slowest() const {
    auto sorted = m_slowest;
    std::sort(sorted.begin(), sorted.end(), fasterOnTop);
    return sorted;
}

json unavailableVerdict(const std::string& reason) {
    return {
        {"bound", "unknown"},
        {"confidence", "low"},
        {"frames", 0},
        {"gpu_measured", false},
        {"next", reason},
    };
}

json judgePerformance(const FrameTimingLog& log, const FrameBudget& budget) {
    const auto& frames = log.frames();
    const size_t count = frames.size();

    std::vector<double> frame, physics, process, draw, outside, render_cpu, gpu;
    double ticks = 0.0;
    size_t drawn = 0;
    for (const auto& timing : frames) {
        frame.push_back(timing.frame_ms);
        physics.push_back(timing.physics_ms);
        process.push_back(timing.process_ms);
        draw.push_back(timing.draw_ms);
        outside.push_back(timing.outside_ms);
        ticks += static_cast<double>(timing.physics_ticks);
        if (!timing.drawn) continue;
        ++drawn;
        render_cpu.push_back(timing.render_cpu_ms);
        if (timing.gpu_ms > 0.0) gpu.push_back(timing.gpu_ms);
    }
    // A renderer without timestamp queries reports zero for every frame, and
    // the first frames after measuring starts have none yet. Half the drawn
    // frames carrying a time is a GPU that was measured.
    const bool gpu_measured = drawn > 0 && gpu.size() * 2 >= drawn;

    json verdict = {
        {"frames", count},
        {"frame_budget_ms", rounded(budget.ms)},
        {"budget_basis", budget.basis},
        {"gpu_measured", gpu_measured},
    };
    const bool separate = budget.separate_render_thread;
    if (separate) verdict["render_thread"] = "separate";
    if (count == 0) {
        verdict["bound"] = "unknown";
        verdict["confidence"] = "low";
        verdict["next"] = "No whole frame was timed. Read for longer.";
        return verdict;
    }

    const double F = median(frame);
    const double P = median(physics);
    const double S = median(process);
    const double D = median(draw);
    const double O = median(outside);
    const double R = median(render_cpu);
    const double G = gpu_measured ? median(gpu) : 0.0;
    const double ticks_per_frame = ticks / static_cast<double>(count);
    // The main thread's own work. The rest of the draw is waiting.
    const double C = P + S + R;

    verdict["median_ms"] = {
        {"frame", rounded(F)},
        {"physics", rounded(P)},
        {"process", rounded(S)},
        {"render_cpu", rounded(R)},
        {"gpu", gpu_measured ? json(rounded(G)) : json(nullptr)},
        {"draw", rounded(D)},
        {"outside", rounded(O)},
    };
    verdict["physics_ticks_per_frame"] = std::round(ticks_per_frame * 100.0) / 100.0;
    verdict["slow_frames"] = slowFrames(log, budget, gpu_measured);

    const auto conclude = [&](const char* bound, const char* confidence, std::string next) {
        verdict["bound"] = bound;
        verdict["confidence"] = confidence;
        verdict["next"] = std::move(next);
        return verdict;
    };

    if (count < kMinimumFrames) {
        return conclude("unknown", "low",
                        "Only " + std::to_string(count) + " whole frames were timed. Read for longer.");
    }

    if (F <= budget.ms * kOverBudget) {
        const double peak = std::max(C, G);
        const auto& slow = verdict["slow_frames"];
        std::string next = "The frame keeps to its " + ms(budget.ms) + " ms budget (" + budget.basis +
                           ") with " + ms(std::max(0.0, budget.ms - peak)) + " ms to spare.";
        if (!slow["worst"].empty()) {
            const auto& first = slow["worst"][0];
            next += " " + std::to_string(slow["count"].get<size_t>()) +
                    " frame(s) took over twice that; the slowest took " +
                    ms(first["frame_ms"].get<double>()) + " ms and was " +
                    first["bound"].get<std::string>() + " bound (slow_frames has its parts).";
        } else {
            next += " Read again while the slow part of the game is on screen.";
        }
        return conclude("none", peak <= budget.ms * kComfortable ? "high" : "medium", std::move(next));
    }

    // A separate render thread is waited for out there, so that time is not
    // unplaced work.
    if (!separate && O > std::max(C, G) && O >= F * kExplainsFrame) {
        return conclude("unknown", "low",
                        "Most of the " + ms(F) + " ms frame (" + ms(O) +
                            " ms) is spent before its physics, process and draw steps: input "
                            "handling, a frame delay, or the first physics tick's sync.");
    }

    const char* cpu_side = P >= S + R ? "physics" : "cpu";
    double lead = 0.0;
    double ratio = 0.0;
    const char* bound = nullptr;
    if (gpu_measured) {
        if (G >= C) {
            lead = G;
            ratio = G / std::max(C, 0.001);
            bound = ratio >= kLeadRatio ? "gpu" : "contested";
        } else {
            lead = C;
            ratio = C / std::max(G, 0.001);
            bound = ratio >= kLeadRatio ? cpu_side : "contested";
        }
        // The larger side has to account for a fair part of the frame, or the
        // frame went to a wait neither side measures.
        if (lead < F * kExplainsSome) {
            const std::string waited =
                separate ? "The main thread waited " + ms(O + std::max(0.0, D - R)) +
                               " ms for the separate render thread, which neither measures."
                         : "The draw waited " + ms(std::max(0.0, D - R)) +
                               " ms for something neither measures: another program on the GPU, "
                               "or the compositor.";
            return conclude("unknown", "low",
                            "Neither the CPU work (" + ms(C) + " ms) nor the GPU (" + ms(G) +
                                " ms) accounts for the " + ms(F) + " ms frame. " + waited);
        }
    } else if (C >= F * kExplainsFrame) {
        // No GPU time to weigh against, so the lead is only what the CPU work
        // explains. It is never stated with high confidence.
        lead = C;
        ratio = kLeadRatio;
        bound = cpu_side;
    } else {
        // Only a drawn frame reaches here: an undrawn one has no draw to wait
        // in, so time the CPU work does not explain is outside the frame.
        return conclude("unknown", "low",
                        "The frame took " + ms(F) + " ms and the timed CPU work only " + ms(C) +
                            " ms. The renderer reported no GPU time, so the rest cannot be placed.");
    }

    const double explained = lead / F;
    const char* confidence = "low";
    if (std::string(bound) == "contested") {
        confidence = count >= kConfidentFrames ? "medium" : "low";
    } else if (ratio >= kStrongLeadRatio && explained >= kExplainsWell && count >= kConfidentFrames) {
        confidence = "high";
    } else if (explained >= kExplainsSome) {
        confidence = "medium";
    }
    // Probed on one engine line only, in a mode Godot calls experimental.
    if (separate && std::string(confidence) == "high") confidence = "medium";

    const std::string of_frame = " of a " + ms(F) + " ms frame";
    const std::string kind = bound;
    if (kind == "gpu") {
        return conclude(bound, confidence,
                        "The GPU took " + ms(G) + " ms" + of_frame +
                            ". Look at shader cost and fill: full-screen effects, overdraw, "
                            "resolution, lights and shadows.");
    }
    if (kind == "contested") {
        return conclude(bound, confidence,
                        "The CPU (" + ms(C) + " ms) and the GPU (" + ms(G) +
                            " ms) are within a quarter of each other in a " + ms(F) +
                            " ms frame, so cutting one alone gains little.");
    }
    if (kind == "physics") {
        std::string next = "Physics took " + ms(P) + " ms" + of_frame + " over " +
                           ms(ticks_per_frame) +
                           " ticks. Look at active bodies, collision pairs, shape complexity and "
                           "_physics_process.";
        if (ticks_per_frame >= 1.5) {
            next += " More than one tick a frame is the engine catching up, so each tick's cost "
                    "is paid several times.";
        }
        return conclude(bound, confidence, std::move(next));
    }
    if (S >= R) {
        return conclude(bound, confidence,
                        "The process step took " + ms(S) + " ms" + of_frame +
                            ". Look at _process, timers and signal handlers that run every frame.");
    }
    return conclude(bound, confidence,
                    "Rendering on the CPU took " + ms(R) + " ms" + of_frame +
                        ". Look at draw calls and visible objects: batching, culling, fewer "
                        "materials and lights.");
}

double selfCheckStallMs(const json& baseline_verdict) {
    double largest = numberOr(baseline_verdict, "frame_budget_ms", 1000.0 / 60.0);
    if (baseline_verdict.is_object() && baseline_verdict.contains("median_ms")) {
        const auto& medians = baseline_verdict["median_ms"];
        largest = std::max(largest, numberOr(medians, "frame", 0.0));
        largest = std::max(largest, numberOr(medians, "gpu", 0.0));
    }
    return std::clamp(2.0 * largest, kStallFloorMs, kStallCapMs);
}

int selfCheckWindowMs(double stall_ms) {
    return std::clamp(static_cast<int>(std::lround(6.0 * stall_ms)), kSelfCheckMinimumWindowMs,
                      kSelfCheckMaximumWindowMs);
}

json judgeSelfCheck(double stall_ms, const json& stalled_verdict) {
    const std::string bound = stalled_verdict.value("bound", std::string("unknown"));
    const double process =
        stalled_verdict.contains("median_ms") ? numberOr(stalled_verdict["median_ms"], "process", 0.0)
                                              : 0.0;
    const bool passed = bound == "cpu" && process >= stall_ms * kStallSeen;
    json result = {
        {"stall_ms", rounded(stall_ms)},
        {"bound", bound},
        {"process_ms", rounded(process)},
        {"frames", stalled_verdict.value("frames", 0)},
        {"passed", passed},
    };
    if (!passed) {
        result["note"] = "A " + ms(stall_ms) + " ms stall in the process step read as " + bound +
                         " with " + ms(process) +
                         " ms of process time, so this read's verdict is unproven here.";
    }
    return result;
}

} // namespace runtime
} // namespace didi
