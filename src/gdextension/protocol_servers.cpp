#include "didi/gdextension/protocol_servers.hpp"

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#elif defined(__APPLE__)
#include <crt_externs.h>
#else
#include <fstream>
#include <iterator>
#endif

#include <cctype>
#include <cstdint>

namespace didi::godot {
namespace {

#if defined(_WIN32)
std::string utf8From(const wchar_t* value) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string out(static_cast<size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, out.data(), size, nullptr, nullptr);
    return out;
}
#endif

std::string trimmed(const std::string& value) {
    size_t begin = 0;
    size_t end = value.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
    return value.substr(begin, end - begin);
}

// String::to_int: the digits before any '.', anything else skipped, and a '-'
// before the first digit negates.
int godotToInt(const std::string& value) {
    int64_t number = 0;
    int64_t sign = 1;
    for (const char c : value) {
        if (c == '.') break;
        if (c >= '0' && c <= '9') {
            if (number > 100000000) return -1;
            number = number * 10 + (c - '0');
        } else if (c == '-' && number == 0) {
            sign = -sign;
        }
    }
    return static_cast<int>(number * sign);
}

} // namespace

std::vector<std::string> processArguments() {
    std::vector<std::string> arguments;
#if defined(_WIN32)
    // The same call Godot's own entry point makes, so the split is Godot's.
    int count = 0;
    LPWSTR* values = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!values) return arguments;
    for (int index = 1; index < count; ++index) arguments.push_back(utf8From(values[index]));
    LocalFree(values);
#elif defined(__APPLE__)
    const int count = *_NSGetArgc();
    char** values = *_NSGetArgv();
    for (int index = 1; index < count && values; ++index) {
        if (values[index]) arguments.emplace_back(values[index]);
    }
#else
    std::ifstream input("/proc/self/cmdline", std::ios::binary);
    if (!input.is_open()) return arguments;
    const std::string contents{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    size_t start = 0;
    bool first = true;
    while (start < contents.size()) {
        auto end = contents.find('\0', start);
        if (end == std::string::npos) end = contents.size();
        if (!first) arguments.push_back(contents.substr(start, end - start));
        first = false;
        start = end + 1;
    }
#endif
    return arguments;
}

ProtocolPortOverrides protocolPortOverrides(const std::vector<std::string>& arguments) {
    ProtocolPortOverrides overrides;
    for (size_t index = 0; index < arguments.size(); ++index) {
        const auto argument = trimmed(arguments[index]);
        if (argument == "--" || argument == "++") break;
        const bool language_server = argument == "--lsp-port";
        if (!language_server && argument != "--dap-port") continue;
        if (index + 1 >= arguments.size()) break;
        const int port = godotToInt(trimmed(arguments[++index]));
        (language_server ? overrides.language_server : overrides.debug_adapter) = port;
    }
    return overrides;
}

std::optional<int> fixedFpsArgument(const std::vector<std::string>& arguments) {
    std::optional<int> rate;
    for (size_t index = 0; index < arguments.size(); ++index) {
        const auto argument = trimmed(arguments[index]);
        if (argument == "--" || argument == "++") break;
        if (argument != "--fixed-fps") continue;
        if (index + 1 >= arguments.size()) break;
        const int value = godotToInt(trimmed(arguments[++index]));
        rate = value > 0 ? std::optional<int>(value) : std::nullopt;
    }
    return rate;
}

bool startPausedRequested(const std::vector<std::string>& arguments) {
    bool user_arguments = false;
    for (const auto& raw : arguments) {
        const auto argument = trimmed(raw);
        if (!user_arguments) {
            user_arguments = argument == "--" || argument == "++";
            continue;
        }
        if (argument == kStartPausedArgument) return true;
    }
    return false;
}

} // namespace didi::godot
