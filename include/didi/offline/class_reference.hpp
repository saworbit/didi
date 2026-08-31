#pragma once

#include "didi/common/types.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace didi::offline {

// Offline reflection over Godot's own API dump.
//
// reflectClass used to answer from a hand-written snapshot of about eighteen
// classes and return is_known_class false for everything else, which is most of
// the engine. The repository already pins extension_api.json, so the answer was
// there the whole time; what it needed was to be small enough to ship and cheap
// enough to read.
//
// tools/generate_class_reference.py trims the 6.9 MB dump to the parts this
// answers with, and the result is loaded once on first use and cached. Nothing
// is parsed if no caller ever asks.
class ClassReference {
public:
    // The reference for this process. Loading is attempted once; a failure is
    // remembered so a missing file does not cost a filesystem probe per call.
    static const ClassReference& instance();

    // Where the loader will look, in order, so callers and tests can say what
    // they mean rather than depending on the working directory:
    //   1. DIDI_CLASS_REFERENCE, if set
    //   2. next to the running executable
    //   3. the build tree's generated directory, for a development run
    static std::optional<std::filesystem::path> resolveReferencePath();

    // Overrides the search for the lifetime of the process. Used by tests, and
    // by anyone shipping the reference somewhere unusual.
    static void setReferencePathForTesting(std::optional<std::filesystem::path> path);

    bool loaded() const { return m_loaded; }
    const std::string& apiVersion() const { return m_apiVersion; }
    size_t size() const { return m_classes.is_object() ? m_classes.size() : 0; }

    // The trimmed record for one class, or nullptr when the reference is
    // unavailable or does not name it.
    const json* find(const std::string& class_name) const;

private:
    ClassReference();

    bool m_loaded{false};
    std::string m_apiVersion;
    json m_classes = json::object();
};

// Absolute directory holding the running executable, or empty when the platform
// refuses to say. Kept here because the class reference is its only caller.
std::filesystem::path executableDirectory();

} // namespace didi::offline
