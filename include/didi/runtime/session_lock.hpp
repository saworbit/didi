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

private:
    RuntimeSessionLock(std::filesystem::path path, intptr_t native_handle)
        : m_path(std::move(path)), m_nativeHandle(native_handle) {}

    std::filesystem::path m_path;
    intptr_t m_nativeHandle{-1};
};

} // namespace didi::runtime
