#include "didi/gdextension/crash_capture.hpp"

#if defined(_WIN32)

#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>

#include "didi/common/project_path.hpp"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

#ifndef STATUS_STACK_BUFFER_OVERRUN
#define STATUS_STACK_BUFFER_OVERRUN 0xC0000409L
#endif

namespace didi {
namespace godot {

namespace {

using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                          PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION,
                                          PMINIDUMP_CALLBACK_INFORMATION);

// Everything the handler needs is resolved while the process is still healthy.
// A crashed process is a bad place to allocate, open a library, or format a
// path, so none of that happens after the fault.
std::string g_report_path;
std::wstring g_report_path_wide;
std::wstring g_dump_path_wide;
DWORD g_main_thread_id = 0;
HMODULE g_dbghelp = nullptr;
MiniDumpWriteDumpFn g_write_dump = nullptr;
LPTOP_LEVEL_EXCEPTION_FILTER g_previous_filter = nullptr;
bool g_armed = false;
std::atomic<int> g_reports{0};
std::atomic<bool> g_dump_written{false};

// A handled fault would otherwise fill the file, and the report after the cap
// is the one nobody reads. Four is enough to see a pattern and few enough to
// keep the last one findable.
constexpr int kMaxReports = 4;

// A fixed buffer, because the process this runs in has already failed and the
// allocator may be part of why. Static rather than automatic so a stack
// overflow still has somewhere to write.
class Report {
public:
    void reset() { m_used = 0; }

    void text(const char* value) {
        while (value && *value && m_used + 1 < sizeof(m_data)) m_data[m_used++] = *value++;
    }

    void hex(unsigned long long value, int digits) {
        char out[17];
        if (digits < 1) digits = 1;
        if (digits > 16) digits = 16;
        for (int index = digits - 1; index >= 0; --index) {
            out[index] = "0123456789abcdef"[value & 0xFULL];
            value >>= 4;
        }
        out[digits] = 0;
        text(out);
    }

    void dec(unsigned long long value) {
        char out[21];
        int index = 20;
        out[index] = 0;
        if (value == 0) out[--index] = '0';
        while (value > 0 && index > 0) {
            out[--index] = static_cast<char>('0' + (value % 10ULL));
            value /= 10ULL;
        }
        text(out + index);
    }

    void line() { text("\r\n"); }

    const char* data() const { return m_data; }
    DWORD size() const { return static_cast<DWORD>(m_used); }

private:
    char m_data[32768]{};
    size_t m_used{0};
};

Report g_report;

const char* exceptionName(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION: return "ACCESS_VIOLATION";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_PRIV_INSTRUCTION: return "PRIV_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR: return "IN_PAGE_ERROR";
        case EXCEPTION_STACK_OVERFLOW: return "STACK_OVERFLOW";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_INT_DIVIDE_BY_ZERO: return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "NONCONTINUABLE_EXCEPTION";
        case EXCEPTION_INVALID_DISPOSITION: return "INVALID_DISPOSITION";
        case static_cast<DWORD>(STATUS_STACK_BUFFER_OVERRUN): return "STACK_BUFFER_OVERRUN";
        default: return "UNKNOWN";
    }
}

bool isFatal(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_IN_PAGE_ERROR:
        case EXCEPTION_STACK_OVERFLOW:
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_NONCONTINUABLE_EXCEPTION:
        case EXCEPTION_INVALID_DISPOSITION:
        case static_cast<DWORD>(STATUS_STACK_BUFFER_OVERRUN):
            return true;
        default:
            return false;
    }
}

// Reads through the kernel so an address that is not mapped returns false
// instead of faulting inside the handler.
bool safeRead(DWORD64 address, void* out, size_t size) {
    if (address == 0) return false;
    SIZE_T read = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address), out, size,
                             &read) != 0 &&
           read == size;
}

