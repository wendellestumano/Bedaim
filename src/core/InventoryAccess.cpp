#include "InventoryAccess.hpp"
#include "Diagnostics.hpp"

#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/offsets/Inventory.hpp>
#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <link.h>
#include <iterator>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace bedrocktools::core {
namespace {
using namespace bedrocktools::sdk::offsets::Inventory;

template<class T>
T read(const void* object, std::size_t offset) {
    return *reinterpret_cast<const T*>(static_cast<const std::byte*>(object) + offset);
}

bool isReadableMemory(const void* pointer, std::size_t size);

template<class T>
bool safeRead(const void* object, std::size_t offset, T& value) {
    if (!object) return false;
    const auto base = reinterpret_cast<std::uintptr_t>(object);
    if (offset > std::numeric_limits<std::uintptr_t>::max() - base) return false;
    const auto* field = reinterpret_cast<const void*>(base + offset);
    if (!isReadableMemory(field, sizeof(T))) return false;
    std::memcpy(&value, field, sizeof(T));
    return true;
}

bool inventorySlots(const void* player, std::uintptr_t& begin, std::size_t& size) {
    begin = 0;
    size = 0;
    if (!player) return false;

    const void* proxy = nullptr;
    if (!safeRead(player, PlayerInventory, proxy) || !proxy ||
        !isReadableMemory(proxy, sizeof(void*))) return false;

    const void* inventory = nullptr;
    if (!safeRead(proxy, PlayerInventoryContainer, inventory) || !inventory ||
        !isReadableMemory(inventory, sizeof(void*))) return false;

    std::uintptr_t end = 0;
    if (!safeRead(inventory, FillingContainerItems, begin) ||
        !safeRead(inventory, FillingContainerItems + sizeof(void*), end)) return false;
    if (!begin && !end) return true;
    if (!begin || end < begin || begin % alignof(void*) != 0) return false;

    const auto bytes = end - begin;
    if (bytes % ItemStackSize != 0) return false;
    size = static_cast<std::size_t>(bytes / ItemStackSize);
    if (size > 64) return false;
    if (size != 0 && !isReadableMemory(reinterpret_cast<const void*>(begin),
                                       size * ItemStackSize)) return false;
    return true;
}

constexpr std::size_t PlayerTransactionManagerOffset = 0xA28;
constexpr std::size_t TransactionManagerSize = 48;
constexpr std::size_t TransactionManagerExpectedActions = 16;
constexpr std::size_t TransactionActionSize = 512;
constexpr std::size_t ContainerSetItemVTable = 13;
constexpr std::size_t ContainerRemoveItemVTable = 15;
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

struct ReadableRegion {
    std::uintptr_t begin{};
    std::uintptr_t end{};
};

bool isReadableMemory(const void* pointer, std::size_t size) {
    if (!pointer || size == 0) return false;
    const auto begin = reinterpret_cast<std::uintptr_t>(pointer);
    if (size > std::numeric_limits<std::uintptr_t>::max() - begin) return false;
    const auto end = begin + size;

    // Inventory validation is a hot path. Reopening and scanning /proc/self/maps
    // for each stack/vtable pointer made cost grow with both inventory size and
    // process mapping count. Reuse a brief readable-map snapshot instead.
    // Each game thread performs many checks in one tick. Thread-local cache
    // avoids serializing all inventory checks on a shared mutex and keeps the
    // process map parse out of the normal per-slot validation path.
    thread_local std::vector<ReadableRegion> readableRegions;
    thread_local std::chrono::steady_clock::time_point mapsRefreshed{};
    thread_local bool mapsCacheValid = false;

    const auto now = std::chrono::steady_clock::now();
    if (!mapsCacheValid || now < mapsRefreshed ||
        now - mapsRefreshed > std::chrono::milliseconds(50)) {
        std::vector<ReadableRegion> refreshed;
        std::FILE* maps = std::fopen("/proc/self/maps", "r");
        if (!maps) {
            readableRegions.clear();
            mapsCacheValid = false;
            return false;
        }
        char line[512]{};
        while (std::fgets(line, sizeof(line), maps)) {
            unsigned long long regionBegin = 0, regionEnd = 0;
            char permissions[5]{};
            if (std::sscanf(line, "%llx-%llx %4s", &regionBegin, &regionEnd, permissions) == 3 &&
                permissions[0] == 'r' && regionEnd > regionBegin) {
                refreshed.push_back({static_cast<std::uintptr_t>(regionBegin),
                                     static_cast<std::uintptr_t>(regionEnd)});
            }
        }
        std::fclose(maps);
        if (refreshed.empty()) {
            readableRegions.clear();
            mapsCacheValid = false;
            return false;
        }
        readableRegions.swap(refreshed);
        mapsRefreshed = now;
        mapsCacheValid = true;
    }

    const auto it = std::upper_bound(
        readableRegions.begin(), readableRegions.end(), begin,
        [](std::uintptr_t address, const ReadableRegion& region) {
            return address < region.begin;
        });
    if (it == readableRegions.begin()) return false;
    const auto& region = *std::prev(it);
    return begin >= region.begin && end <= region.end;
}

struct MinecraftExecutableAddressQuery {
    std::uintptr_t imageBase{};
    std::uintptr_t address{};
    bool found{};
};

int inspectMinecraftExecutableAddress(struct dl_phdr_info* info, std::size_t, void* opaque) {
    if (!info || !info->dlpi_name) return 0;
    const std::string_view imageName(info->dlpi_name);
    const auto slash = imageName.find_last_of('/');
    const auto basename = slash == std::string_view::npos
        ? imageName : imageName.substr(slash + 1);
    auto& query = *static_cast<MinecraftExecutableAddressQuery*>(opaque);
    if (basename != "libminecraftpe.so" ||
        static_cast<std::uintptr_t>(info->dlpi_addr) != query.imageBase) return 0;

    const auto maximum = std::numeric_limits<std::uintptr_t>::max();
    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
        const auto& segment = info->dlpi_phdr[i];
        if (segment.p_type != PT_LOAD || (segment.p_flags & PF_X) == 0) continue;
        const auto offset = static_cast<std::uintptr_t>(segment.p_vaddr);
        const auto size = static_cast<std::uintptr_t>(segment.p_memsz);
        if (offset > maximum - query.imageBase) continue;
        const auto begin = query.imageBase + offset;
        if (size > maximum - begin) continue;
        const auto end = begin + size;
        if (query.address >= begin && query.address < end) {
            query.found = true;
            return 1;
        }
    }
    return 0;
}

