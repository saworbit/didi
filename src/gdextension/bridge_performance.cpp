#include "bridge_internal.hpp"
#include "didi/gdextension/godot_bridge.hpp"
#include "didi/gdextension/runtime_bridge.hpp"
#include <optional>
#include <utility>

namespace didi::godot {
using namespace bridge_internal;

// runtime.readProfiler reads these through Performance.get_monitor. The bind
// hash and the monitor enum values are the ones the Phase 7 feasibility gate
// pinned on Godot 4.5.1 and 4.7.2; availability is exactly that bind existing.
// A zero sample is a legitimate reading, so nothing here infers absence from
// a value.
namespace {
constexpr int64_t kPerformanceGetMonitorHash = 1943275655LL;
} // namespace

Result<void> GodotBridge::preflightPerformanceMonitors() {
    auto performance = singleton("Performance");
    if (performance.isErr()) return performance.error();
    return requireMethodBind("Performance", "get_monitor", kPerformanceGetMonitorHash);
}

Result<std::vector<double>> GodotBridge::samplePerformanceMonitors(
    const std::vector<int64_t>& monitors) {
    auto performance = singleton("Performance");
    if (performance.isErr()) return performance.error();
    std::vector<double> values;
    values.reserve(monitors.size());
    for (const auto monitor : monitors) {
        auto argument = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, monitor);
        if (argument.isErr()) return argument.error();
        auto reading = callObject(performance.value(), "Performance", "get_monitor",
                                  kPerformanceGetMonitorHash, {&argument.value()});
        if (reading.isErr()) return reading.error();
        auto value = scalarFromVariant<double>(reading.value(), GDEXTENSION_VARIANT_TYPE_FLOAT);
        if (value.isErr()) return value.error();
        values.push_back(value.value());
    }
    return values;
}

// The frame timer behind runtime.readProfiler's verdict (Q16).
//
// Every bind here carries the same hash on 4.5.1, 4.6.2 and 4.7.2 (probed from
// each binary's --dump-extension-api, 2026-10-09).
namespace {
constexpr const char* kFrameTimerPath = "res://addons/didi/didi_frame_timer.gd";
constexpr int64_t kTimeGetTicksUsecHash = 3905245786LL;
constexpr int64_t kViewportGetRidHash = 2944877500LL;
constexpr int64_t kSetMeasureRenderTimeHash = 1265174801LL;
constexpr int64_t kMeasuredRenderTimeHash = 866169185LL;
constexpr int64_t kFrameSetupTimeHash = 1740695150LL;
constexpr int64_t kIsOnRenderThreadHash = 2240911060LL;

std::optional<VariantValue> g_frame_timer;
// The root viewport's RID, which the render-time calls take.
std::optional<VariantValue> g_frame_timer_viewport;
// Didi switched render-time measurement on for the read that is running.
bool g_render_timing_ours = false;
// Switching measurement off keeps the last times it took, so after a read
// Didi switched it off for, nonzero times are Didi's own and say nothing
// about whether the game measures. Without this, the second read would take
// them for the game's and leave measurement on.
bool g_render_timing_stale_from_didi = false;

Result<double> measuredRenderTime(GDExtensionObjectPtr server, const char* method, VariantValue& rid) {
    auto value = callObject(server, "RenderingServer", method, kMeasuredRenderTimeHash, {&rid});
    if (value.isErr()) return value.error();
    return scalarFromVariant<double>(value.value(), GDEXTENSION_VARIANT_TYPE_FLOAT);
}

Result<void> setRenderTiming(GDExtensionObjectPtr server, VariantValue& rid, bool enabled) {
    auto flag = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, static_cast<GDExtensionBool>(enabled ? 1 : 0));
    if (flag.isErr()) return flag.error();
    auto set = callObject(server, "RenderingServer", "viewport_set_measure_render_time",
                          kSetMeasureRenderTimeHash, {&rid, &flag.value()});
    if (set.isErr()) return set.error();
    return Result<void>();
}

// A setting the frame budget is made of. One the engine cannot give leaves
// the default, which errs towards no screen and no cap.
template <typename T>
void readBudgetPart(GDExtensionObjectPtr object, const char* class_name, const char* method,
                    int64_t hash, const std::vector<const VariantValue*>& arguments, GDExtensionVariantType type,
                    T& into) {
    auto value = callObject(object, class_name, method, hash, arguments);
    if (value.isErr()) return;
    auto scalar = scalarFromVariant<T>(value.value(), type);
    if (scalar.isOk()) into = scalar.value();
}

Result<VariantValue> callFrameTimer(const char* method) {
    auto timer = objectFromVariant(*g_frame_timer);
    if (timer.isErr() || !timer.value()) return Error::internal("The frame timer is gone");
    json empty = json::array();
    auto no_arguments = makeJsonVariant(empty);
    auto name = makeStringName(method);
    if (no_arguments.isErr()) return no_arguments.error();
    if (name.isErr()) return name.error();
    return callObject(timer.value(), "Object", "callv", 1260104456LL,
                      {&name.value(), &no_arguments.value()});
}
} // namespace