const wchar_t* fileNameOf(const wchar_t* path) {
    // 92 is the path separator. Written as a value so this line survives every
    // generator and shell between here and the file.
    const wchar_t separator = static_cast<wchar_t>(92);
    const wchar_t* name = path;
    for (const wchar_t* cursor = path; *cursor; ++cursor) {
        if (*cursor == separator || *cursor == L'/') name = cursor + 1;
    }
    return name;
}

bool narrow(const wchar_t* wide, char* out, int out_size) {
    if (!wide || !out || out_size <= 0) return false;
    out[0] = 0;
    return WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, out_size, nullptr, nullptr) > 0;
}

bool moduleForAddress(DWORD64 address, char* name, int name_size, DWORD64* offset,
                      HMODULE* out_module) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &module) ||
        !module) {
        return false;
    }
    wchar_t path[MAX_PATH]{};
    if (GetModuleFileNameW(module, path, MAX_PATH) == 0) return false;
    if (!narrow(fileNameOf(path), name, name_size)) return false;
    *offset = address - reinterpret_cast<DWORD64>(module);
    if (out_module) *out_module = module;
    return true;
}

void writeAddress(Report& report, DWORD64 address) {
    report.text("0x");
    report.hex(address, 16);
    char name[MAX_PATH]{};
    DWORD64 offset = 0;
    if (moduleForAddress(address, name, static_cast<int>(sizeof(name)), &offset, nullptr)) {
        report.text("  ");
        report.text(name);
        report.text("+0x");
        report.hex(offset, 8);
    } else {
        report.text("  <address is in no loaded module>");
    }
}

void writeFrame(Report& report, int index, DWORD64 address) {
    report.text("  #");
    if (index < 10) report.text("0");
    report.dec(static_cast<unsigned long long>(index));
    report.text(" ");
    writeAddress(report, address);
    report.line();
}

// Unwinds with the same tables the operating system uses, so no symbols and no
// dbghelp are needed to get the frames. Module plus offset is enough to say
// which binary is on the stack, which is the question a bare exit code cannot
// answer.
void writeStack(Report& report, const CONTEXT& start) {
#if defined(_M_X64)
    CONTEXT context = start;
    for (int frame = 0; frame < 64; ++frame) {
        if (context.Rip == 0) break;
        writeFrame(report, frame, context.Rip);

        DWORD64 image_base = 0;
        PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
        if (function) {
            PVOID handler_data = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context,
                             &handler_data, &establisher, nullptr);
            continue;
        }
        // A leaf function has no unwind data. Nothing in its frame has moved
        // yet, so the return address is still on the top of the stack.
        DWORD64 return_address = 0;
        if (!safeRead(context.Rsp, &return_address, sizeof(return_address))) break;
        if (return_address == 0) break;
        context.Rip = return_address;
        context.Rsp += sizeof(DWORD64);
    }
#else
    writeFrame(report, 0, static_cast<DWORD64>(start.Pc));
    report.text("  <full stack walking is implemented for x64 only>");
    report.line();
#endif
}

void writeModules(Report& report) {
    report.text("loaded modules:");
    report.line();
    // Reads the loader's own list rather than snapshotting the process, which
    // is the one of the two that does not allocate or take the loader lock in
    // a way a crashed process may not survive.
    HMODULE modules[512];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed)) {
        report.text("  <module list unavailable>");
        report.line();
        return;
    }
    const DWORD count = needed / sizeof(HMODULE);
    const DWORD limit = count < 512 ? count : 512;
    for (DWORD index = 0; index < limit; ++index) {
        wchar_t path[MAX_PATH]{};
        if (GetModuleFileNameW(modules[index], path, MAX_PATH) == 0) continue;
        MODULEINFO info{};
        const bool has_info =
            GetModuleInformation(GetCurrentProcess(), modules[index], &info, sizeof(info)) != 0;
        report.text("  0x");
        report.hex(reinterpret_cast<DWORD64>(modules[index]), 16);
        report.text("  size 0x");
        report.hex(has_info ? info.SizeOfImage : 0, 8);
        report.text("  ");
        char narrowed[MAX_PATH]{};
        if (narrow(path, narrowed, static_cast<int>(sizeof(narrowed)))) report.text(narrowed);
        report.line();
    }
}

