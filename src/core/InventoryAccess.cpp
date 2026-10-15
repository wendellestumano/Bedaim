#include "InventoryAccess.hpp"

#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/offsets/Inventory.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/Memory.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <limits>
#include <string>
#include <vector>

namespace bedrocktools::core {
namespace {
using namespace bedrocktools::sdk::offsets::Inventory;

template<class T>
T read(const void* object, std::size_t offset) {
    return *reinterpret_cast<const T*>(static_cast<const std::byte*>(object) + offset);
}

bool inventorySlots(const void* player, std::uintptr_t& begin, std::size_t& size) {
    begin = 0;
    size = 0;
    if (!player) return false;
    const void* proxy = read<const void*>(player, PlayerInventory);
    if (!proxy) return false;
    const void* inventory = read<const void*>(proxy, PlayerInventoryContainer);
    if (!inventory) return false;
    begin = read<std::uintptr_t>(inventory, FillingContainerItems);
    const auto end = read<std::uintptr_t>(inventory, FillingContainerItems + sizeof(void*));
    if (!begin && !end) return true;
    if (!begin || end < begin || begin % alignof(void*) != 0) return false;
    const auto bytes = end - begin;
    if (bytes % ItemStackSize != 0) return false;
    size = bytes / ItemStackSize;
    return size <= 64;
}

constexpr std::size_t PlayerTransactionManagerOffset = 0xA28;
constexpr std::size_t TransactionManagerSize = 48;
constexpr std::size_t TransactionManagerExpectedActions = 16;
constexpr std::size_t TransactionActionSize = 512;
constexpr std::size_t PlayerSetOffhandVTable = bedrocktools::sdk::offsets::VTable::PlayerGetCarriedItem + 1;
constexpr std::int8_t InventoryContainerId = 0;
constexpr std::int8_t OffhandContainerId = 119;

struct NativeInventorySource {
    std::uint32_t type;
    std::int8_t containerId;
    std::uint8_t padding[3];
    std::uint32_t flags;
};
static_assert(sizeof(NativeInventorySource) == 12);
static_assert(offsetof(NativeInventorySource, flags) == 8);
static_assert(ItemStackSize == 0x98);

bool isReadableMemory(const void* pointer, std::size_t size) {
    if (!pointer || size == 0) return false;
    const auto begin = reinterpret_cast<std::uintptr_t>(pointer);
    if (size > std::numeric_limits<std::uintptr_t>::max() - begin) return false;
    const auto end = begin + size;
    std::FILE* maps = std::fopen("/proc/self/maps", "r");
    if (!maps) return false;
    char line[512]{};
    bool found = false;
    while (std::fgets(line, sizeof(line), maps)) {
        unsigned long long regionBegin = 0, regionEnd = 0;
        char permissions[5]{};
        if (std::sscanf(line, "%llx-%llx %4s", &regionBegin, &regionEnd, permissions) == 3 &&
            permissions[0] == 'r' &&
            begin >= static_cast<std::uintptr_t>(regionBegin) &&
            end <= static_cast<std::uintptr_t>(regionEnd)) {
            found = true;
            break;
        }
    }
    std::fclose(maps);
    return found;
}

bool isMinecraftFunction(const void* address) {
    if (!address) return false;
    Dl_info info{};
    return dladdr(address, &info) != 0 && info.dli_fname &&
        std::string_view(info.dli_fname).find("libminecraftpe.so") != std::string_view::npos;
}

void* resolveMinecraftSymbol(const char* symbol) {
    if (!symbol || !*symbol) return nullptr;
    void* address = dlsym(RTLD_DEFAULT, symbol);
    if (isMinecraftFunction(address)) return address;

    void* library = dlopen("libminecraftpe.so", RTLD_NOW | RTLD_NOLOAD);
    if (!library) return nullptr;
    address = dlsym(library, symbol);
    const bool valid = isMinecraftFunction(address);
    dlclose(library);
    return valid ? address : nullptr;
}

bool validExpectedActionsVector(const void* manager) {
    const auto begin = read<std::uintptr_t>(manager, TransactionManagerExpectedActions);
    const auto end = read<std::uintptr_t>(manager, TransactionManagerExpectedActions + sizeof(void*));
    const auto capacity = read<std::uintptr_t>(manager, TransactionManagerExpectedActions + 2 * sizeof(void*));
    if (!begin && !end && !capacity) return true;
    if (!begin || end < begin || capacity < end) return false;
    const auto sizeBytes = end - begin;
    const auto capacityBytes = capacity - begin;
    return sizeBytes % TransactionActionSize == 0 &&
        capacityBytes % TransactionActionSize == 0 &&
        capacityBytes <= TransactionActionSize * 4096 &&
        isReadableMemory(reinterpret_cast<const void*>(begin), capacityBytes == 0 ? 1 : capacityBytes);
}
}

InventoryAccess& InventoryAccess::get() {
    static InventoryAccess access;
    return access;
}

void InventoryAccess::initialize() {
    m_getOffhandSlot = reinterpret_cast<GetOffhandSlotFn>(
        memory::resolve(memory::SignatureId::ActorGetOffhandSlot));
}

int InventoryAccess::countItems(const sdk::Player* player, std::string_view identifierPart) const {
    if (!player) return 0;
    std::uintptr_t begin = 0;
    std::size_t size = 0;
    int total = 0;
    const bool valid = inventorySlots(player, begin, size);
    if (valid) {
        for (std::size_t slot = 0; slot < size; ++slot) {
            total += countStack(reinterpret_cast<const void*>(begin + slot * ItemStackSize), identifierPart);
        }
    }
    const void* offhand = m_getOffhandSlot ? m_getOffhandSlot(player) : nullptr;
    const auto offhandAddress = reinterpret_cast<std::uintptr_t>(offhand);
    const bool alreadyCounted = valid && begin && offhandAddress >= begin &&
        offhandAddress - begin < size * ItemStackSize && (offhandAddress - begin) % ItemStackSize == 0;
    if (!alreadyCounted) total += countStack(offhand, identifierPart);
    return total;
}

std::string InventoryAccess::stackIdentifier(const void* stack) const {
    if (!stack || !read<std::uint8_t>(stack, ItemStackValid)) return {};
    if (read<std::uint8_t>(stack, ItemStackCount) == 0) return {};
    const void* counter = read<const void*>(stack, ItemStackItemCounter);
    const void* item = counter ? read<const void*>(counter, 0) : nullptr;
    if (!item) return {};
    const auto* vtable = read<const std::uintptr_t*>(item, 0);
    if (!vtable || !vtable[ItemGetDescriptionIdVtableIndex]) return {};
    using GetDescriptionIdFn = const std::string& (*)(const void*);
    const auto& identifier = reinterpret_cast<GetDescriptionIdFn>(vtable[ItemGetDescriptionIdVtableIndex])(item);
    if (identifier.empty() || identifier.size() > 128) return {};
    return identifier;
}

int InventoryAccess::countStack(const void* stack, std::string_view identifierPart) const {
    if (identifierPart.empty()) return 0;
    const auto identifier = stackIdentifier(stack);
    if (identifier.empty()) return 0;
    return identifier.find(identifierPart) != std::string::npos
        ? static_cast<int>(read<std::uint8_t>(stack, ItemStackCount)) : 0;
}

int InventoryAccess::findHotbarSlot(const sdk::Player* player, std::string_view identifierPart) const {
    if (!player || identifierPart.empty()) return -1;
    std::uintptr_t begin = 0;
    std::size_t size = 0;
    if (!inventorySlots(player, begin, size) || !begin || size < 9) return -1;

    // Player inventory slots 0..8 are the hotbar. Avoid picking an item from the
    // lower inventory because the swapper must be able to equip it immediately.
    for (std::size_t slot = 0; slot < 9; ++slot) {
        const void* stack = reinterpret_cast<const void*>(begin + slot * ItemStackSize);
        if (countStack(stack, identifierPart) > 0) return static_cast<int>(slot);
    }
    return -1;
}

int InventoryAccess::findInventorySlot(const sdk::Player* player, std::string_view identifierPart) const {
    if (!player || identifierPart.empty()) return -1;
    std::uintptr_t begin = 0;
    std::size_t size = 0;
    if (!inventorySlots(player, begin, size) || !begin || size == 0) return -1;
    for (std::size_t slot = 0; slot < size; ++slot) {
        const void* stack = reinterpret_cast<const void*>(begin + slot * ItemStackSize);
        if (countStack(stack, identifierPart) > 0) return static_cast<int>(slot);
    }
    return -1;
}

std::string InventoryAccess::offhandIdentifier(const sdk::Player* player) const {
    if (!player || !m_getOffhandSlot) return {};
    const void* offhand = m_getOffhandSlot(player);
    if (!isReadableMemory(offhand, ItemStackSize)) return {};
    return stackIdentifier(offhand);
}

int InventoryAccess::selectedHotbarSlot(const sdk::Player* player) const {
    if (!player) return -1;
    const void* inventory = read<const void*>(player, PlayerInventory);
    if (!inventory) return -1;
    const int slot = read<int>(inventory, PlayerInventorySelectedSlot);
    return slot >= 0 && slot < 9 ? slot : -1;
}

std::string InventoryAccess::selectedHotbarIdentifier(const sdk::Player* player) const {
    if (!player) return {};
    std::uintptr_t begin = 0;
    std::size_t size = 0;
    if (!inventorySlots(player, begin, size) || !begin || size < 9) return {};
    const int selected = selectedHotbarSlot(player);
    if (selected < 0 || static_cast<std::size_t>(selected) >= size) return {};
    const void* stack = reinterpret_cast<const void*>(begin +
        static_cast<std::size_t>(selected) * ItemStackSize);
    return stackIdentifier(stack);
}

bool InventoryAccess::selectHotbarSlot(sdk::Player* player, int slot) const {
    if (!player || slot < 0 || slot > 8) return false;
    std::uintptr_t begin = 0;
    std::size_t size = 0;
    if (!inventorySlots(player, begin, size) || !begin || size < 9) return false;

    void* inventory = const_cast<void*>(read<const void*>(player, PlayerInventory));
    if (!inventory) return false;

    // Prefer the engine's native PlayerInventory::selectSlot method if this build exports it.
    using SelectSlotFn = bool (*)(void*, int, int);
    static SelectSlotFn nativeSelectSlot = []() -> SelectSlotFn {
        constexpr const char* symbol = "_ZN15PlayerInventory10selectSlotEi11ContainerID";
        void* address = dlsym(RTLD_DEFAULT, symbol);
        if (!address) {
            void* library = dlopen("libminecraftpe.so", RTLD_NOW | RTLD_NOLOAD);
            if (library) {
                address = dlsym(library, symbol);
                dlclose(library);
            }
        }
        return reinterpret_cast<SelectSlotFn>(address);
    }();

    if (nativeSelectSlot) {
        const bool result = nativeSelectSlot(inventory, slot, 0); // ContainerID::Inventory
        return result && selectedHotbarSlot(player) == slot;
    }

    // Some Android builds strip the native method's symbol. Fall back to the verified
    // selected-slot field, then ask the native player to publish the updated selection.
    auto* selected = reinterpret_cast<int*>(
        static_cast<std::byte*>(inventory) + PlayerInventorySelectedSlot);
    *selected = slot;
    if (selectedHotbarSlot(player) != slot) return false;
    bedrocktools::sdk::virtualCall<void>(
        player, bedrocktools::sdk::offsets::VTable::PlayerSendInventory, true);
    return selectedHotbarSlot(player) == slot;
}

OffhandTransferResult InventoryAccess::moveInventoryItemToOffhand(sdk::Player* player, int slot) const {
    if (!player) return OffhandTransferResult::InvalidPlayer;
    if (slot < 0 || slot >= 64) return OffhandTransferResult::InvalidSlot;
    if (!m_getOffhandSlot) return OffhandTransferResult::RuntimeApiUnavailable;

    std::uintptr_t slotsBegin = 0;
    std::size_t slotCount = 0;
    if (!inventorySlots(player, slotsBegin, slotCount) || !slotsBegin ||
        static_cast<std::size_t>(slot) >= slotCount) {
        return OffhandTransferResult::InvalidSlot;
    }

    const auto offhandAddress = reinterpret_cast<std::uintptr_t>(m_getOffhandSlot(player));
    if (!isReadableMemory(reinterpret_cast<const void*>(offhandAddress), ItemStackSize)) {
        return OffhandTransferResult::RuntimeApiUnavailable;
    }
    const void* offhand = reinterpret_cast<const void*>(offhandAddress);
    const std::string offhandId = stackIdentifier(offhand);
    if (offhandId.find("totem") != std::string::npos) return OffhandTransferResult::AlreadyEquipped;
    if (!offhandId.empty()) return OffhandTransferResult::OffhandOccupied;

    const auto sourceAddress = slotsBegin + static_cast<std::size_t>(slot) * ItemStackSize;
    const void* sourceItem = reinterpret_cast<const void*>(sourceAddress);
    if (stackIdentifier(sourceItem).find("totem") == std::string::npos ||
        countStack(sourceItem, "totem") <= 0) {
        return OffhandTransferResult::ItemUnavailable;
    }

    // Player::mTransactionManager is at 0xA28 in the target's current Player layout.
    // Verify the embedded Player& before calling any native transaction method.
    auto* manager = reinterpret_cast<void*>(
        reinterpret_cast<std::uintptr_t>(player) + PlayerTransactionManagerOffset);
    if (!isReadableMemory(manager, TransactionManagerSize) ||
        read<const void*>(manager, 0) != player || !validExpectedActionsVector(manager)) {
        return OffhandTransferResult::InvalidNativeState;
    }

    using InventoryActionCtorFn = void (*)(void*, NativeInventorySource, std::uint32_t, const void*, const void*);
    using InventoryActionDtorFn = void (*)(void*);
    using AddActionFn = void (*)(void*, const void*, bool);
    using RemoveItemFn = void (*)(void*, int, int);
    using SetOffhandFn = void (*)(void*, const void*);

    auto actionCtor = reinterpret_cast<InventoryActionCtorFn>(
        resolveMinecraftSymbol("_ZN15InventoryActionC1E15InventorySourcejRK9ItemStackS4_"));
    if (!actionCtor) actionCtor = reinterpret_cast<InventoryActionCtorFn>(
        resolveMinecraftSymbol("_ZN15InventoryActionC2E15InventorySourcejRK9ItemStackS4_"));
    auto actionDtor = reinterpret_cast<InventoryActionDtorFn>(
        resolveMinecraftSymbol("_ZN15InventoryActionD1Ev"));
    if (!actionDtor) actionDtor = reinterpret_cast<InventoryActionDtorFn>(
        resolveMinecraftSymbol("_ZN15InventoryActionD2Ev"));
    auto addAction = reinterpret_cast<AddActionFn>(
        resolveMinecraftSymbol("_ZN27InventoryTransactionManager9addActionERK15InventoryActionb"));
    auto removeItem = reinterpret_cast<RemoveItemFn>(
        resolveMinecraftSymbol("_ZN16FillingContainer10removeItemEii"));

    void** vtable = nullptr;
    if (isReadableMemory(player, sizeof(void*))) {
        vtable = *reinterpret_cast<void***>(player);
    }
    SetOffhandFn setOffhand = nullptr;
    if (vtable && isReadableMemory(vtable, (PlayerSetOffhandVTable + 1) * sizeof(void*))) {
        void* entry = vtable[PlayerSetOffhandVTable];
        if (isMinecraftFunction(entry)) setOffhand = reinterpret_cast<SetOffhandFn>(entry);
    }

    if (!actionCtor || !actionDtor || !addAction || !removeItem || !setOffhand) {
        return OffhandTransferResult::RuntimeApiUnavailable;
    }

    // Keep ItemStack snapshots and action objects alive until addAction has copied them.
    alignas(8) std::array<std::byte, ItemStackSize> totemSnapshot{};
    alignas(8) std::array<std::byte, ItemStackSize> emptyOffhandSnapshot{};
    std::memcpy(totemSnapshot.data(), sourceItem, ItemStackSize);
    std::memcpy(emptyOffhandSnapshot.data(), offhand, ItemStackSize);

    alignas(8) std::array<std::byte, TransactionActionSize> inventoryAction{};
    alignas(8) std::array<std::byte, TransactionActionSize> offhandAction{};
    NativeInventorySource inventorySource{0, InventoryContainerId, {0, 0, 0}, 0};
    NativeInventorySource offhandSource{0, OffhandContainerId, {0, 0, 0}, 0};
    actionCtor(inventoryAction.data(), inventorySource, static_cast<std::uint32_t>(slot),
               totemSnapshot.data(), emptyOffhandSnapshot.data());
    actionCtor(offhandAction.data(), offhandSource, 0,
               emptyOffhandSnapshot.data(), totemSnapshot.data());

    // Only use the supported engine mutators and publish the paired transaction;
    // never edit ItemStack bytes or the remote server's inventory state directly.
    setOffhand(player, totemSnapshot.data());
    removeItem(const_cast<void*>(read<const void*>(read<const void*>(player, PlayerInventory),
                                                   PlayerInventoryContainer)), slot, 1);

    const bool equipped = stackIdentifier(m_getOffhandSlot(player)).find("totem") != std::string::npos;
    const void* updatedSource = reinterpret_cast<const void*>(
        slotsBegin + static_cast<std::size_t>(slot) * ItemStackSize);
    const bool removed = stackIdentifier(updatedSource).find("totem") == std::string::npos;
    if (!equipped || !removed) {
        // The native route did not complete coherently. Do not add a transaction
        // that describes state we cannot observe locally.
        actionDtor(inventoryAction.data());
        actionDtor(offhandAction.data());
        return OffhandTransferResult::VerificationFailed;
    }

    addAction(manager, inventoryAction.data(), false);
    addAction(manager, offhandAction.data(), false);
    actionDtor(inventoryAction.data());
    actionDtor(offhandAction.data());
    return OffhandTransferResult::Success;
}

}
