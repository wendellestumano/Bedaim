#include "core/RequiredHookInstall.hpp"

#include <iostream>

namespace {
int failures = 0;

void check(bool condition, const char* name) {
    if (condition) {
        std::cout << "PASS: " << name << '\n';
    } else {
        std::cerr << "FAIL: " << name << '\n';
        ++failures;
    }
}
}

int main() {
    using bedrocktools::core::gamehooks::detail::installRequiredHooksTransactionally;

    {
        int tickAttempts = 0;
        int readinessChecks = 0;
        int rollbacks = 0;
        const bool installed = installRequiredHooksTransactionally(
            [&] { ++tickAttempts; return false; },
            [&] { ++readinessChecks; return true; },
            [&] { ++rollbacks; });
        check(!installed && tickAttempts == 1 && readinessChecks == 0 && rollbacks == 1,
              "NormalTick failure rolls back and skips trampoline check");
    }

    {
        int tickAttempts = 0;
        int clientUpdateAttempts = 0;
        int readinessChecks = 0;
        int rollbacks = 0;
        const bool installed = installRequiredHooksTransactionally(
            [&] {
                ++tickAttempts;
                const bool tickOk = true;
                if (!tickOk) return false;
                ++clientUpdateAttempts;
                const bool clientUpdateOk = false;
                return tickOk && clientUpdateOk;
            },
            [&] { ++readinessChecks; return true; },
            [&] { ++rollbacks; });
        check(!installed && tickAttempts == 1 && clientUpdateAttempts == 1 &&
                  readinessChecks == 0 && rollbacks == 1,
              "ClientInstanceUpdate failure rolls back the earlier hook");
    }

    {
        int rollbacks = 0;
        const bool installed = installRequiredHooksTransactionally(
            [] { return true; }, [] { return false; }, [&] { ++rollbacks; });
        check(!installed && rollbacks == 1,
              "Missing required trampoline rolls back installed handles");
    }

    {
        int rollbacks = 0;
        const bool installed = installRequiredHooksTransactionally(
            [] { return true; }, [] { return true; }, [&] { ++rollbacks; });
        check(installed && rollbacks == 0,
              "Both required hooks and trampolines ready avoids rollback");
    }

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All 4 required-hook installation tests passed\n";
    return 0;
}
