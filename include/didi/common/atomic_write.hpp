#pragma once

#include "didi/common/project_path.hpp"
#include "didi/common/secure_random.hpp"
#include "didi/common/types.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

namespace didi::files {

// Replaces target with contents, or leaves target exactly as it was.
//
// The bytes go to a sibling temporary file first. Only once the write, the
// flush and the close have all been checked does the temporary take the
// destination's place. A full disk, a quota failure or a stopped process can
// therefore no longer destroy the file the caller was updating, and a caller
// that is told the write succeeded can rely on the bytes being on disk.
inline Result<void> writeFileAtomically(const std::filesystem::path& target,
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

    std::filesystem::rename(temporary, target, error);
    if (error) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return Error::internal("Replacing the destination file failed; the original is unchanged");
    }
    return Result<void>::ok();
}

} // namespace didi::files
