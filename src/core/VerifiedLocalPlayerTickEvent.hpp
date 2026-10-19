#pragma once

namespace bedrocktools::sdk { class Player; }

namespace bedaim::events {
struct VerifiedLocalPlayerTickEvent { bedrocktools::sdk::Player* player; };
}
