#pragma once

#include <atomic>
#include <string>
#include <string_view>
#include <vector>
#include <cstdint>

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

struct InventorySlotSnapshot {
    int slotIndex = -1;
    bool empty = true;
    std::string itemIdentifier;
    std::uint32_t count = 0;
};

class InventoryAccess {
public:
    static InventoryAccess& get();
    void initialize();
    int countItems(const sdk::Player* player, std::string_view identifierPart) const;
    int countStack(const void* stack, std::string_view identifierPart) const;
    std::string stackIdentifier(const void* stack) const;
    int findHotbarSlot(const sdk::Player* player, std::string_view identifierPart) const;
    std::string hotbarSlotIdentifier(const sdk::Player* player, int slot) const;
    int findInventorySlot(const sdk::Player* player, std::string_view identifierPart) const;
    int selectedHotbarSlot(const sdk::Player* player) const;
    // Reads the existing 1.26.60 inventory layout without mutating it. Returns false
    // for any unreadable/unknown slot so callers never mistake unknown for empty.
    bool readInventorySnapshot(const sdk::Player* player,
                               std::vector<InventorySlotSnapshot>& slots,
                               int* selectedSlot = nullptr) const;
    std::string selectedHotbarIdentifier(const sdk::Player* player) const;
    std::string offhandIdentifier(const sdk::Player* player) const;
    bool readOffhandIdentifier(const sdk::Player* player, std::string& identifier,
                               std::string* error = nullptr) const;
    bool selectHotbarSlot(sdk::Player* player, int slot) const;
    OffhandTransferResult moveInventoryItemToOffhand(sdk::Player* player, int slot) const;

private:
    using GetOffhandSlotFn = const void* (*)(const void*);
    GetOffhandSlotFn m_getOffhandSlot = nullptr;
    std::string m_getOffhandSlotSource = "unresolved";
    mutable std::atomic_bool m_offhandGetterQuarantined{false};
};

}
