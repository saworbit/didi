#pragma once

#include "didi/common/project_path.hpp"
#include "didi/common/secure_random.hpp"
#include "didi/common/types.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <system_error>

namespace didi::files {

// A write that is fully prepared but not yet in place.
//
// Staging every file of a multi-file change before replacing any of them is as
// close as a filesystem gets to a transaction. Everything that can fail on the
// way to disk -- a full volume, a quota, a permission, an unwritable directory
// -- has already either failed or not by the time the first destination is
// touched, so a change cannot stop half applied because the tenth file was the
// one that could not be written. What remains is the renames themselves, which
// are metadata operations on the same volume; a caller that needs to describe
// the residual window should describe that and not claim more.
//
// Discarding is the default. A staged write that is never committed removes its
// temporary when it goes out of scope, including on the error path that
// abandoned it.
class StagedWrite {
public:
    StagedWrite() = default;
    StagedWrite(std::filesystem::path target, std::filesystem::path temporary)
        : m_target(std::move(target)), m_temporary(std::move(temporary)), m_staged(true) {}
    StagedWrite(const StagedWrite&) = delete;
    StagedWrite& operator=(const StagedWrite&) = delete;
    StagedWrite(StagedWrite&& other) noexcept { *this = std::move(other); }
    StagedWrite& operator=(StagedWrite&& other) noexcept {
        if (this != &other) {
            discard();
            m_target = std::move(other.m_target);
            m_temporary = std::move(other.m_temporary);
            m_staged = other.m_staged;
            other.m_staged = false;
        }
        return *this;
    }
    ~StagedWrite() { discard(); }

    const std::filesystem::path& target() const { return m_target; }

    Result<void> commit() {
        if (!m_staged) return Error::internal("No staged write to commit");
        std::error_code error;
        std::filesystem::rename(m_temporary, m_target, error);
        if (error) {
            discard();
            return Error::internal("Replacing the destination file failed; the original is unchanged");
        }
        m_staged = false;
        return Result<void>::ok();
    }

    void discard() {
        if (!m_staged) return;
        std::error_code ignored;
        std::filesystem::remove(m_temporary, ignored);
        m_staged = false;
    }

private:
    std::filesystem::path m_target;
    std::filesystem::path m_temporary;
    bool m_staged{false};
};

// Puts the bytes on disk beside the destination without replacing it yet.
inline Result<StagedWrite> stageFileWrite(const std::filesystem::path& target,
                                          std::string_view contents) {
    auto suffix = security::secureRandomHex(8);
    if (suffix.isErr()) return suffix.error();

    std::filesystem::path temporary = target;
    temporary += paths::projectPathFromUtf8(".didi-tmp-" + suffix.value());

    std::error_code error;
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output.is_open()) {
            return Error::internal("Unable to open a temporary file next to the destination");
        }
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        output.flush();
        const bool wrote = output.good();
        output.close();
        if (!wrote || output.fail()) {
            std::filesystem::remove(temporary, error);
            return Error::internal("Writing the replacement file failed; the original is unchanged");
        }
    }
    return StagedWrite(target, std::move(temporary));
}

// Replaces target with contents, or leaves target exactly as it was.
//
// The bytes go to a sibling temporary file first. Only once the write, the
// flush and the close have all been checked does the temporary take the
// destination's place. A full disk, a quota failure or a stopped process can
// therefore no longer destroy the file the caller was updating, and a caller
// that is told the write succeeded can rely on the bytes being on disk.
inline Result<void> writeFileAtomically(const std::filesystem::path& target,
                                        std::string_view contents) {
    auto staged = stageFileWrite(target, contents);
    if (staged.isErr()) return staged.error();
    return staged.value().commit();
}

} // namespace didi::files
