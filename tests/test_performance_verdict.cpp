#include "didi/runtime/performance_verdict.hpp"

#include <cmath>
#include <functional>
#include <stdexcept>
#include <string>

#define ASSERT_TRUE(cond) \
    if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_EQ(a, b) \
    if (!((a) == (b))) throw std::runtime_error("Assertion failed: " #a " == " #b);
#define ASSERT_NEAR(a, b, tolerance) \
    if (std::fabs((a) - (b)) > (tolerance)) \
        throw std::runtime_error("Assertion failed: " #a " near " #b);

void registerTest(const std::string& name, std::function<void()> fn);

namespace {

using didi::json;
using didi::runtime::FrameBudget;
using didi::runtime::FrameBudgetInputs;
using didi::runtime::FrameMarks;
using didi::runtime::FrameTiming;
using didi::runtime::FrameTimingLog;
using didi::runtime::frameBudget;
using didi::runtime::frameTimingFromMarks;
using didi::runtime::judgePerformance;
using didi::runtime::judgeSelfCheck;
using didi::runtime::selfCheckStallMs;
using didi::runtime::selfCheckWindowMs;
using didi::runtime::unavailableVerdict;

const FrameBudget kVsync60{1000.0 / 60.0, "vsync"};
const FrameBudget kHeadless{1000.0 / 60.0, "assumed_60_fps"};

FrameTiming timing(double physics, double process, double draw, double outside,
                   double render_cpu, double gpu, int64_t ticks, bool drawn = true) {
    FrameTiming frame;
    frame.physics_ms = physics;
    frame.process_ms = process;
    frame.draw_ms = draw;
    frame.outside_ms = outside;
    frame.frame_ms = physics + process + draw + outside;
    frame.render_cpu_ms = render_cpu;
    frame.gpu_ms = gpu;
    frame.physics_ticks = ticks;
    frame.drawn = drawn;
    return frame;
}

FrameTimingLog repeated(const FrameTiming& frame, int count) {
    FrameTimingLog log;
    for (int index = 0; index < count; ++index) log.add(frame);
    return log;
}

std::string bound(const json& verdict) {
    return verdict["bound"].get<std::string>();
}

void test_marks_split_a_frame_into_parts_that_sum_to_it() {
    FrameMarks marks;
    marks.physics_start = 1100;
    marks.physics_ticks = 2;
    marks.process_start = 4100;
    marks.draw_start = 9100;
    auto frame = frameTimingFromMarks(marks, 1000, 26000, 0.5, 12.0);
    ASSERT_TRUE(frame.has_value());
    ASSERT_NEAR(frame->frame_ms, 25.0, 1e-9);
    ASSERT_NEAR(frame->outside_ms, 0.1, 1e-9);
    ASSERT_NEAR(frame->physics_ms, 3.0, 1e-9);
    ASSERT_NEAR(frame->process_ms, 5.0, 1e-9);
    ASSERT_NEAR(frame->draw_ms, 16.9, 1e-9);
    ASSERT_NEAR(frame->outside_ms + frame->physics_ms + frame->process_ms + frame->draw_ms,
                frame->frame_ms, 1e-9);
    ASSERT_EQ(frame->physics_ticks, 2);
    ASSERT_TRUE(frame->drawn);
    ASSERT_NEAR(frame->render_cpu_ms, 0.5, 1e-9);
    ASSERT_NEAR(frame->gpu_ms, 12.0, 1e-9);
}

void test_an_undrawn_frame_keeps_no_render_times() {
    // A headless game never draws: the process step runs to the frame's end,
    // and whatever the viewport last measured is not this frame's.
    FrameMarks marks;
    marks.process_start = 2000;
    auto frame = frameTimingFromMarks(marks, 1000, 8000, 3.0, 4.0);
    ASSERT_TRUE(frame.has_value());
    ASSERT_TRUE(!frame->drawn);
    ASSERT_NEAR(frame->process_ms, 6.0, 1e-9);
    ASSERT_NEAR(frame->draw_ms, 0.0, 1e-9);
    ASSERT_NEAR(frame->physics_ms, 0.0, 1e-9);
    ASSERT_EQ(frame->physics_ticks, 0);
    ASSERT_NEAR(frame->render_cpu_ms, 0.0, 1e-9);
    ASSERT_NEAR(frame->gpu_ms, 0.0, 1e-9);
}

void test_marks_from_another_frame_are_refused() {
    FrameMarks left_over;
    left_over.process_start = 900;  // before the previous frame ended
    ASSERT_TRUE(!frameTimingFromMarks(left_over, 1000, 5000, 0, 0).has_value());

    FrameMarks none;
    ASSERT_TRUE(!frameTimingFromMarks(none, 1000, 5000, 0, 0).has_value());

    FrameMarks draw_before_process;
    draw_before_process.process_start = 3000;
    draw_before_process.draw_start = 2000;
    ASSERT_TRUE(!frameTimingFromMarks(draw_before_process, 1000, 5000, 0, 0).has_value());

    FrameMarks physics_after_process;
    physics_after_process.physics_start = 3500;
    physics_after_process.process_start = 3000;
    ASSERT_TRUE(!frameTimingFromMarks(physics_after_process, 1000, 5000, 0, 0).has_value());

    FrameMarks fine;
    fine.process_start = 3000;
    ASSERT_TRUE(!frameTimingFromMarks(fine, -1, 5000, 0, 0).has_value());
    ASSERT_TRUE(!frameTimingFromMarks(fine, 5000, 5000, 0, 0).has_value());
    ASSERT_TRUE(!frameTimingFromMarks(fine, 1000, 2000, 0, 0).has_value());

    FrameMarks non_finite;
    non_finite.process_start = 3000;
    non_finite.draw_start = 4000;
    auto frame = frameTimingFromMarks(non_finite, 1000, 5000, std::nan(""), -2.0);
    ASSERT_TRUE(frame.has_value());
    ASSERT_NEAR(frame->render_cpu_ms, 0.0, 1e-9);
    ASSERT_NEAR(frame->gpu_ms, 0.0, 1e-9);
}

void test_budget_names_where_it_came_from() {
    auto vsync = frameBudget({true, 1, 60.0, 0});
    ASSERT_EQ(vsync.basis, std::string("vsync"));
    ASSERT_NEAR(vsync.ms, 16.667, 0.001);

    auto refresh = frameBudget({true, 0, 144.0, 0});
    ASSERT_EQ(refresh.basis, std::string("display_refresh"));
    ASSERT_NEAR(refresh.ms, 6.944, 0.001);

    // A cap below the refresh rate is the limit the frame is held to; one
    // above it is not.
    auto capped = frameBudget({true, 1, 60.0, 30});
    ASSERT_EQ(capped.basis, std::string("max_fps"));
    ASSERT_NEAR(capped.ms, 33.333, 0.001);
    ASSERT_EQ(frameBudget({true, 1, 60.0, 240}).basis, std::string("vsync"));

    // Headless reports vsync enabled and no refresh rate (probed on 4.5.1,
    // 4.6.2 and 4.7.2). Neither means anything without a window.
    auto headless = frameBudget({false, 1, -1.0, 0});
    ASSERT_EQ(headless.basis, std::string("assumed_60_fps"));
    ASSERT_NEAR(headless.ms, 16.667, 0.001);
    ASSERT_EQ(frameBudget({false, 1, 60.0, 0}).basis, std::string("assumed_60_fps"));
    ASSERT_EQ(frameBudget({false, 1, -1.0, 20}).basis, std::string("max_fps"));
}

// The probes behind these numbers: windowed Forward+ at 60 Hz, medians per
// frame, the same on 4.5.1, 4.6.2 and 4.7.2 within a few percent.

void test_an_idle_game_waiting_on_vsync_is_not_bound() {
    // 16.7 ms frames, 16.5 of them waiting inside the draw. TIME_PROCESS reads
    // 18.5 ms here, which is the reading a monitor-only verdict would call CPU.
    auto verdict = judgePerformance(repeated(timing(0.045, 0.008, 16.52, 0.08, 0.027, 0.015, 1), 120),
                                    kVsync60);
    ASSERT_EQ(bound(verdict), std::string("none"));
    ASSERT_EQ(verdict["confidence"], "high");
    ASSERT_EQ(verdict["budget_basis"], "vsync");
    ASSERT_TRUE(verdict["gpu_measured"].get<bool>());
    ASSERT_TRUE(verdict["next"].get<std::string>().find("16.7 ms budget") != std::string::npos);
}

void test_a_busy_process_step_is_cpu_bound() {
    auto verdict = judgePerformance(repeated(timing(0.045, 25.04, 0.34, 0.06, 0.03, 0.011, 2), 80),
                                    kVsync60);
    ASSERT_EQ(bound(verdict), std::string("cpu"));
    ASSERT_EQ(verdict["confidence"], "high");
    ASSERT_NEAR(verdict["median_ms"]["process"].get<double>(), 25.04, 0.001);
    ASSERT_TRUE(verdict["next"].get<std::string>().find("_process") != std::string::npos);
}

void test_a_heavy_shader_is_gpu_bound_though_the_main_thread_waits() {
    // The main thread spends the frame inside the draw, waiting on the GPU's
    // fence: wall time there is not CPU work.
    auto vsync = judgePerformance(repeated(timing(0.017, 0.008, 33.07, 0.08, 0.044, 22.93, 2), 67),
                                  kVsync60);
    ASSERT_EQ(bound(vsync), std::string("gpu"));
    ASSERT_EQ(vsync["confidence"], "high");
    ASSERT_NEAR(vsync["median_ms"]["gpu"].get<double>(), 22.93, 0.001);
    ASSERT_TRUE(vsync["next"].get<std::string>().find("shader") != std::string::npos);

    auto unsynced = judgePerformance(repeated(timing(0.023, 0.008, 23.93, 0.07, 0.04, 22.99, 1), 84),
                                     {1000.0 / 60.0, "display_refresh"});
    ASSERT_EQ(bound(unsynced), std::string("gpu"));
}

void test_a_pile_of_bodies_is_physics_bound() {
    // 2,500 rigid bodies: eight ticks a frame, the engine catching up.
    auto verdict = judgePerformance(repeated(timing(153.08, 0.046, 0.887, 2.86, 0.52, 0.013, 8), 14),
                                    kVsync60);
    ASSERT_EQ(bound(verdict), std::string("physics"));
    ASSERT_EQ(verdict["confidence"], "high");
    ASSERT_NEAR(verdict["physics_ticks_per_frame"].get<double>(), 8.0, 1e-9);
    const auto next = verdict["next"].get<std::string>();
    ASSERT_TRUE(next.find("8.0 ticks") != std::string::npos);
    ASSERT_TRUE(next.find("catching up") != std::string::npos);
}

void test_rendering_on_the_cpu_is_named_as_such() {
    auto verdict = judgePerformance(repeated(timing(0.1, 2.0, 30.0, 0.1, 28.0, 4.0, 1), 30),
                                    kVsync60);
    ASSERT_EQ(bound(verdict), std::string("cpu"));
    ASSERT_TRUE(verdict["next"].get<std::string>().find("draw calls") != std::string::npos);
}

void test_close_cpu_and_gpu_are_contested() {
    auto verdict = judgePerformance(repeated(timing(0.5, 18.0, 21.0, 0.1, 1.0, 20.0, 1), 30),
                                    kVsync60);
    ASSERT_EQ(bound(verdict), std::string("contested"));
    ASSERT_EQ(verdict["confidence"], "medium");
}

void test_a_headless_game_is_never_called_gpu_bound() {
    // Headless: no draw, every render time zero, paced at 6.9 ms by the
    // low-processor sleep.
    auto idle = judgePerformance(repeated(timing(0.0, 0.05, 0.0, 6.85, 0, 0, 0, false), 290),
                                 kHeadless);
    ASSERT_EQ(bound(idle), std::string("none"));
    ASSERT_TRUE(!idle["gpu_measured"].get<bool>());
    ASSERT_TRUE(idle["median_ms"]["gpu"].is_null());

    auto busy = judgePerformance(repeated(timing(0.0, 25.04, 0.0, 0.01, 0, 0, 0, false), 80),
                                 kHeadless);
    ASSERT_EQ(bound(busy), std::string("cpu"));
    // Without a GPU time to weigh against, never high.
    ASSERT_EQ(busy["confidence"], "medium");
    ASSERT_TRUE(busy["median_ms"]["gpu"].is_null());
}

void test_a_slow_frame_neither_side_explains_is_not_placed() {
    // 33.6 ms frames with 0.08 ms of CPU work and 0.01 ms on the GPU: the
    // draw waited for something else. The larger of two tiny numbers is not
    // a bound.
    auto verdict = judgePerformance(repeated(timing(0.04, 0.01, 33.5, 0.06, 0.03, 0.01, 2), 15),
                                    kVsync60);
    ASSERT_EQ(bound(verdict), std::string("unknown"));
    ASSERT_TRUE(verdict["gpu_measured"].get<bool>());
    ASSERT_TRUE(verdict["next"].get<std::string>().find("Neither the CPU work") != std::string::npos);
}

void test_a_renderer_without_gpu_times_does_not_invent_one() {
    // Drawn frames, nothing measured on the GPU, and a long wait in the draw:
    // that wait could be the GPU, so it is not called CPU either.
    auto verdict = judgePerformance(repeated(timing(0.1, 2.0, 30.0, 0.1, 0.5, 0.0, 1), 30),
                                    kVsync60);
    ASSERT_EQ(bound(verdict), std::string("unknown"));
    ASSERT_TRUE(!verdict["gpu_measured"].get<bool>());
    ASSERT_TRUE(verdict["next"].get<std::string>().find("no GPU time") != std::string::npos);

    auto headless = judgePerformance(repeated(timing(0.1, 2.0, 0.0, 30.0, 0.0, 0.0, 1, false), 30),
                                     kHeadless);
    ASSERT_EQ(bound(headless), std::string("unknown"));
}

void test_time_before_the_frame_is_not_given_to_any_part() {
    auto verdict = judgePerformance(repeated(timing(0.1, 1.0, 1.0, 40.0, 0.5, 0.5, 1), 30),
                                    kVsync60);
    ASSERT_EQ(bound(verdict), std::string("unknown"));
    ASSERT_TRUE(verdict["next"].get<std::string>().find("before its physics") != std::string::npos);
}

void test_a_separate_render_thread_is_waited_for_outside_the_frame() {
    // Probed on 4.7.2 with --render-thread separate: the main thread waits for
    // the render thread before the frame's first part, not inside the draw.
    FrameBudget separate = kVsync60;
    separate.separate_render_thread = true;
    auto gpu = judgePerformance(repeated(timing(0.024, 0.04, 0.059, 83.257, 0.059, 68.623, 6), 21),
                                separate);
    ASSERT_EQ(bound(gpu), std::string("gpu"));
    ASSERT_EQ(gpu["confidence"], "medium");
    ASSERT_EQ(gpu["render_thread"], "separate");

    // 30 ms of process time in 116.7 ms frames: neither side accounts for the
    // frame, and the answer says what the rest of it was spent on.
    auto cpu = judgePerformance(repeated(timing(0.026, 30.066, 0.067, 86.534, 0.038, 0.004, 6), 20),
                                separate);
    ASSERT_EQ(bound(cpu), std::string("unknown"));
    ASSERT_TRUE(cpu["next"].get<std::string>().find("separate render thread") != std::string::npos);

    // The same frames from a game that renders on its main thread are time
    // spent before the frame, and no render_thread is named.
    auto safe = judgePerformance(repeated(timing(0.024, 0.04, 0.059, 83.257, 0.059, 68.623, 6), 21),
                                 kVsync60);
    ASSERT_EQ(bound(safe), std::string("unknown"));
    ASSERT_TRUE(!safe.contains("render_thread"));
    ASSERT_TRUE(frameBudget({true, 1, 60.0, 0, true}).separate_render_thread);
}

void test_too_few_frames_decide_nothing() {
    auto two = judgePerformance(repeated(timing(0.1, 40.0, 0.5, 0.1, 0.1, 0.1, 1), 2), kVsync60);
    ASSERT_EQ(bound(two), std::string("unknown"));
    ASSERT_EQ(two["frames"], 2);
    ASSERT_TRUE(two.contains("median_ms"));

    FrameTimingLog empty;
    auto none = judgePerformance(empty, kVsync60);
    ASSERT_EQ(bound(none), std::string("unknown"));
    ASSERT_TRUE(!none.contains("median_ms"));

    // Under ten frames a lead is not stated with high confidence.
    auto few = judgePerformance(repeated(timing(0.1, 40.0, 0.5, 0.1, 0.1, 0.1, 1), 5), kVsync60);
    ASSERT_EQ(bound(few), std::string("cpu"));
    ASSERT_EQ(few["confidence"], "medium");
}

void test_one_hitch_does_not_decide_the_window() {
    FrameTimingLog log;
    for (int index = 0; index < 59; ++index) log.add(timing(0.05, 0.01, 16.5, 0.08, 0.03, 0.01, 1));
    log.add(timing(0.05, 400.0, 0.5, 0.08, 0.03, 0.01, 1));
    const auto verdict = judgePerformance(log, kVsync60);
    ASSERT_EQ(bound(verdict), std::string("none"));
    // It does not decide the bound, and it is named beside it (#1230).
    const auto& slow = verdict["slow_frames"];
    ASSERT_EQ(slow["count"], 1);
    ASSERT_NEAR(slow["threshold_ms"].get<double>(), 2000.0 / 60.0, 0.01);
    ASSERT_EQ(slow["worst"].size(), 1u);
    ASSERT_EQ(slow["worst"][0]["bound"], "cpu");
    ASSERT_NEAR(slow["worst"][0]["frame_ms"].get<double>(), 400.63, 0.01);
    ASSERT_NEAR(slow["worst"][0]["process_ms"].get<double>(), 400.0, 0.01);
    ASSERT_TRUE(!slow.contains("at_least"));
    ASSERT_TRUE(verdict["next"].get<std::string>().find("slowest took 400.6 ms and was cpu bound") !=
                std::string::npos);
}

// The log halves what it keeps past capacity, which drops every second frame.
// A hitch is one frame, so it is kept aside from that.
void test_a_hitch_the_stride_drops_is_still_named() {
    FrameTimingLog log(8);
    for (int index = 0; index < 100; ++index) {
        log.add(index == 41 ? timing(0.05, 300.0, 0.5, 0.08, 0.03, 0.01, 1)
                            : timing(0.05, 0.01, 16.5, 0.08, 0.03, 0.01, 1));
    }
    for (const auto& frame : log.frames()) ASSERT_TRUE(frame.process_ms < 1.0);
    const auto slow = judgePerformance(log, kVsync60)["slow_frames"];
    ASSERT_EQ(slow["count"], 1);
    ASSERT_NEAR(slow["worst"][0]["process_ms"].get<double>(), 300.0, 0.01);
}

void test_each_slow_frame_is_judged_on_its_own_parts() {
    FrameTimingLog log;
    for (int index = 0; index < 30; ++index) log.add(timing(0.05, 2.0, 14.0, 0.1, 1.0, 3.0, 1));
    // A shader compile: the draw waited 90 ms on the GPU.
    log.add(timing(0.05, 2.0, 92.0, 0.1, 1.0, 90.0, 1));
    // Physics catching up: four ticks in one frame.
    log.add(timing(120.0, 2.0, 14.0, 0.1, 1.0, 3.0, 4));
    // Nothing timed accounts for it.
    log.add(timing(0.05, 2.0, 14.0, 70.0, 1.0, 3.0, 1));
    const auto slow = judgePerformance(log, kVsync60)["slow_frames"];
    ASSERT_EQ(slow["count"], 3);
    ASSERT_EQ(slow["worst"][0]["bound"], "physics");
    ASSERT_EQ(slow["worst"][0]["physics_ticks"], 4);
    ASSERT_EQ(slow["worst"][1]["bound"], "gpu");
    ASSERT_NEAR(slow["worst"][1]["gpu_ms"].get<double>(), 90.0, 0.01);
    ASSERT_EQ(slow["worst"][2]["bound"], "unknown");
}

void test_slow_frames_stay_a_fixed_size() {
    FrameTimingLog log;
    for (int index = 0; index < 40; ++index) log.add(timing(0.05, 50.0 + index, 0.5, 0.1, 0.0, 0.0, 1, false));
    const auto slow = judgePerformance(log, kHeadless)["slow_frames"];
    ASSERT_EQ(slow["count"], static_cast<int>(FrameTimingLog::kSlowestKept));
    ASSERT_EQ(slow["at_least"], true);
    ASSERT_EQ(slow["worst"].size(), 3u);
    ASSERT_NEAR(slow["worst"][0]["process_ms"].get<double>(), 89.0, 0.01);
    ASSERT_TRUE(slow["worst"][0]["gpu_ms"].is_null());
}

void test_the_log_keeps_the_whole_window_past_capacity() {
    FrameTimingLog log(8);
    for (int index = 0; index < 40; ++index) log.add(timing(static_cast<double>(index), 1, 1, 1, 0, 0, 1));
    ASSERT_EQ(log.seen(), 40);
    ASSERT_TRUE(log.frames().size() <= 8u);
    ASSERT_TRUE(log.frames().size() >= 4u);
    ASSERT_NEAR(log.frames().front().physics_ms, 0.0, 1e-9);
    // The last frame recorded is from the second half of the window.
    ASSERT_TRUE(log.frames().back().physics_ms >= 20.0);
    log.skip();
    ASSERT_EQ(log.skipped(), 1);
}

void test_the_unavailable_verdict_names_its_reason() {
    auto verdict = unavailableVerdict("No frame timer.");
    ASSERT_EQ(bound(verdict), std::string("unknown"));
    ASSERT_EQ(verdict["frames"], 0);
    ASSERT_EQ(verdict["next"], "No frame timer.");
    ASSERT_TRUE(!verdict["gpu_measured"].get<bool>());
}

void test_the_self_check_stall_outweighs_the_baseline() {
    auto idle = judgePerformance(repeated(timing(0.045, 0.008, 16.52, 0.08, 0.027, 0.015, 1), 120),
                                 kVsync60);
    ASSERT_NEAR(selfCheckStallMs(idle), 33.334, 1e-6);
    auto gpu = judgePerformance(repeated(timing(0.017, 0.008, 33.07, 0.08, 0.044, 22.93, 2), 67),
                                kVsync60);
    ASSERT_NEAR(selfCheckStallMs(gpu), 66.35, 0.1);
    auto physics = judgePerformance(repeated(timing(153.08, 0.046, 0.887, 2.86, 0.52, 0.013, 8), 14),
                                    kVsync60);
    ASSERT_NEAR(selfCheckStallMs(physics), 250.0, 1e-9);
    ASSERT_NEAR(selfCheckStallMs(unavailableVerdict("x")), 33.333, 0.001);
    ASSERT_NEAR(selfCheckStallMs(json::object({{"frame_budget_ms", 2.0}})), 20.0, 1e-9);

    ASSERT_EQ(selfCheckWindowMs(20.0), 500);
    ASSERT_EQ(selfCheckWindowMs(100.0), 600);
    ASSERT_EQ(selfCheckWindowMs(250.0), 1500);
}

void test_the_self_check_passes_only_when_the_verdict_sees_the_stall() {
    const double stall = 33.4;
    auto seen = judgePerformance(repeated(timing(0.04, 33.5, 0.3, 0.06, 0.03, 0.01, 2), 15), kVsync60);
    auto passed = judgeSelfCheck(stall, seen);
    ASSERT_TRUE(passed["passed"].get<bool>());
    ASSERT_EQ(passed["bound"], "cpu");
    ASSERT_TRUE(!passed.contains("note"));

    // An instrument that missed the stall: the frame got longer and the
    // verdict did not say why.
    auto missed = judgePerformance(repeated(timing(0.04, 0.01, 33.5, 0.06, 0.03, 0.01, 2), 15), kVsync60);
    auto failed = judgeSelfCheck(stall, missed);
    ASSERT_TRUE(!failed["passed"].get<bool>());
    ASSERT_TRUE(failed["note"].get<std::string>().find("unproven") != std::string::npos);

    // Named CPU, but with less process time than was stalled.
    auto short_of_it = judgePerformance(repeated(timing(0.04, 25.0, 0.3, 0.06, 0.03, 0.01, 2), 15), kVsync60);
    ASSERT_EQ(bound(short_of_it), std::string("cpu"));
    ASSERT_TRUE(!judgeSelfCheck(stall, short_of_it)["passed"].get<bool>());

    ASSERT_TRUE(!judgeSelfCheck(stall, unavailableVerdict("x"))["passed"].get<bool>());
}

struct RegisterPerformanceVerdict {
    RegisterPerformanceVerdict() {
        registerTest("performance_verdict.marks_split_a_frame",
                     test_marks_split_a_frame_into_parts_that_sum_to_it);
        registerTest("performance_verdict.undrawn_frame", test_an_undrawn_frame_keeps_no_render_times);
        registerTest("performance_verdict.foreign_marks_refused", test_marks_from_another_frame_are_refused);
        registerTest("performance_verdict.budget_basis", test_budget_names_where_it_came_from);
        registerTest("performance_verdict.idle_vsync_is_none", test_an_idle_game_waiting_on_vsync_is_not_bound);
        registerTest("performance_verdict.process_is_cpu", test_a_busy_process_step_is_cpu_bound);
        registerTest("performance_verdict.shader_is_gpu",
                     test_a_heavy_shader_is_gpu_bound_though_the_main_thread_waits);
        registerTest("performance_verdict.bodies_are_physics", test_a_pile_of_bodies_is_physics_bound);
        registerTest("performance_verdict.render_cpu_named", test_rendering_on_the_cpu_is_named_as_such);
        registerTest("performance_verdict.contested", test_close_cpu_and_gpu_are_contested);
        registerTest("performance_verdict.headless_never_gpu", test_a_headless_game_is_never_called_gpu_bound);
        registerTest("performance_verdict.no_gpu_times", test_a_renderer_without_gpu_times_does_not_invent_one);
        registerTest("performance_verdict.unexplained_wait",
                     test_a_slow_frame_neither_side_explains_is_not_placed);
        registerTest("performance_verdict.outside_time", test_time_before_the_frame_is_not_given_to_any_part);
        registerTest("performance_verdict.separate_render_thread",
                     test_a_separate_render_thread_is_waited_for_outside_the_frame);
        registerTest("performance_verdict.too_few_frames", test_too_few_frames_decide_nothing);
        registerTest("performance_verdict.one_hitch", test_one_hitch_does_not_decide_the_window);
        registerTest("performance_verdict.stride_keeps_the_hitch", test_a_hitch_the_stride_drops_is_still_named);
        registerTest("performance_verdict.slow_frames_judged_alone", test_each_slow_frame_is_judged_on_its_own_parts);
        registerTest("performance_verdict.slow_frames_fixed_size", test_slow_frames_stay_a_fixed_size);
        registerTest("performance_verdict.log_capacity", test_the_log_keeps_the_whole_window_past_capacity);
        registerTest("performance_verdict.unavailable", test_the_unavailable_verdict_names_its_reason);
        registerTest("performance_verdict.self_check_stall", test_the_self_check_stall_outweighs_the_baseline);
        registerTest("performance_verdict.self_check_judgement",
                     test_the_self_check_passes_only_when_the_verdict_sees_the_stall);
    }
} g_registerPerformanceVerdict;

} // namespace
