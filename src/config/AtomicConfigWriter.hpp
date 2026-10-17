#pragma once

#include <string>

namespace bedrocktools::config {

// Write a complete config snapshot to a sibling temporary file, then atomically
// replace the destination. A failed rename must never delete the previous config.
bool writePayloadAtomically(const std::string& path, const std::string& payload) noexcept;

} // namespace bedrocktools::config
