#include "core/NativeAttackResultPolicy.hpp"

#include <cassert>

int main() {
    using bedrocktools::core::attackresult::confirmedByNativeRoute;

    // Exact validated public three-argument adapter can use its own bool return
    // if the secondary observer hook was not reached.
    assert(confirmedByNativeRoute(false, false, false, false, false, true));
    assert(!confirmedByNativeRoute(false, false, false, false, false, false));

    // When the internal observer runs, its result takes precedence.
    assert(confirmedByNativeRoute(true, true, false, false, false, false));
    assert(!confirmedByNativeRoute(true, false, false, false, false, true));

    // Guarded four-argument route still requires the observer and the output flag.
    assert(confirmedByNativeRoute(true, true, true, true, true, true));
    assert(!confirmedByNativeRoute(false, false, true, true, true, true));
    assert(!confirmedByNativeRoute(true, true, true, true, false, true));
    assert(!confirmedByNativeRoute(true, false, true, true, true, true));
    return 0;
}
