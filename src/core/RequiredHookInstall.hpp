#pragma once

#include <utility>

namespace bedrocktools::core::gamehooks::detail {

template <typename InstallRequired, typename TrampolinesReady, typename Rollback>
bool installRequiredHooksTransactionally(
    InstallRequired&& installRequired,
    TrampolinesReady&& trampolinesReady,
    Rollback&& rollback) {
    const bool hooksInstalled = std::forward<InstallRequired>(installRequired)();
    const bool ready = hooksInstalled && std::forward<TrampolinesReady>(trampolinesReady)();
    if (!ready) {
        std::forward<Rollback>(rollback)();
        return false;
    }
    return true;
}

} // namespace bedrocktools::core::gamehooks::detail
