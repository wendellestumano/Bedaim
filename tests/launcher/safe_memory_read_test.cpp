#include "core/memory/SafeRead.hpp"

#include <cstdint>
#include <cstdio>
#include <sys/mman.h>
#include <unistd.h>

namespace {
int fail(const char* message) {
    std::fprintf(stderr, "safe_memory_read_test: %s\n", message);
    return 1;
}
}

int main() {
    std::uint64_t source = 0x123456789abcdef0ULL;
    std::uint64_t destination = 0;

    if (!bedrocktools::core::memory::readSelfMemory(
            reinterpret_cast<std::uintptr_t>(&source), &destination, sizeof(source)) ||
        destination != source) {
        return fail("valid self-memory read did not preserve the value");
    }

    if (bedrocktools::core::memory::readSelfMemory(1, &destination, sizeof(destination))) {
        return fail("low address was accepted");
    }
    if (bedrocktools::core::memory::readSelfMemory(
            reinterpret_cast<std::uintptr_t>(&source), nullptr, sizeof(source))) {
        return fail("null destination was accepted");
    }
    if (bedrocktools::core::memory::readSelfMemory(
            reinterpret_cast<std::uintptr_t>(&source), &destination, 0)) {
        return fail("zero-length read was accepted");
    }
    if (bedrocktools::core::memory::readSelfMemory(
            UINTPTR_MAX - 1, &destination, sizeof(destination))) {
        return fail("overflowing address range was accepted");
    }

    const long pageSize = ::sysconf(_SC_PAGESIZE);
    if (pageSize <= 0) return fail("could not determine page size");
    void* region = ::mmap(nullptr, static_cast<std::size_t>(pageSize),
                          PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (region == MAP_FAILED) return fail("mmap failed");
    const auto unmappedAddress = reinterpret_cast<std::uintptr_t>(region);
    if (::munmap(region, static_cast<std::size_t>(pageSize)) != 0) {
        return fail("munmap failed");
    }
    if (bedrocktools::core::memory::readSelfMemory(
            unmappedAddress, &destination, sizeof(destination))) {
        return fail("unmapped address was accepted");
    }

    std::puts("safe_memory_read_test: 6 checks passed");
    return 0;
}
