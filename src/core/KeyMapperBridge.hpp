#pragma once

#include <string_view>

namespace bedrocktools::core {

enum class KeyMapperDispatchResult {
    Sent,
    InvalidUid,
    VmUnavailable,
    ThreadAttachFailed,
    ContextUnavailable,
    IntentApiUnavailable,
    BroadcastFailed
};

// Sends the explicit Key Mapper trigger broadcast for one configured keymap UID.
// "Sent" means Android accepted sendBroadcast; it does not prove that the game
// delivered the resulting input or that an in-game action succeeded.
KeyMapperDispatchResult dispatchKeyMapperKeymap(std::string_view uid) noexcept;
const char* keyMapperDispatchResultName(KeyMapperDispatchResult result) noexcept;

} // namespace bedrocktools::core
