#pragma once

namespace bedrocktools::core::gamehooks {

bool install();
void uninstall();
void* clientInstance();
bool attack(void* gameMode, void* target, bool playPredictiveSound, const void* hitPosition);

}
