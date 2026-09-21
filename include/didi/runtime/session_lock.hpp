#pragma once

#include "didi/common/types.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>

namespace didi::runtime {

class RuntimeSessionLock {
public:
    ~RuntimeSessionLock();
    RuntimeSessionLock(const RuntimeSessionLock&) = delete;
    RuntimeSessionLock& operator=(const RuntimeSessionLock&) = delete;

    static Result<std::shared_ptr<RuntimeSessionLock>> acquire(
        const std::filesystem::path& path, const json& owner);

    const std::filesystem::path& path() const { return m_path; }

    // Releases the lock and removes the file, for a lock this process has just
    // proved nobody else holds. The destructor releases and leaves the path, so
    // a session directory shared by every project on the machine collected one
    // file per session forever while the descriptor beside it was cleaned up
    // (#787).
    void releaseAndRemove();

private:
    RuntimeSessionLock(std::filesystem::path path, intptr_t native_handle)
        : m_path(std::move(path)), m_nativeHandle(native_handle) {}

    std::filesystem::path m_path;
    intptr_t m_nativeHandle{-1};
};

} // namespace didi::runtime
