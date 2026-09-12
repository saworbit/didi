#pragma once

#include <system_error>

namespace didi::files {

// The HTTP-shaped status a failed filesystem call deserves.
//
// A writer that reports every std::error_code as 500 tells an agent the server
// broke when the path it sent was too long or was not a path at all, and the
// fix is in the caller's hands (#526). The comparison is against std::errc
// rather than a raw value so the Windows error codes map the same way the
// POSIX ones do.
inline int statusForFilesystemError(const std::error_code& error) {
    if (!error) return 500;
    if (error == std::errc::filename_too_long ||
        error == std::errc::invalid_argument ||
        error == std::errc::no_such_file_or_directory ||
        error == std::errc::not_a_directory ||
        error == std::errc::is_a_directory ||
        error == std::errc::illegal_byte_sequence) {
        return 400;
    }
    if (error == std::errc::permission_denied ||
        error == std::errc::operation_not_permitted ||
        error == std::errc::read_only_file_system) {
        return 403;
    }
    if (error == std::errc::file_exists) return 409;
    return 500;
}

} // namespace didi::files
