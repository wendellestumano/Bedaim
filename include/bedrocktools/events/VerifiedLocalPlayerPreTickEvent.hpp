#pragma once

#include <bedrocktools/events/Event.hpp>

namespace bedrocktools::sdk { class Player; }

namespace bedrocktools::events {

// Published only when NormalTick's actor matches the player returned by the
// SDK's documented ClientInstance::getLocalPlayer virtual method. Existing
// generic tick events remain unchanged for modules that rely on them.
struct VerifiedLocalPlayerPreTickEvent {
    static constexpr EventType type = EventType::VerifiedLocalPlayerPreTick;
    sdk::Player* player;
};

}