Result<runtime::FrameBudgetInputs> GodotBridge::startFrameTimer() {
    stopFrameTimer();
    auto loader = singleton("ResourceLoader");
    if (loader.isErr()) return loader.error();
    auto path = makeString(kFrameTimerPath);
    auto type_hint = makeString("");
    auto cache_mode = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, static_cast<int64_t>(1));
    if (path.isErr() || type_hint.isErr() || cache_mode.isErr()) {
        return Error::internal("Failed to construct the frame timer request");
    }
    // Asked before loading, because loading a file that is not there prints
    // an ERROR. ResourceLoader.exists is 4185558881 on all three lines.
    auto exists = callObject(loader.value(), "ResourceLoader", "exists", 4185558881LL,
                             {&path.value(), &type_hint.value()});
    auto found = exists.isOk() ? scalarFromVariant<GDExtensionBool>(exists.value(), GDEXTENSION_VARIANT_TYPE_BOOL)
                               : Result<GDExtensionBool>(exists.error());
    if (found.isErr() || found.value() == 0) {
        return Error(501, std::string("This project's copy of the addon has no ") + kFrameTimerPath +
                              ", so frames cannot be timed. Copy the built addon folder in again.");
    }
    auto script = callObject(loader.value(), "ResourceLoader", "load", 3358495409LL,
                             {&path.value(), &type_hint.value(), &cache_mode.value()});
    auto script_object = script.isOk() ? objectFromVariant(script.value())
                                       : Result<GDExtensionObjectPtr>(script.error());
    if (script_object.isErr() || !script_object.value()) {
        return Error(501, std::string(kFrameTimerPath) + " did not load, so frames cannot be timed.");
    }
    json empty = json::array();
    auto no_arguments = makeJsonVariant(empty);
    auto new_name = makeStringName("new");
    if (no_arguments.isErr() || new_name.isErr()) return Error::internal("The frame timer could not be made");
    auto timer = callObject(script_object.value(), "Object", "callv", 1260104456LL,
                            {&new_name.value(), &no_arguments.value()});
    auto timer_object = timer.isOk() ? objectFromVariant(timer.value())
                                     : Result<GDExtensionObjectPtr>(timer.error());
    if (timer_object.isErr() || !timer_object.value()) {
        return Error::internal("The frame timer could not be made");
    }
    g_frame_timer = std::move(timer.value());
    auto watching = callFrameTimer("watch");
    if (watching.isErr()) {
        g_frame_timer.reset();
        return watching.error();
    }

    runtime::FrameBudgetInputs budget;
    auto tree = liveSceneTree();
    auto root = tree.isOk() ? liveSceneTreeRoot(tree.value()) : Result<GDExtensionObjectPtr>(tree.error());
    auto server = singleton("RenderingServer");
    if (root.isOk() && server.isOk()) {
        auto rid = callObject(root.value(), "Viewport", "get_viewport_rid", kViewportGetRidHash);
        if (rid.isOk()) {
            g_frame_timer_viewport = std::move(rid.value());
            // Times that were never measured are exactly zero. Nonzero ones
            // the game took mean it measures, and its measurement stays on.
            auto cpu = measuredRenderTime(server.value(), "viewport_get_measured_render_time_cpu",
                                          *g_frame_timer_viewport);
            auto gpu = measuredRenderTime(server.value(), "viewport_get_measured_render_time_gpu",
                                          *g_frame_timer_viewport);
            const bool measured_before = (cpu.isOk() && cpu.value() > 0.0) ||
                                         (gpu.isOk() && gpu.value() > 0.0);
            if (!measured_before || g_render_timing_stale_from_didi) {
                g_render_timing_ours =
                    setRenderTiming(server.value(), *g_frame_timer_viewport, true).isOk();
            }
        }
    }

    auto display = singleton("DisplayServer");
    if (display.isOk()) {
        auto main_window = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, static_cast<int64_t>(0));
        // DisplayServer.SCREEN_OF_MAIN_WINDOW.
        auto main_screen = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, static_cast<int64_t>(-1));
        if (main_window.isOk() && main_screen.isOk()) {
            GDExtensionBool can_draw = 0;
            readBudgetPart(display.value(), "DisplayServer", "window_can_draw", 1051549951LL,
                           {&main_window.value()}, GDEXTENSION_VARIANT_TYPE_BOOL, can_draw);
            budget.can_draw = can_draw != 0;
            readBudgetPart(display.value(), "DisplayServer", "window_get_vsync_mode", 578873795LL,
                           {&main_window.value()}, GDEXTENSION_VARIANT_TYPE_INT, budget.vsync_mode);
            readBudgetPart(display.value(), "DisplayServer", "screen_get_refresh_rate", 909105437LL,
                           {&main_screen.value()}, GDEXTENSION_VARIANT_TYPE_FLOAT, budget.refresh_hz);
        }
    }
    auto engine = singleton("Engine");
    if (engine.isOk()) {
        readBudgetPart(engine.value(), "Engine", "get_max_fps", 3905245786LL, {},
                       GDEXTENSION_VARIANT_TYPE_INT, budget.max_fps);
    }
    // Asked from the main thread, this is false only when rendering has a
    // thread of its own. The mode cannot be read anywhere else: the command
    // line's --render-thread never reaches OS.get_cmdline_args, and when it is
    // given the project setting is never defined (probed on all three lines).
    if (server.isOk()) {
        GDExtensionBool on_render_thread = 1;
        readBudgetPart(server.value(), "RenderingServer", "is_on_render_thread", kIsOnRenderThreadHash,
                       {}, GDEXTENSION_VARIANT_TYPE_BOOL, on_render_thread);
        budget.separate_render_thread = on_render_thread == 0;
    }
    return budget;
}

