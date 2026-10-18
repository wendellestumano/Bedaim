#pragma once

namespace bedrocktools::sdk {
class Player;
}

namespace bedaim::events {

// Published after the native actor tick, and only when the object identity was
// verified as the current local player. Use this for post-tick inventory/HUD
// sampling instead of the raw LocalPlayerTickEvent, which is emitted for every actor.
struct VerifiedLocalPlayerTickEvent {
    bedrocktools::sdk::Player* player{};
};

} // namespace bedaim::events