void writeAll(HANDLE handle, const char* data, DWORD size) {
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    while (written < size) {
        DWORD chunk = 0;
        if (!WriteFile(handle, data + written, size - written, &chunk, nullptr) || chunk == 0) return;
        written += chunk;
    }
}

void writeMiniDump(EXCEPTION_POINTERS* pointers) {
    if (!g_write_dump || g_dump_path_wide.empty()) return;
    bool expected = false;
    if (!g_dump_written.compare_exchange_strong(expected, true)) return;
    HANDLE file = CreateFileW(g_dump_path_wide.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    MINIDUMP_EXCEPTION_INFORMATION information{};
    information.ThreadId = GetCurrentThreadId();
    information.ExceptionPointers = pointers;
    information.ClientPointers = FALSE;
    // Thread stacks and the module list, and not the heap. That is what a
    // debugger needs to place the fault, and it keeps the file small enough to
    // travel as a build artifact.
    const auto type =
        static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
    g_write_dump(GetCurrentProcess(), GetCurrentProcessId(), file, type, &information, nullptr,
                 nullptr);
    CloseHandle(file);
}

} // namespace

bool writeCrashReport(const void* exception_pointers) {
    auto* pointers =
        const_cast<EXCEPTION_POINTERS*>(static_cast<const EXCEPTION_POINTERS*>(exception_pointers));
    if (!pointers || !pointers->ExceptionRecord || !pointers->ContextRecord) return false;
    if (g_report_path_wide.empty()) return false;

    const EXCEPTION_RECORD& record = *pointers->ExceptionRecord;
    Report& report = g_report;
    report.reset();

    report.text("DIDI CRASH CAPTURE");
    report.line();
    report.text("exception: 0x");
    report.hex(record.ExceptionCode, 8);
    report.text("  ");
    report.text(exceptionName(record.ExceptionCode));
    report.line();
    report.text("flags: 0x");
    report.hex(record.ExceptionFlags, 8);
    report.text("  (0 is first chance, 1 is non continuable)");
    report.line();
    report.text("address: ");
    writeAddress(report, reinterpret_cast<DWORD64>(record.ExceptionAddress));
    report.line();
    report.text("process: ");
    report.dec(GetCurrentProcessId());
    report.line();
    report.text("thread: ");
    report.dec(GetCurrentThreadId());
    report.text("  main thread: ");
    report.dec(g_main_thread_id);
    report.text("  on main thread: ");
    report.text(GetCurrentThreadId() == g_main_thread_id ? "yes" : "no");
    report.line();
    if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
        record.ExceptionCode == EXCEPTION_IN_PAGE_ERROR) {
        if (record.NumberParameters >= 2) {
            report.text("access: ");
            const ULONG_PTR operation = record.ExceptionInformation[0];
            report.text(operation == 0   ? "read"
                        : operation == 1 ? "write"
                        : operation == 8 ? "execute"
                                         : "unknown");
            report.text(" at 0x");
            report.hex(record.ExceptionInformation[1], 16);
            report.line();
        }
    }
    report.text("stack:");
    report.line();
    writeStack(report, *pointers->ContextRecord);
    // Everything to here is what a reader needs at the moment they see the exit
    // code. The module list after it is for working out an offset later, so the
    // file keeps it and the console does not.
    const DWORD console_size = report.size();
    writeModules(report);
    report.text("END DIDI CRASH CAPTURE");
    report.line();
    report.line();

    HANDLE file = CreateFileW(g_report_path_wide.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        SetFilePointer(file, 0, nullptr, FILE_END);
        writeAll(file, report.data(), report.size());
        FlushFileBuffers(file);
        CloseHandle(file);
    }
    // Also to stderr, because that is the stream the harness already keeps and
    // the one a reader is looking at when they see the exit code.
    writeAll(GetStdHandle(STD_ERROR_HANDLE), report.data(), console_size);
    return file != INVALID_HANDLE_VALUE;
}

