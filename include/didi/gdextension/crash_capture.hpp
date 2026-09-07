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
// Does nothing unless DIDI_CRASH_CAPTURE_DIR names a directory that exists.
// Returns true when a handler was installed.
bool armCrashCapture();

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
