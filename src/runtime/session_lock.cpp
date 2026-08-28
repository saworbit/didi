#include "didi/runtime/session_lock.hpp"

#include <limits>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace didi::runtime {

Result<std::shared_ptr<RuntimeSessionLock>> RuntimeSessionLock::acquire(
    const std::filesystem::path& path, const json& owner) {
    if (!owner.is_object()) return Error::invalidArgument("Session lock owner must be an object");
    const auto contents = owner.dump();
#if defined(_WIN32)
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_ALWAYS,
                                FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return Error::internal("Unable to open the runtime session lock");
    }
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &attributes, sizeof(attributes)) ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        CloseHandle(handle);
        return Error::internal("Runtime session lock is not a regular file");
    }
    OVERLAPPED lock_range{};
    if (!LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                    0, 1, 0, &lock_range)) {
        CloseHandle(handle);
        return Error(423, "Runtime session is locked by another MCP client");
    }
    LARGE_INTEGER beginning{};
    DWORD written = 0;
    if (!SetFilePointerEx(handle, beginning, nullptr, FILE_BEGIN) || !SetEndOfFile(handle) ||
        (contents.size() > static_cast<size_t>(std::numeric_limits<DWORD>::max())) ||
        !WriteFile(handle, contents.data(), static_cast<DWORD>(contents.size()), &written, nullptr) ||
        written != contents.size() || !FlushFileBuffers(handle)) {
        UnlockFileEx(handle, 0, 1, 0, &lock_range);
        CloseHandle(handle);
        return Error::internal("Unable to publish runtime session lock ownership");
    }
    return std::shared_ptr<RuntimeSessionLock>(
        new RuntimeSessionLock(path, reinterpret_cast<intptr_t>(handle)));
#else
    const int fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
                        S_IRUSR | S_IWUSR);
    if (fd < 0) return Error::internal("Unable to open the runtime session lock");
    struct stat status{};
    if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_uid != geteuid() || fchmod(fd, S_IRUSR | S_IWUSR) != 0) {
        close(fd);
        return Error::internal("Runtime session lock is not an owner-only regular file");
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        return Error(423, "Runtime session is locked by another MCP client");
    }
    if (ftruncate(fd, 0) != 0 || lseek(fd, 0, SEEK_SET) < 0) {
        flock(fd, LOCK_UN);
        close(fd);
        return Error::internal("Unable to initialize runtime session lock ownership");
    }
    size_t offset = 0;
    while (offset < contents.size()) {
        const auto written = write(fd, contents.data() + offset, contents.size() - offset);
        if (written > 0) offset += static_cast<size_t>(written);
        else if (written < 0 && errno == EINTR) continue;
        else {
            flock(fd, LOCK_UN);
            close(fd);
            return Error::internal("Unable to publish runtime session lock ownership");
        }
    }
    if (fsync(fd) != 0) {
        flock(fd, LOCK_UN);
        close(fd);
        return Error::internal("Unable to sync runtime session lock ownership");
    }
    return std::shared_ptr<RuntimeSessionLock>(new RuntimeSessionLock(path, fd));
#endif
}

RuntimeSessionLock::~RuntimeSessionLock() {
    if (m_nativeHandle == -1) return;
#if defined(_WIN32)
    auto handle = reinterpret_cast<HANDLE>(m_nativeHandle);
    OVERLAPPED lock_range{};
    UnlockFileEx(handle, 0, 1, 0, &lock_range);
    CloseHandle(handle);
#else
    const int fd = static_cast<int>(m_nativeHandle);
    flock(fd, LOCK_UN);
    close(fd);
#endif
    m_nativeHandle = -1;
}

} // namespace didi::runtime
