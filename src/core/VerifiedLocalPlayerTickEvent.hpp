#pragma once

#include <bedrocktools/events/Event.hpp>

namespace bedrocktools::sdk {
class Player;
}

namespace bedaim::events {

// Published after the native actor tick, and only when the object identity was
// verified as the current local player. Use this for post-tick inventory/HUD
// sampling instead of the raw LocalPlayerTickEvent, which is emitted for every actor.
struct VerifiedLocalPlayerTickEvent {
    static constexpr bedrocktools::events::EventType type =
        bedrocktools::events::EventType::VerifiedLocalPlayerTick;
    bedrocktools::sdk::Player* player{};
};

} // namespace bedaim::events