bool isMinecraftFunction(const void* address) {
    if (!address) return false;
    Dl_info info{};
    if (dladdr(address, &info) == 0 || !info.dli_fname || !info.dli_fbase) return false;
    const std::string_view imageName(info.dli_fname);
    const auto slash = imageName.find_last_of('/');
    const auto basename = slash == std::string_view::npos
        ? imageName : imageName.substr(slash + 1);
    if (basename != "libminecraftpe.so") return false;

    // dladdr alone also accepts data addresses inside the shared object.
    // A vtable entry must resolve into an executable PT_LOAD segment before
    // InventoryAccess is allowed to call it as a native function.
    MinecraftExecutableAddressQuery query{
        reinterpret_cast<std::uintptr_t>(info.dli_fbase),
        reinterpret_cast<std::uintptr_t>(address),
        false
    };
    if (!query.imageBase || !query.address) return false;
    dl_iterate_phdr(&inspectMinecraftExecutableAddress, &query);
    return query.found;
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
    m_offhandGetterQuarantined.store(false, std::memory_order_release);
    // Prefer exact exported accessors over the generic signature pattern. The
    // old pattern could resolve on newer builds but return an unreadable pointer,
    // which made AutoTotem mistake an unknown offhand state for an empty slot.
    struct GetterSymbol { const char* name; const char* label; };
    constexpr GetterSymbol candidates[] = {
        {"_ZNK5Actor14getOffhandSlotEv", "Actor::getOffhandSlot const symbol"},
        {"_ZN5Actor14getOffhandSlotEv", "Actor::getOffhandSlot symbol"},
        {"_ZNK6Player14getOffhandSlotEv", "Player::getOffhandSlot const symbol"},
        {"_ZN6Player14getOffhandSlotEv", "Player::getOffhandSlot symbol"},
        {"_ZNK11LocalPlayer14getOffhandSlotEv", "LocalPlayer::getOffhandSlot const symbol"},
        {"_ZN11LocalPlayer14getOffhandSlotEv", "LocalPlayer::getOffhandSlot symbol"}
    };
    m_getOffhandSlot = nullptr;
    m_getOffhandSlotSource = "unresolved";
    for (const auto& candidate : candidates) {
        if (void* address = resolveMinecraftSymbol(candidate.name)) {
            m_getOffhandSlot = reinterpret_cast<GetOffhandSlotFn>(address);
            m_getOffhandSlotSource = candidate.label;
            break;
        }
    }
    if (!m_getOffhandSlot) {
        m_getOffhandSlot = reinterpret_cast<GetOffhandSlotFn>(
            memory::resolve(memory::SignatureId::ActorGetOffhandSlot));
        if (m_getOffhandSlot) m_getOffhandSlotSource = "signature-pattern fallback (runtime pointer validation required)";
    }
    bedrocktools::diagnostics::probe(
        m_getOffhandSlot ? bedrocktools::diagnostics::Level::Info : bedrocktools::diagnostics::Level::Warning,
        "InventoryAccess", "offhand_reader",
        m_getOffhandSlot ? "ready" : "signature_unavailable",
        std::string("source=") + m_getOffhandSlotSource +
            (m_getOffhandSlot ? "; returned ItemStack pointers are validated before use"
                              : "; offhand transfers fail closed"));
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
    // Never dereference the offhand getter directly here: the diagnostic logs
    // showed that a signature-pattern fallback can return an unreadable pointer.
    // Reuse the same validation/quarantine path as AutoTotem before counting it.
    if (!identifierPart.empty() && m_getOffhandSlot &&
        !m_offhandGetterQuarantined.load(std::memory_order_acquire)) {
        std::string offhandId;
        if (readOffhandIdentifier(player, offhandId, nullptr) &&
            offhandId.find(identifierPart) != std::string::npos) {
            ++total; // The offhand slot holds one stack; the item itself is validated.
        }
    }
    return total;
}

