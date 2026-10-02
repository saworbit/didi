#include "didi/setup/build_identity.hpp"

#include "didi/common/project_path.hpp"
#include "didi/common/version.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <set>
#include <tuple>
#include <vector>

namespace didi::setup {
namespace {

bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isLowerHex(char c) { return isDigit(c) || (c >= 'a' && c <= 'f'); }

// A version component as CMake writes it: one to four digits, no sign.
std::optional<int> component(std::string_view text) {
    if (text.empty() || text.size() > 4) return std::nullopt;
    int value = 0;
    for (char c : text) {
        if (!isDigit(c)) return std::nullopt;
        value = value * 10 + (c - '0');
    }
    return value;
}

// The tail after the `+`: commit, a dot, then the configure stamp. Returns its
// length when `bytes` starts with one, else 0.
size_t tailLength(std::string_view bytes) {
    size_t commit = 0;
    if (bytes.substr(0, 5) == "nogit") {
        commit = 5;
    } else {
        while (commit < bytes.size() && commit < 12 && isLowerHex(bytes[commit])) ++commit;
        if (commit != 12) return 0;
    }
    // .YYYYMMDDTHHMMSS
    constexpr size_t kStamp = 1 + 8 + 1 + 6;
    if (bytes.size() < commit + kStamp || bytes[commit] != '.') return 0;
    const auto stamp = bytes.substr(commit + 1, kStamp - 1);
    for (size_t i = 0; i < stamp.size(); ++i) {
        if (i == 8 ? stamp[i] != 'T' : !isDigit(stamp[i])) return 0;
    }
    const size_t length = commit + kStamp;
    // A digit straight after would make this the front of something longer.
    if (bytes.size() > length && isDigit(bytes[length])) return 0;
    return length;
}

}  // namespace

std::optional<BuildId> parseBuildId(std::string_view text) {
    const auto plus = text.find('+');
    if (plus == std::string_view::npos) return std::nullopt;
    const auto version = text.substr(0, plus);
    const auto first = version.find('.');
    const auto second = first == std::string_view::npos ? first : version.find('.', first + 1);
    if (second == std::string_view::npos || version.find('.', second + 1) != std::string_view::npos) {
        return std::nullopt;
    }
    const auto major = component(version.substr(0, first));
    const auto minor = component(version.substr(first + 1, second - first - 1));
    const auto patch = component(version.substr(second + 1));
    if (!major || !minor || !patch) return std::nullopt;
    const auto tail = text.substr(plus + 1);
    if (tailLength(tail) != tail.size()) return std::nullopt;

    BuildId id;
    id.text = std::string(text);
    id.major = *major;
    id.minor = *minor;
    id.patch = *patch;
    const auto dot = tail.find('.');
    id.commit = std::string(tail.substr(0, dot));
    id.stamp = std::string(tail.substr(dot + 1));
    return id;
}

BuildOrder compareBuilds(const BuildId& subject, const BuildId& reference) {
    if (subject.text == reference.text) return BuildOrder::Same;
    const auto left = std::tie(subject.major, subject.minor, subject.patch, subject.stamp);
    const auto right = std::tie(reference.major, reference.minor, reference.patch, reference.stamp);
    if (left < right) return BuildOrder::Older;
    if (right < left) return BuildOrder::Newer;
    return BuildOrder::Unknown;
}

const char* buildOrderWord(BuildOrder order) {
    switch (order) {
        case BuildOrder::Same: return "the same build as";
        case BuildOrder::Older: return "older than";
        case BuildOrder::Newer: return "newer than";
        case BuildOrder::Unknown: break;
    }
    return "not comparable with";
}

Result<std::optional<BuildId>> readBuildIdFromBinary(const std::filesystem::path& binary) {
    std::error_code error;
    const auto size = std::filesystem::file_size(binary, error);
    if (error) {
        return Error::notFound("Cannot read " + paths::projectPathToUtf8(binary) + ": " + error.message());
    }
    // A Didi binary is a few megabytes. Anything this size is not one, and
    // reading it whole to find out would be the wrong cost.
    constexpr std::uintmax_t kLargest = 512ull * 1024 * 1024;
    if (size > kLargest) {
        return Error::invalidArgument(paths::projectPathToUtf8(binary) +
                                      " is larger than any Didi binary, so it was not read");
    }
    std::ifstream input(binary, std::ios::binary);
    if (!input) return Error::notFound("Cannot open " + paths::projectPathToUtf8(binary));
    std::string bytes(static_cast<size_t>(size), '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (static_cast<std::uintmax_t>(input.gcount()) != size) {
        return Error::internal("Short read from " + paths::projectPathToUtf8(binary));
    }

    // Every `+` is a candidate. Walk back over `digits.digits.digits` and
    // forward over the commit and stamp; std::regex over a library this size
    // is slow on MSVC and has blown its stack on long inputs elsewhere (#661).
    std::set<std::string> found;
    for (size_t plus = bytes.find('+'); plus != std::string::npos; plus = bytes.find('+', plus + 1)) {
        const size_t tail = tailLength(std::string_view(bytes).substr(plus + 1));
        if (tail == 0) continue;
        size_t start = plus;
        int dots = 0;
        while (start > 0 && (isDigit(bytes[start - 1]) || bytes[start - 1] == '.') && plus - start < 14) {
            if (bytes[start - 1] == '.') ++dots;
            --start;
        }
        if (dots != 2) continue;
        // Whatever precedes the version must not be part of it.
        if (start > 0 && (std::isalnum(static_cast<unsigned char>(bytes[start - 1])) != 0)) continue;
        const auto candidate = std::string_view(bytes).substr(start, plus - start + 1 + tail);
        if (parseBuildId(candidate)) found.insert(std::string(candidate));
    }
    if (found.empty()) return std::optional<BuildId>{};
    if (found.size() > 1) {
        std::string listed;
        for (const auto& text : found) listed += (listed.empty() ? "" : ", ") + text;
        return Error::invalidArgument(paths::projectPathToUtf8(binary) +
                                      " carries more than one build id (" + listed +
                                      "), so which build it is cannot be said");
    }
    return parseBuildId(*found.begin());
}

BuildId ownBuildId() {
    if (auto parsed = parseBuildId(kBuildId)) return *parsed;
    // A build id CMakeLists.txt did not compose (a hand-edited build_id.cpp)
    // still identifies this binary exactly; it just cannot be ordered.
    BuildId id;
    id.text = kBuildId;
    return id;
}

}  // namespace didi::setup
