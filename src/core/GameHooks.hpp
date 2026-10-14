#pragma once

namespace bedrocktools::core::gamehooks {

bool install();
void uninstall();
void* clientInstance();
void* gameMode();
bool attack(void* gameMode, void* target, const void* hitPosition);

}
