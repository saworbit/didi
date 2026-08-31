#include "didi/offline/class_reference.hpp"

#include "didi/common/logger.hpp"
#include "didi/common/project_path.hpp"

#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

namespace didi::offline {
namespace {

constexpr const char* kReferenceFileName = "didi_class_reference.json";

std::mutex& overrideMutex() {
    static std::mutex mutex;
    return mutex;
}

std::optional<std::filesystem::path>& overridePath() {
    static std::optional<std::filesystem::path> path;
    return path;
}

} // namespace

std::filesystem::path executableDirectory() {
    std::error_code error;
#if defined(_WIN32)
    std::wstring buffer(MAX_PATH, L'\0');
    for (int attempt = 0; attempt < 5; ++attempt) {
        const DWORD written = GetModuleFileNameW(nullptr, buffer.data(),
                                                 static_cast<DWORD>(buffer.size()));
        if (written == 0) return {};
        if (written < buffer.size()) {
            buffer.resize(written);
            return std::filesystem::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
    return {};
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) return {};
    buffer.resize(std::char_traits<char>::length(buffer.c_str()));
    const auto resolved = std::filesystem::weakly_canonical(buffer, error);
    return error ? std::filesystem::path(buffer).parent_path() : resolved.parent_path();
#else
    const auto resolved = std::filesystem::read_symlink("/proc/self/exe", error);
    if (error) return {};
    return resolved.parent_path();
#endif
}

std::optional<std::filesystem::path> ClassReference::resolveReferencePath() {
    {
        std::lock_guard<std::mutex> lock(overrideMutex());
        if (overridePath().has_value()) return overridePath();
    }

    std::error_code error;
    if (const char* configured = std::getenv("DIDI_CLASS_REFERENCE");
        configured && *configured) {
        auto path = paths::projectPathFromUtf8(configured);
        if (std::filesystem::is_regular_file(path, error) && !error) return path;
    }

    const auto directory = executableDirectory();
    if (!directory.empty()) {
        // Next to the binary, which is where the build copies it and where the
        // release archive puts it.
        auto beside = directory / kReferenceFileName;
        if (std::filesystem::is_regular_file(beside, error) && !error) return beside;
        // A development run out of build/Release still finds the generated copy.
        auto generated = directory.parent_path() / "generated" / kReferenceFileName;
        if (std::filesystem::is_regular_file(generated, error) && !error) return generated;
    }
    return std::nullopt;
}

void ClassReference::setReferencePathForTesting(std::optional<std::filesystem::path> path) {
    std::lock_guard<std::mutex> lock(overrideMutex());
    overridePath() = std::move(path);
}

ClassReference::ClassReference() {
    const auto path = resolveReferencePath();
    if (!path.has_value()) {
        DIDI_LOG_DEBUG("CLASS_REFERENCE",
                       "No offline class reference found; reflection falls back to the "
                       "built-in snapshot");
        return;
    }

    std::ifstream input(*path, std::ios::binary);
    if (!input) {
        DIDI_LOG_WARN("CLASS_REFERENCE", "Offline class reference is unreadable: ",
                      paths::projectPathToUtf8(*path));
        return;
    }
    std::ostringstream contents;
    contents << input.rdbuf();

    // Parse into a local document, keep only the classes map, and let the rest
    // go. Holding the whole parsed document would cost several times what the
    // answers do.
    auto document = json::parse(contents.str(), nullptr, false);
    if (document.is_discarded() || !document.is_object() ||
        !document.contains("classes") || !document["classes"].is_object()) {
        DIDI_LOG_WARN("CLASS_REFERENCE", "Offline class reference is malformed: ",
                      paths::projectPathToUtf8(*path));
        return;
    }
    m_apiVersion = document.value("api_version", std::string("unknown"));
    m_classes = std::move(document["classes"]);
    m_loaded = true;
    DIDI_LOG_INFO("CLASS_REFERENCE", "Loaded ", m_classes.size(),
                  " offline class definitions for ", m_apiVersion);
}

const ClassReference& ClassReference::instance() {
    static ClassReference reference;
    return reference;
}

const json* ClassReference::find(const std::string& class_name) const {
    if (!m_loaded) return nullptr;
    const auto found = m_classes.find(class_name);
    return found == m_classes.end() ? nullptr : &found.value();
}

} // namespace didi::offline