namespace {

// Runs only for an exception nothing else handled, which is to say a process
// that is already going down. A vectored handler was the first thing tried and
// it is the wrong instrument: it fires on exceptions the engine raises and
// handles as a matter of course, including during teardown while the loader
// lock is held, and doing this much work there hangs the shutdown.
//
// The exit code in #285 is the exception status itself, which is what an
// unhandled exception leaves behind, so this is the hook that fault reaches.
LONG WINAPI onUnhandledException(EXCEPTION_POINTERS* pointers) {
    if (pointers && pointers->ExceptionRecord &&
        isFatal(pointers->ExceptionRecord->ExceptionCode) &&
        g_reports.fetch_add(1) < kMaxReports) {
        writeCrashReport(pointers);
        writeMiniDump(pointers);
    }
    // Whatever the engine installed before this still decides what happens.
    // Reporting must not change the outcome, only record it.
    if (g_previous_filter) return g_previous_filter(pointers);
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

bool armCrashCapture(const std::string& fallback_directory) {
    if (g_armed) return true;
    std::error_code error;
    std::filesystem::path root;
    const char* configured = std::getenv("DIDI_CRASH_CAPTURE_DIR");
    if (configured && *configured && std::filesystem::is_directory(configured, error)) {
        root = paths::projectPathFromUtf8(configured);
    } else if (!fallback_directory.empty()) {
        // Created rather than required, because the point of the fallback is
        // that an ordinary editor session leaves a report without anyone
        // having set anything up first.
        const auto candidate = paths::projectPathFromUtf8(fallback_directory);
        std::filesystem::create_directories(candidate, error);
        if (error || !std::filesystem::is_directory(candidate, error)) return false;
        root = candidate;
    } else {
        return false;
    }

    const auto base = root / ("godot_crash_" + std::to_string(GetCurrentProcessId()));
    const auto report = std::filesystem::path(base).replace_extension(".log");
    const auto dump = std::filesystem::path(base).replace_extension(".dmp");
    g_report_path = report.string();
    g_report_path_wide = report.wstring();
    g_dump_path_wide = dump.wstring();
    g_main_thread_id = GetCurrentThreadId();
    g_reports.store(0);
    g_dump_written.store(false);

    // Resolved now rather than after the fault, because loading a library from
    // inside a crash handler is its own way to lose the report.
    if (!g_dbghelp) g_dbghelp = LoadLibraryW(L"dbghelp.dll");
    if (g_dbghelp && !g_write_dump) {
        g_write_dump =
            reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(g_dbghelp, "MiniDumpWriteDump"));
    }

    // First in the chain, so the report is taken before anything else has a
    // chance to swallow the exception or unwind past the frames that matter.
    g_previous_filter = SetUnhandledExceptionFilter(onUnhandledException);
    g_armed = true;
    return true;
}

void reassertCrashCapture() {
    if (!g_armed) return;
    // There is no way to read the current filter without setting one, so the
    // swap is the read. If something else had taken the top level it becomes
    // the filter this one chains to, which keeps the engine's own handler in
    // the chain rather than displacing it.
    LPTOP_LEVEL_EXCEPTION_FILTER previous = SetUnhandledExceptionFilter(onUnhandledException);
    if (previous != onUnhandledException) g_previous_filter = previous;
}

bool disarmCrashCapture() {
    if (!g_armed) return false;
    SetUnhandledExceptionFilter(g_previous_filter);
    g_previous_filter = nullptr;
    g_armed = false;
    g_report_path.clear();
    g_report_path_wide.clear();
    g_dump_path_wide.clear();
    return true;
}

std::string crashReportPath() { return g_report_path; }

} // namespace godot
} // namespace didi

#else

namespace didi {
namespace godot {

bool armCrashCapture(const std::string&) { return false; }
void reassertCrashCapture() {}
bool disarmCrashCapture() { return false; }
std::string crashReportPath() { return {}; }

} // namespace godot
} // namespace didi

#endif
