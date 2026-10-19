#pragma once
#include <bedrocktools/events/Event.hpp>

namespace bedrocktools::sdk { class Player; }

namespace bedaim::events {
struct VerifiedLocalPlayerTickEvent {
    static constexpr bedrocktools::events::EventType type = bedrocktools::events::EventType::VerifiedLocalPlayerTick; bedrocktools::sdk::Player* player; };
}
