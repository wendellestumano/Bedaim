#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "core/memory/MinecraftBuildIdentity.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>

namespace {
int checks = 0;
int failures = 0;

void check(bool condition, std::string_view name) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << name << '\n';
    }
}
}

int main() {
    using namespace bedrocktools::core::memory;
    using bedrocktools::core::memory::detail::alignNoteSize;
    using bedrocktools::core::memory::detail::checkedAdd;

    check(matchesMinecraft260BuildId(kMinecraft260BuildId.data(), kMinecraft260BuildId.size()),
          "the pinned 26.60 GNU Build ID matches");
    check(!matchesMinecraft260BuildId(kMinecraft260BuildId.data(), kMinecraft260BuildId.size() - 1),
          "a truncated GNU Build ID is rejected");

    auto altered = kMinecraft260BuildId;
    altered[7] ^= 0x01u;
    check(!matchesMinecraft260BuildId(altered.data(), altered.size()),
          "a different GNU Build ID is rejected");

    std::uintptr_t sum = 0;
    check(checkedAdd(10u, 20u, sum) && sum == 30u,
          "checked address addition returns the expected sum");
    sum = 77u;
    check(!checkedAdd(std::numeric_limits<std::uintptr_t>::max(), 1u, sum) && sum == 77u,
          "checked address addition rejects overflow without changing output");

    std::size_t aligned = 0;
    check(alignNoteSize(1u, aligned) && aligned == 4u,
          "ELF note names are aligned to four bytes");
    check(alignNoteSize(4u, aligned) && aligned == 4u,
          "already aligned ELF note sizes remain unchanged");

    std::cout << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
