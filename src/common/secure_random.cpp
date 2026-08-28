#include "didi/common/secure_random.hpp"

#include <limits>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#else
#include <cerrno>
#include <fcntl.h>
#if defined(__linux__)
#include <sys/random.h>
#endif
#include <unistd.h>
#endif

namespace didi::security {

Result<std::vector<uint8_t>> secureRandomBytes(size_t count) {
    std::vector<uint8_t> bytes(count);
#if defined(_WIN32)
    if (count > static_cast<size_t>(std::numeric_limits<ULONG>::max()) ||
        BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        return Error::internal("The operating-system random generator failed");
    }
#else
    size_t offset = 0;
#if defined(__linux__)
    while (offset < bytes.size()) {
        const auto read_count = getrandom(bytes.data() + offset, bytes.size() - offset, 0);
        if (read_count > 0) {
            offset += static_cast<size_t>(read_count);
        } else if (read_count < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
#endif
    if (offset < bytes.size()) {
        const int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
        if (fd < 0) return Error::internal("The operating-system random generator is unavailable");
        while (offset < bytes.size()) {
            const auto read_count = read(fd, bytes.data() + offset, bytes.size() - offset);
            if (read_count > 0) {
                offset += static_cast<size_t>(read_count);
            } else if (read_count < 0 && errno == EINTR) {
                continue;
            } else {
                close(fd);
                return Error::internal("The operating-system random generator failed");
            }
        }
        close(fd);
    }
#endif
    return bytes;
}

Result<std::string> secureRandomHex(size_t byte_count) {
    auto bytes = secureRandomBytes(byte_count);
    if (bytes.isErr()) return bytes.error();
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(byte_count * 2);
    for (const auto byte : bytes.value()) {
        result.push_back(digits[(byte >> 4) & 0x0f]);
        result.push_back(digits[byte & 0x0f]);
    }
    return result;
}

} // namespace didi::security