Result<GodotBridge::FrameTimerReading> GodotBridge::readFrameTimer() {
    if (!g_frame_timer.has_value()) return Error::internal("The frame timer is not running");
    FrameTimerReading reading;
    auto time = singleton("Time");
    if (time.isErr()) return time.error();
    auto now = callObject(time.value(), "Time", "get_ticks_usec", kTimeGetTicksUsecHash);
    if (now.isErr()) return now.error();
    auto now_usec = scalarFromVariant<int64_t>(now.value(), GDEXTENSION_VARIANT_TYPE_INT);
    if (now_usec.isErr()) return now_usec.error();
    reading.now_usec = now_usec.value();

    auto timer = objectFromVariant(*g_frame_timer);
    if (timer.isErr() || !timer.value()) return Error::internal("The frame timer is gone");
    auto name = makeStringName("marks");
    if (name.isErr()) return name.error();
    auto marks = callObject(timer.value(), "Object", "get", 2760726917LL, {&name.value()});
    if (marks.isErr()) return marks.error();
    auto values = variantToJson(marks.value());
    if (values.isErr()) return values.error();
    const auto& array = values.value();
    if (!array.is_array() || array.size() != 4) {
        return Error::internal("The frame timer's marks are not four integers");
    }
    for (const auto& entry : array) {
        if (!entry.is_number_integer()) return Error::internal("The frame timer's marks are not four integers");
    }
    reading.marks.physics_start = array[0].get<int64_t>();
    reading.marks.physics_ticks = array[1].get<int64_t>();
    reading.marks.process_start = array[2].get<int64_t>();
    reading.marks.draw_start = array[3].get<int64_t>();

    if (g_frame_timer_viewport.has_value()) {
        auto server = singleton("RenderingServer");
        if (server.isOk()) {
            auto setup = callObject(server.value(), "RenderingServer", "get_frame_setup_time_cpu",
                                    kFrameSetupTimeHash);
            auto setup_ms = setup.isOk() ? scalarFromVariant<double>(setup.value(), GDEXTENSION_VARIANT_TYPE_FLOAT)
                                         : Result<double>(setup.error());
            auto cpu = measuredRenderTime(server.value(), "viewport_get_measured_render_time_cpu",
                                          *g_frame_timer_viewport);
            auto gpu = measuredRenderTime(server.value(), "viewport_get_measured_render_time_gpu",
                                          *g_frame_timer_viewport);
            reading.render_cpu_ms = (setup_ms.isOk() ? setup_ms.value() : 0.0) +
                                    (cpu.isOk() ? cpu.value() : 0.0);
            reading.gpu_ms = gpu.isOk() ? gpu.value() : 0.0;
        }
    }
    return reading;
}

Result<void> GodotBridge::setFrameTimerStall(int64_t stall_usec) {
    if (!g_frame_timer.has_value()) return Error::internal("The frame timer is not running");
    auto timer = objectFromVariant(*g_frame_timer);
    if (timer.isErr() || !timer.value()) return Error::internal("The frame timer is gone");
    auto name = makeStringName("stall_usec");
    auto value = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, stall_usec);
    if (name.isErr()) return name.error();
    if (value.isErr()) return value.error();
    auto set = callObject(timer.value(), "Object", "set", 3776071444LL, {&name.value(), &value.value()});
    if (set.isErr()) return set.error();
    return Result<void>();
}

void GodotBridge::stopFrameTimer(bool engine_exiting) {
    // Freeing the RefCounted would disconnect its signals on its own; unwatch
    // also clears a stall, so nothing holds a frame up after the read.
    if (g_frame_timer.has_value()) {
        if (!engine_exiting) (void)callFrameTimer("unwatch");
        g_frame_timer.reset();
    }
    // The main loop's shutdown callback runs after the scene tree has freed
    // the root viewport, so switching measurement off there named a viewport
    // that was gone: "Parameter "viewport" is null" from a game stopped while a
    // read was starting (CI's 4.6.2 runner, then about one stop in ten here).
    // The flag goes with the viewport.
    if (!engine_exiting && g_render_timing_ours && g_frame_timer_viewport.has_value()) {
        auto server = singleton("RenderingServer");
        if (server.isOk() && setRenderTiming(server.value(), *g_frame_timer_viewport, false).isOk()) {
            g_render_timing_stale_from_didi = true;
        }
    }
    g_render_timing_ours = false;
    g_frame_timer_viewport.reset();
}

} // namespace didi::godot
