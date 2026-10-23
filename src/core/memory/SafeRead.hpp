#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <limits>
#include <sys/types.h>
#include <unistd.h>

namespace bedrocktools::core::memory {

// Read this process's memory through procfs instead of dereferencing a
// potentially stale game pointer. A short/failed read is treated as unavailable.
inline bool readSelfMemory(std::uintptr_t address, void* destination,
                           std::size_t size) noexcept {
    if (!destination || size == 0 || address <= 0x1000 ||
        size > static_cast<std::size_t>(std::numeric_limits<ssize_t>::max())) {
        return false;
    }

    const auto maxOffset = static_cast<std::uintmax_t>(
        std::numeric_limits<off_t>::max());
    const auto start = static_cast<std::uintmax_t>(address);
    const auto length = static_cast<std::uintmax_t>(size);
    if (start > maxOffset || length > maxOffset - start) return false;

    static const int memoryFd = ::open("/proc/self/mem", O_RDONLY | O_CLOEXEC);
    if (memoryFd < 0) return false;

    ssize_t copied = -1;
    do {
        copied = ::pread(memoryFd, destination, size, static_cast<off_t>(address));
    } while (copied < 0 && errno == EINTR);

    return copied == static_cast<ssize_t>(size);
}

} // namespace bedrocktools::core::memory
