#pragma once

#include <string>

namespace didi {
namespace godot {

// Arms a process wide capture of fatal exceptions so a crash inside the editor
// leaves a readable report instead of a bare exit code. Godot's own handler
// prints nothing for the crash this was written for, and the extension is
// already loaded in the process that dies, so this is the one place a stack
// can still be taken.
//
// DIDI_CRASH_CAPTURE_DIR wins when it names a directory that exists, which is
// how the harness points reports at its build tree. Otherwise the reports go
// to fallback_directory, which is created if needed. Passing neither leaves the
// handler uninstalled.
//
// Returns true when a handler was installed, and is a no-op once one is.
bool armCrashCapture(const std::string& fallback_directory = {});

// Puts the handler back on top if something else has taken the top level
// filter since. Godot installs its own after the extension loads, which
// silently replaced ours and left a crash reported by the engine's handler
// with no symbols instead of by this one. Cheap enough to call every frame.
void reassertCrashCapture();

// Removes the handler installed by armCrashCapture. Returns true when one was
// removed.
bool disarmCrashCapture();

// Where a fatal exception will be written. Empty when not armed.
std::string crashReportPath();

#if defined(_WIN32)
// Writes the report for an exception that has already been caught. The handler
// calls this on a fault; a test calls it with a record it built itself, so the
// format and the stack walk are covered without needing a real crash.
// Takes EXCEPTION_POINTERS, as void* to keep windows.h out of this header.
bool writeCrashReport(const void* exception_pointers);
#endif

} // namespace godot
} // namespace didi