std::string InventoryAccess::stackIdentifier(const void* stack) const {
    // The 26.60 PlayerInventory offset is maintained in sdk/offsets/Inventory.hpp.
    // Keep every nested ItemStack/Item/vtable read inside a readable mapped region;
    // an unreadable identifier is not evidence that the slot is empty.
    if (!stack || !isReadableMemory(stack, ItemStackSize)) return {};

    std::uint8_t valid = 0;
    std::uint8_t count = 0;
    if (!safeRead(stack, ItemStackValid, valid) || !safeRead(stack, ItemStackCount, count) ||
        valid == 0 || count == 0) return {};

    const void* counter = nullptr;
    if (!safeRead(stack, ItemStackItemCounter, counter) || !counter ||
        !isReadableMemory(counter, sizeof(void*))) return {};

    const void* item = nullptr;
    if (!safeRead(counter, 0, item) || !item || !isReadableMemory(item, sizeof(void*))) return {};

    const std::uintptr_t* vtable = nullptr;
    if (!safeRead(item, 0, vtable) || !vtable ||
        !isReadableMemory(vtable, (ItemGetDescriptionIdVtableIndex + 1) * sizeof(std::uintptr_t)))
        return {};

    std::uintptr_t methodAddress = 0;
    if (!safeRead(vtable, ItemGetDescriptionIdVtableIndex * sizeof(std::uintptr_t), methodAddress) ||
        methodAddress == 0) return {};

    using GetDescriptionIdFn = const std::string& (*)(const void*);
    try {
        const auto& identifier = reinterpret_cast<GetDescriptionIdFn>(methodAddress)(item);
        if (identifier.empty() || identifier.size() > 128) return {};
        return identifier;
    } catch (...) {
        return {};
    }
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

std::string InventoryAccess::hotbarSlotIdentifier(const sdk::Player* player, int slot) const {
    if (!player || slot < 0 || slot > 8) return {};
    std::uintptr_t begin = 0;
    std::size_t size = 0;
    if (!inventorySlots(player, begin, size) || !begin || size < 9 ||
        static_cast<std::size_t>(slot) >= size) return {};
    const void* stack = reinterpret_cast<const void*>(begin +
        static_cast<std::size_t>(slot) * ItemStackSize);
    return stackIdentifier(stack);
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

bool InventoryAccess::readOffhandState(const sdk::Player* player,
                                        std::string& identifier,
                                        bool& empty,
                                        std::string* error) const {
    identifier.clear();
    empty = false;
    auto fail = [&](std::string message) {
        if (error) *error = std::move(message);
        return false;
    };
    if (!player)
        return fail("LocalPlayerTickEvent supplied a null Player pointer");

    // Confirm the 26.60 PlayerInventory/container fields before calling the
    // native offhand getter. Read each field separately to avoid crossing pages.
    const void* inventoryProxy = nullptr;
    if (!safeRead(player, PlayerInventory, inventoryProxy) || !inventoryProxy ||
        !isReadableMemory(inventoryProxy, sizeof(void*)))
        return fail("26.60 PlayerInventory pointer is null or unreadable");

    const void* container = nullptr;
    if (!safeRead(inventoryProxy, PlayerInventoryContainer, container) || !container ||
        !isReadableMemory(container, sizeof(void*)))
        return fail("26.60 inventory container pointer is null or unreadable");

    if (!m_getOffhandSlot)
        return fail("no validated Actor/Player::getOffhandSlot entry point is available");
    if (m_offhandGetterQuarantined.load(std::memory_order_acquire))
        return fail("offhand getter quarantined after an unreadable pointer; restart Bedaim to retry safely");

    const void* offhand = m_getOffhandSlot(player);
    if (!isReadableMemory(offhand, ItemStackSize)) {
        char detail[256]{};
        std::snprintf(detail, sizeof(detail),
            "getter source=%s returned unreadable ItemStack pointer=%p; getter quarantined",
            m_getOffhandSlotSource.c_str(), offhand);
        m_offhandGetterQuarantined.store(true, std::memory_order_release);
        return fail(detail);
    }

    std::uint8_t valid = 0;
    std::uint8_t count = 0;
    if (!safeRead(offhand, ItemStackValid, valid) || !safeRead(offhand, ItemStackCount, count))
        return fail("offhand ItemStack validity/count fields are unreadable");

    if (valid == 0 || count == 0) {
        empty = true;
        if (error) error->clear();
        return true;
    }

    identifier = stackIdentifier(offhand);
    if (identifier.empty())
        return fail("offhand ItemStack is non-empty but its item identifier could not be resolved; refusing to treat it as empty");

    empty = false;
    if (error) error->clear();
    return true;
}

bool InventoryAccess::readOffhandIdentifier(const sdk::Player* player,
                                            std::string& identifier,
                                            std::string* error) const {
    bool empty = false;
    return readOffhandState(player, identifier, empty, error);
}

std::string InventoryAccess::offhandIdentifier(const sdk::Player* player) const {
    std::string identifier;
    readOffhandIdentifier(player, identifier, nullptr);
    return identifier;
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
    auto report = [](OffhandTransferResult result, std::string_view detail) {
        const char* state = "unknown";
        using R = OffhandTransferResult;
        switch (result) {
            case R::Success: state = "success"; break;
            case R::AlreadyEquipped: state = "already_equipped"; break;
            case R::InvalidPlayer: state = "invalid_player"; break;
            case R::InvalidSlot: state = "invalid_slot"; break;
            case R::ItemUnavailable: state = "totem_unavailable"; break;
            case R::OffhandOccupied: state = "offhand_occupied"; break;
            case R::RuntimeApiUnavailable: state = "native_api_unavailable"; break;
            case R::InvalidNativeState: state = "native_state_invalid"; break;
            case R::VerificationFailed: state = "verification_failed"; break;
        }
        const bool safeNonError = result == R::Success || result == R::AlreadyEquipped;
        bedrocktools::diagnostics::probe(
            safeNonError ? bedrocktools::diagnostics::Level::Info : bedrocktools::diagnostics::Level::Warning,
            "InventoryAccess", "offhand_transfer", state, detail);
        return result;
    };
    if (!player) return report(OffhandTransferResult::InvalidPlayer, "player pointer is null");
    if (slot < 0 || slot >= 64) return report(OffhandTransferResult::InvalidSlot, "source slot or inventory slot range is invalid");
    if (!m_getOffhandSlot) return report(OffhandTransferResult::RuntimeApiUnavailable, "Actor/Player offhand getter unresolved");
    if (m_offhandGetterQuarantined.load(std::memory_order_acquire))
        return report(OffhandTransferResult::InvalidNativeState,
                      "offhand getter quarantined after an unreadable pointer; restart Bedaim to retry safely");

    std::uintptr_t slotsBegin = 0;
    std::size_t slotCount = 0;
    if (!inventorySlots(player, slotsBegin, slotCount) || !slotsBegin ||
        static_cast<std::size_t>(slot) >= slotCount) {
        return report(OffhandTransferResult::InvalidSlot, "source slot or inventory slot range is invalid");
    }

    const auto offhandAddress = reinterpret_cast<std::uintptr_t>(m_getOffhandSlot(player));
    if (!isReadableMemory(reinterpret_cast<const void*>(offhandAddress), ItemStackSize)) {
        char detail[256]{};
        std::snprintf(detail, sizeof(detail),
            "getter source=%s returned unreadable ItemStack pointer=%p; getter quarantined",
            m_getOffhandSlotSource.c_str(), reinterpret_cast<const void*>(offhandAddress));
        m_offhandGetterQuarantined.store(true, std::memory_order_release);
        return report(OffhandTransferResult::InvalidNativeState, detail);
    }
    const void* offhand = reinterpret_cast<const void*>(offhandAddress);
    const std::string offhandId = stackIdentifier(offhand);
    if (offhandId.find("totem") != std::string::npos) return report(OffhandTransferResult::AlreadyEquipped, "totem already in offhand");
    if (!offhandId.empty()) return report(OffhandTransferResult::OffhandOccupied, "offhand contains another item");

    const auto sourceAddress = slotsBegin + static_cast<std::size_t>(slot) * ItemStackSize;
    const void* sourceItem = reinterpret_cast<const void*>(sourceAddress);
    if (stackIdentifier(sourceItem).find("totem") == std::string::npos ||
        countStack(sourceItem, "totem") <= 0) {
        return report(OffhandTransferResult::ItemUnavailable, "selected source slot does not contain a totem");
    }

    // Player::mTransactionManager is at 0xA28 in the target's current Player layout.
    // Verify the embedded Player& before calling any native transaction method.
    auto* manager = reinterpret_cast<void*>(
        reinterpret_cast<std::uintptr_t>(player) + PlayerTransactionManagerOffset);
    if (!isReadableMemory(manager, TransactionManagerSize) ||
        read<const void*>(manager, 0) != player || !validExpectedActionsVector(manager)) {
        return report(OffhandTransferResult::InvalidNativeState,
                      "Player transaction-manager layout/owner/expected-actions vector failed validation");
    }

    const void* playerInventory = read<const void*>(player, PlayerInventory);
    void* container = playerInventory
        ? const_cast<void*>(read<const void*>(playerInventory, PlayerInventoryContainer))
        : nullptr;
    if (!container || !isReadableMemory(container, sizeof(void*))) {
        return report(OffhandTransferResult::InvalidNativeState,
                      "player inventory container pointer is null or unreadable");
    }

    void** containerVtable = nullptr;
    if (isReadableMemory(container, sizeof(void*))) {
        containerVtable = *reinterpret_cast<void***>(container);
    }
    if (!containerVtable ||
        !isReadableMemory(containerVtable, (ContainerRemoveItemVTable + 1) * sizeof(void*))) {
        return report(OffhandTransferResult::RuntimeApiUnavailable,
                      "container vtable is null or unreadable");
    }

    using InventoryActionCtorFn = void (*)(void*, NativeInventorySource, std::uint32_t, const void*, const void*);
    using InventoryActionDtorFn = void (*)(void*);
    using AddActionFn = void (*)(void*, const void*, bool);
    using RemoveItemFn = void (*)(void*, int, int);
    using SetItemFn = void (*)(void*, int, const void*);
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
    if (!removeItem) {
        // FillingContainer inherits Container's declared virtual method order.
        // The slot fallback is used only after validating it points into Minecraft.
        void* entry = containerVtable[ContainerRemoveItemVTable];
        if (isMinecraftFunction(entry)) removeItem = reinterpret_cast<RemoveItemFn>(entry);
    }

    // Do not infer the setter from an adjacent vtable slot: getCarriedItem and
    // setCarriedItem are adjacent on several Bedrock layouts, which can overwrite
    // the main hand instead of the offhand. Resolve the named native method and
    // fail closed if this runtime does not export a verified entry point.
    // The offhand accessor is an Actor API in this runtime family; some builds
    // export its setter on Actor instead of Player. Resolve the exact named native
    // symbol and fail closed if none of these declarations are exported.
    SetOffhandFn setOffhand = reinterpret_cast<SetOffhandFn>(
        resolveMinecraftSymbol("_ZN5Actor14setOffhandSlotERK9ItemStack"));
    if (!setOffhand) {
        setOffhand = reinterpret_cast<SetOffhandFn>(
            resolveMinecraftSymbol("_ZN6Player14setOffhandSlotERK9ItemStack"));
    }
    if (!setOffhand) {
        setOffhand = reinterpret_cast<SetOffhandFn>(
            resolveMinecraftSymbol("_ZN11LocalPlayer14setOffhandSlotERK9ItemStack"));
    }

    if (!actionCtor || !actionDtor || !addAction || !removeItem || !setOffhand) {
        std::string missing;
        auto addMissing = [&](bool absent, const char* name) {
            if (!absent) return;
            if (!missing.empty()) missing += ", ";
            missing += name;
        };
        addMissing(!actionCtor, "InventoryAction constructor");
        addMissing(!actionDtor, "InventoryAction destructor");
        addMissing(!addAction, "InventoryTransactionManager::addAction");
        addMissing(!removeItem, "FillingContainer::removeItem");
        addMissing(!setOffhand, "Actor/Player::setOffhandSlot");
        return report(OffhandTransferResult::RuntimeApiUnavailable,
                      std::string("missing validated native entry points: ") + missing);
    }

    // ItemStack members inside InventoryAction are real engine-constructed
    // snapshots, unlike a byte-copied ItemStack (which could duplicate native
    // ownership state). The current ABI layout places from/to ItemStack at 208/360.
    constexpr std::size_t ActionFromItemOffset = 208;
    constexpr std::size_t ActionToItemOffset = 360;
    static_assert(ActionFromItemOffset + ItemStackSize <= TransactionActionSize);
    static_assert(ActionToItemOffset + ItemStackSize <= TransactionActionSize);

    alignas(8) std::array<std::byte, TransactionActionSize> inventoryAction{};
    alignas(8) std::array<std::byte, TransactionActionSize> offhandAction{};
    NativeInventorySource inventorySource{0, InventoryContainerId, {0, 0, 0}, 0};
    NativeInventorySource offhandSource{0, OffhandContainerId, {0, 0, 0}, 0};
    actionCtor(inventoryAction.data(), inventorySource, static_cast<std::uint32_t>(slot),
               sourceItem, offhand);
    actionCtor(offhandAction.data(), offhandSource, 0, offhand, sourceItem);

    const void* emptyOffhandSnapshot = inventoryAction.data() + ActionToItemOffset;
    const void* totemSnapshot = offhandAction.data() + ActionToItemOffset;
    // First equip using the engine's setter and verify that the offhand changed;
    // only then remove the source slot so a failed setter cannot destroy the item.
    setOffhand(player, totemSnapshot);
    std::string afterSetId;
    std::string afterSetError;
    const bool afterSetReadable = readOffhandIdentifier(player, afterSetId, &afterSetError);
    const bool equippedBeforeRemoval = afterSetReadable &&
        afterSetId.find("totem") != std::string::npos;
    if (!equippedBeforeRemoval) {
        setOffhand(player, emptyOffhandSnapshot);
        actionDtor(inventoryAction.data());
        actionDtor(offhandAction.data());
        const std::string detail = afterSetReadable
            ? "offhand did not contain the totem after setter; rollback attempted"
            : "offhand unreadable after setter: " + afterSetError + "; rollback attempted";
        return report(OffhandTransferResult::VerificationFailed, detail);
    }

    removeItem(container, slot, 1);
    std::string afterRemoveId;
    std::string afterRemoveError;
    const bool afterRemoveReadable = readOffhandIdentifier(player, afterRemoveId, &afterRemoveError);
    const bool equipped = afterRemoveReadable && afterRemoveId.find("totem") != std::string::npos;
    const void* updatedSource = reinterpret_cast<const void*>(
        slotsBegin + static_cast<std::size_t>(slot) * ItemStackSize);
    const bool removed = stackIdentifier(updatedSource).find("totem") == std::string::npos;
    if (!equipped || !removed) {
        if (!removed) {
            // The source item remained, so roll back the local offhand change.
            setOffhand(player, emptyOffhandSnapshot);
        } else if (!equipped) {
            // If the source was cleared but the offhand is not showing the totem,
            // attempt to restore the item before returning failure.
            auto setItem = reinterpret_cast<SetItemFn>(
                resolveMinecraftSymbol("_ZN16FillingContainer7setItemEiRK9ItemStack"));
            if (!setItem) {
                void* entry = containerVtable[ContainerSetItemVTable];
                if (isMinecraftFunction(entry)) setItem = reinterpret_cast<SetItemFn>(entry);
            }
            if (setItem) setItem(container, slot, totemSnapshot);
        }
        actionDtor(inventoryAction.data());
        actionDtor(offhandAction.data());
        const std::string detail = !afterRemoveReadable
            ? "offhand unreadable after source removal: " + afterRemoveError + "; rollback/restoration attempted"
            : "offhand/source postcondition failed; rollback/restoration attempted";
        return report(OffhandTransferResult::VerificationFailed, detail);
    }

    addAction(manager, inventoryAction.data(), false);
    addAction(manager, offhandAction.data(), false);
    actionDtor(inventoryAction.data());
    actionDtor(offhandAction.data());
    return report(OffhandTransferResult::Success, "offhand and source slot verified");
}

}
