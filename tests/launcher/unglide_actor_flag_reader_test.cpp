#include "modules/player/UnglideActorFlagReader.hpp"

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
    using namespace bedaim::macefall::actor_flag;

    check(kActorDataFlagComponentId == 0xC67426F3u,
          "ActorDataFlagComponent uses the recovered 26.60 FNV-1a descriptor");
    check(fnv1a32("ActorDataFlagComponent") == kActorDataFlagComponentId,
          "the ActorDataFlagComponent descriptor hash recomputes to the binary constant");
    check(kGlidingFlagBit == 32,
          "query.is_gliding tests actor flag bit 32");
    check(matchesReaderStub(kReaderInstructions),
          "the recovered Actor* wrapper instruction sequence is accepted");
    check(readerBranchesToHelper(kReaderInstructions),
          "the wrapper branch resolves to the verified component reader");
    check(matchesHelperPrefix(kHelperInstructions),
          "the generic helper prefix materializes ActorDataFlagComponent hash");

    auto wrongReader = kReaderInstructions;
    wrongReader[0] ^= 0x1u;
    check(!matchesReaderStub(wrongReader),
          "a changed Actor* context adjustment is rejected");

    wrongReader = kReaderInstructions;
    wrongReader[1] ^= 0x4u;
    check(!readerBranchesToHelper(wrongReader),
          "a branch to a different helper RVA is rejected");

    auto wrongHelper = kHelperInstructions;
    wrongHelper[2] ^= 0x00010000u;
    check(!matchesHelperPrefix(wrongHelper),
          "a different component hash in the helper is rejected");

    check(decodeAarch64BranchTarget(kReaderRva + 4u, 0xD65F03C0u) == 0,
          "a non-branch instruction cannot be interpreted as a helper target");
    check(decodeAarch64BranchTarget(0x1000u, 0x17FFFFFFu) == 0x0FFCu,
          "AArch64 negative branch immediates are sign-extended correctly");
    check(decodeAarch64BranchTarget(
              std::numeric_limits<std::uintptr_t>::max() - 1u, 0x14000001u) == 0,
          "AArch64 branch target overflow is rejected");

    std::cout << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
