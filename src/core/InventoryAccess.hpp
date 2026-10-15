#pragma once

#include <string>
#include <string_view>

namespace bedrocktools::sdk { class Player; }

namespace bedrocktools::core {

enum class OffhandTransferResult {
    Success,
    AlreadyEquipped,
    InvalidPlayer,
    InvalidSlot,
    ItemUnavailable,
    OffhandOccupied,
    RuntimeApiUnavailable,
    InvalidNativeState,
    VerificationFailed
};

class InventoryAccess {
public:
    static InventoryAccess& get();
    void initialize();
    int countItems(const sdk::Player* player, std::string_view identifierPart) const;
    int countStack(const void* stack, std::string_view identifierPart) const;
    std::string stackIdentifier(const void* stack) const;
    int findHotbarSlot(const sdk::Player* player, std::string_view identifierPart) const;
    int findInventorySlot(const sdk::Player* player, std::string_view identifierPart) const;
    int selectedHotbarSlot(const sdk::Player* player) const;
    std::string selectedHotbarIdentifier(const sdk::Player* player) const;
    std::string offhandIdentifier(const sdk::Player* player) const;
    bool selectHotbarSlot(sdk::Player* player, int slot) const;
    OffhandTransferResult moveInventoryItemToOffhand(sdk::Player* player, int slot) const;

private:
    using GetOffhandSlotFn = const void* (*)(const void*);
    GetOffhandSlotFn m_getOffhandSlot = nullptr;
};

}
