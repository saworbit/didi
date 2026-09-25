#pragma once

#include "didi/common/project_path.hpp"
#include "didi/common/filesystem_status.hpp"
#include "didi/common/secure_random.hpp"
#include "didi/common/types.hpp"

#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <system_error>

namespace didi::files {

// Renames from to to, retrying for up to budget while Windows says one of them
// is in use.
//
// A file or folder written moments ago is routinely held for a fraction of a
// second by somebody else: a scanner, an indexer, a backup agent, or a reader
// that has the destination open. Windows refuses a rename out of or over an
// open file until that handle goes, so an operation that tried once failed on
// a lock that had usually gone by the time anyone read the error (#678, #937).
// Nowhere else is that refusal transient, so other platforms try once. The
// caller gets the last error, so a handle that really is held still reports
// the refusal it always did.
inline std::error_code renameWithRetry(const std::filesystem::path& from,
                                       const std::filesystem::path& to,
                                       std::chrono::milliseconds budget) {
    std::error_code error;
    std::filesystem::rename(from, to, error);
#if defined(_WIN32)
    const auto in_use = [](const std::error_code& code) {
        constexpr int kAccessDenied = 5;       // ERROR_ACCESS_DENIED
        constexpr int kSharingViolation = 32;  // ERROR_SHARING_VIOLATION
        constexpr int kLockViolation = 33;     // ERROR_LOCK_VIOLATION
        return code.category() == std::system_category() &&
               (code.value() == kAccessDenied || code.value() == kSharingViolation ||
                code.value() == kLockViolation);
    };
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (error && in_use(error) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        error.clear();
        std::filesystem::rename(from, to, error);
    }
#else
    (void)budget;
#endif
    return error;
}

// How long a replace waits out a handle on the destination. A reader holds the
// file for milliseconds; a real refusal, a read-only destination say, is told
// after this.
inline constexpr std::chrono::milliseconds kReplaceRetryBudget{2000};

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
        // A reader with the destination open refuses the replace on Windows
        // until it lets go: project_list_export_presets reading while
        // project_add_export_preset wrote was one, outside the lock (#937).
        const auto error = renameWithRetry(m_temporary, m_target, kReplaceRetryBudget);
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
        errno = 0;
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output.is_open()) {
            // The reason matters to the caller. A path the filesystem will not
            // take -- too long, or holding something a name may not hold -- is
            // the caller's argument to fix, and reporting every open failure as
            // an internal error told an agent the server had broken (#526).
            const std::error_code open_error(errno, std::generic_category());
            const int status = statusForFilesystemError(open_error);
            std::string reason = "Unable to open a temporary file next to the destination";
            if (open_error) reason += ": " + open_error.message();
            return Error(status, std::move(reason));
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
