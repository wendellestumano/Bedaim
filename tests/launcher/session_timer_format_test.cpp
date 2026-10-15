#include "modules/hud/SessionTimerFormat.hpp"

#include <cassert>

int main() {
    using bedaim::hud::formatSessionDuration;

    assert(formatSessionDuration(0) == "00:00:00");
    assert(formatSessionDuration(1) == "00:00:01");
    assert(formatSessionDuration(59) == "00:00:59");
    assert(formatSessionDuration(60) == "00:01:00");
    assert(formatSessionDuration(3599) == "00:59:59");
    assert(formatSessionDuration(3600) == "01:00:00");
    assert(formatSessionDuration(86400) == "24:00:00");
    assert(formatSessionDuration(-1) == "00:00:00");
    assert(formatSessionDuration(3599, false) == "59:59");
    assert(formatSessionDuration(3600, false) == "01:00:00");
    return 0;
}
