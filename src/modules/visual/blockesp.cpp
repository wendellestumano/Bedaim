#include "blockesp.hpp"
#include "hitbox.hpp"
#include "../ModuleRegistry.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/world/BlockSource.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>
#include <bedrocktools/sdk/render/Block.hpp>
#include <pl/ModMenu.hpp>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <string_view>

namespace {
using BlockSourceGetBlockFn = void* (*)(void*, const bedrocktools::sdk::BlockPos&);
std::atomic<BlockSourceGetBlockFn> g_blockEspGetBlock{nullptr};
constexpr std::size_t kMaxBlockEspCache = 8192;
constexpr std::size_t kMaxBlockEspRendered = 128;

void resolveBlockGetBlock() {
    if (g_blockEspGetBlock.load(std::memory_order_acquire)) return;
    const auto address = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::BlockSourceGetBlock);
    if (address) g_blockEspGetBlock.store(reinterpret_cast<BlockSourceGetBlockFn>(address), std::memory_order_release);
}
std::string configText(const nlohmann::json& j, const char* key, const char* fallback) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return fallback;
    if (it->is_string()) return it->get<std::string>();
    if (it->is_boolean()) return it->get<bool>() ? "true" : "false";
    return it->dump();
}
bool parseBool(std::string_view value, bool& out) {
    if (value == "true" || value == "1" || value == "on") { out = true; return true; }
    if (value == "false" || value == "0" || value == "off") { out = false; return true; }
    return false;
}
bool parseInt(std::string_view value, int& out) {
    try {
        std::string s(value); std::size_t n = 0; int v = std::stoi(s, &n);
        while (n < s.size() && std::isspace(static_cast<unsigned char>(s[n]))) ++n;
        if (n != s.size()) return false; out = v; return true;
    } catch (...) { return false; }
}
bool parseFloat(std::string_view value, float& out) {
    try {
        std::string s(value); std::size_t n = 0; float v = std::stof(s, &n);
        while (n < s.size() && std::isspace(static_cast<unsigned char>(s[n]))) ++n;
        if (n != s.size() || !std::isfinite(v)) return false; out = v; return true;
    } catch (...) { return false; }
}
bool parseColor(std::string_view value, std::uint32_t& out) {
    std::string s(value); if (!s.empty() && s.front() == '#') s.erase(s.begin());
    if (s.size() != 6 && s.size() != 8) return false;
    if (!std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isxdigit(c) != 0; })) return false;
    try {
        const auto n = static_cast<std::uint32_t>(std::stoul(s, nullptr, 16));
        out = s.size() == 6 ? (0xFF000000u | n) : n; return true;
    } catch (...) { return false; }
}
void loadColor(const nlohmann::json& j, const char* key, std::uint32_t& value) {
    if (!j.contains(key) || !j[key].is_string()) return;
    std::uint32_t parsed = 0;
    if (parseColor(j[key].get<std::string>(), parsed)) value = parsed;
}
std::string stripNamespace(std::string_view name) {
    const auto pos = name.find(':');
    if (pos != std::string_view::npos) name.remove_prefix(pos + 1);
    std::string out(name);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}
const std::vector<std::string>& utilityBlockNames() {
    static const std::vector<std::string> names = {
        "crafting_table","brewing_stand","anvil","chipped_anvil","damaged_anvil",
        "smithing_table","enchanting_table","loom","stonecutter","cartography_table",
        "fletching_table","grindstone","composter","lectern","beacon","bell","jukebox",
        "note_block","observer","piston","sticky_piston","crafter","redstone_block",
        "lever","target","block_button","redstone_torch","redstone_wire","redstone_lamp",
        "block_pressure_plate"
    };
    return names;
}
std::string utilityTitle(std::string name) {
    if (name == "block_button") return "Block Button";
    if (name == "block_pressure_plate") return "Pressure Plate";
    std::replace(name.begin(), name.end(), '_', ' ');
    bool upper = true;
    for (char& ch : name) {
        if (upper) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        upper = ch == ' ';
    }
    return "Show " + name;
}
}

BlockEspModule::BlockEspModule()
    : Module("BlockESP", "Colored outlines and tracers for useful blocks: shulkers, storage, redstone and workstations.") {
    showInMenu = true;
    for (const auto& name : utilityBlockNames()) m_utilityFilters.emplace(name, true);
}

std::size_t BlockEspModule::PositionHash::operator()(const PositionKey& p) const noexcept {
    std::uint64_t h = static_cast<std::uint32_t>(p.x);
    h ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.y)) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    h ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.z)) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return static_cast<std::size_t>(h);
}
BlockEspModule::PositionKey BlockEspModule::keyFor(const bedrocktools::sdk::BlockPos& p) noexcept {
    return {p.x, p.y, p.z};
}
bool BlockEspModule::classify(std::string_view raw, Kind& kind) {
    // Bedrock block identifiers are canonical lowercase names. Strip the
    // namespace as a view so the hot scan loop does not allocate a string for
    // every air/stone block it inspects.
    const auto namespaceEnd = raw.find(':');
    if (namespaceEnd != std::string_view::npos) raw.remove_prefix(namespaceEnd + 1);
    const std::string_view name = raw;
    // Treat every chest variant as storage, including copper chests and any
    // future vanilla/custom names that follow the chest naming convention.
    if (name.find("shulker_box") != std::string_view::npos) kind = Kind::Shulker;
    else if (name == "ender_chest") kind = Kind::EnderChest;
    else if (name.find("chest") != std::string_view::npos) kind = Kind::Chest;
    else if (name.find("vault") != std::string_view::npos) kind = Kind::Vault;
    else if (name.find("spawner") != std::string_view::npos || name == "mob_spawner" || name == "sculk_shrieker") kind = Kind::Spawner;
    else if (name == "hopper") kind = Kind::Hopper;
    else if (name == "dispenser") kind = Kind::Dispenser;
    else if (name == "dropper") kind = Kind::Dropper;
    else if (name == "barrel") kind = Kind::Barrel;
    else if (name == "furnace" || name == "blast_furnace" || name == "smoker") kind = Kind::Furnace;
    else if (name == "crafting_table" || name == "brewing_stand" || name == "anvil" ||
             name == "chipped_anvil" || name == "damaged_anvil" || name == "smithing_table" ||
             name == "enchanting_table" || name == "loom" || name == "stonecutter" ||
             name == "cartography_table" || name == "fletching_table" || name == "grindstone" ||
             name == "composter" || name == "lectern" || name == "beacon" || name == "bell" ||
             name == "jukebox" || name == "note_block" || name == "observer" ||
             name == "piston" || name == "sticky_piston" || name == "crafter" ||
             name == "redstone_block" || name == "redstone_torch" || name == "redstone_wire" || name == "redstone_lamp" || name == "lever" || name.find("button") != std::string_view::npos ||
             name.find("pressure_plate") != std::string_view::npos || name == "target") kind = Kind::Utility;
    else return false;
    return true;
}
std::uint32_t BlockEspModule::defaultColor(Kind kind) noexcept {
    switch (kind) {
        case Kind::Shulker: return 0xFFFF40FFu;
        case Kind::Chest: return 0xFFFFB52Eu;
        case Kind::EnderChest: return 0xFFB45CFFu;
        case Kind::Hopper: return 0xFF55DDE0u;
        case Kind::Dispenser: return 0xFF65A5FFu;
        case Kind::Dropper: return 0xFF4E78D2u;
        case Kind::Barrel: return 0xFFB77A42u;
        case Kind::Furnace: return 0xFFFF6655u;
        case Kind::Utility: return 0xFF8DDF73u;
        default: return 0xFFFFFFFFu;
    }
}
bool BlockEspModule::isKindEnabled(Kind kind) const noexcept {
    switch (kind) {
        case Kind::Shulker: return showShulkers;
        case Kind::Chest: return showChests;
        case Kind::EnderChest: return showEnderChests;
        case Kind::Vault: return showVaults;
        case Kind::Spawner: return showSpawners;
        case Kind::Hopper: return showHoppers;
        case Kind::Dispenser: return showDispensers;
        case Kind::Dropper: return showDroppers;
        case Kind::Barrel: return showBarrels;
        case Kind::Furnace: return showFurnaces;
        case Kind::Utility: return true;
        default: return false;
    }
}
bool BlockEspModule::isBlockEnabled(const Outline& outline) const {
    if (outline.kind != Kind::Utility) return isKindEnabled(outline.kind);
    const auto it = m_utilityFilters.find(outline.blockName);
    return it != m_utilityFilters.end() && it->second;
}
std::uint32_t BlockEspModule::colorFor(Kind kind) const noexcept {
    switch (kind) {
        case Kind::Shulker: return shulkerColor;
        case Kind::Chest: return chestColor;
        case Kind::Vault: return 0xFFFFD34Eu;
        case Kind::Spawner: return 0xFF45E8D2u;
        case Kind::EnderChest: return enderChestColor;
        case Kind::Hopper: return hopperColor;
        case Kind::Dispenser: return dispenserColor;
        case Kind::Dropper: return dropperColor;
        case Kind::Barrel: return barrelColor;
        case Kind::Furnace: return furnaceColor;
        case Kind::Utility: return utilityColor;
        default: return defaultColor(kind);
    }
}
std::uint32_t BlockEspModule::colorForBlock(Kind kind, std::string_view name) const noexcept {
    if (kind != Kind::Utility) return colorFor(kind);
    if (name == "vault") return 0xFFFFD34Eu;
    if (name.find("spawner") != std::string::npos || name == "mob_spawner") return 0xFF45E8D2u;
    if (name == "sculk_shrieker") return 0xFF8A4DCEu;
    if (name == "redstone_block" || name == "redstone_torch" || name == "redstone_wire" || name == "redstone_lamp") return 0xFFFF3030u;
    if (name == "block_button" || name == "block_pressure_plate" ||
        name == "polished_blackstone_button" || name == "polished_blackstone_pressure_plate") return 0xFFB9B9B9u;
    if (name == "lever") return 0xFFFFD34Eu;
    if (name == "target") return 0xFFFF5252u;
    if (name == "beacon") return 0xFF6DF7F0u;
    if (name == "crafting_table") return 0xFFB77A42u;
    if (name == "brewing_stand") return 0xFF9A78D1u;
    if (name == "anvil" || name == "chipped_anvil" || name == "damaged_anvil") return 0xFF777F8Au;
    if (name == "smithing_table" || name == "stonecutter") return 0xFF9BA7B0u;
    if (name == "enchanting_table") return 0xFF8B4DCCu;
    if (name == "loom" || name == "cartography_table" || name == "fletching_table") return 0xFFD4A56Au;
    if (name == "grindstone") return 0xFFB9C3C9u;
    if (name == "composter") return 0xFF7C9A45u;
    if (name == "lectern") return 0xFFB98954u;
    if (name == "bell") return 0xFFFFD05Au;
    if (name == "jukebox" || name == "note_block") return 0xFF9A633Au;
    if (name == "observer" || name == "piston" || name == "sticky_piston" || name == "crafter") return 0xFF8E9A9Fu;
    return utilityColor;
}
void BlockEspModule::clearCache() {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    m_cache.clear();
}
void BlockEspModule::onInit() {
    resolveBlockGetBlock();
    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [this](auto& event) { onPlayerTick(event.player); });
}
void BlockEspModule::onEnable() {
    if (auto* hitbox = ModuleRegistry::get().find("bedrocktools.Hitbox"))
        static_cast<HitboxModule*>(hitbox)->ensureRenderHook();
    m_blockSource = nullptr; m_scanActive = false; m_hasCenter = false; m_scanCursor = 0; m_scanTotal = 0;
    m_priorityScanActive = false; m_hasPriorityCenter = false; m_priorityDirectionDirty = false; m_priorityScanCursor = 0;
    m_hasPreviousPlayerPosition = false; m_hasMovementDirection = false;
    clearCache();
}
void BlockEspModule::onDisable() {
    m_scanActive = false; m_hasCenter = false; m_scanCursor = 0; m_scanTotal = 0;
    m_priorityScanActive = false; m_hasPriorityCenter = false; m_priorityDirectionDirty = false; m_priorityScanCursor = 0;
    m_hasPreviousPlayerPosition = false; m_hasMovementDirection = false;
    clearCache();
}
void BlockEspModule::onPlayerTick(bedrocktools::sdk::Player* player) {
    ++m_tickCount;
    if (!enabled || !player) return;

    auto* dimension = player->dimension();
    auto* source = dimension ? dimension->blockSource() : nullptr;
    if (!source) return;
    if (source != m_blockSource) {
        m_blockSource = source;
        m_scanActive = false; m_hasCenter = false; m_scanCursor = 0; m_scanTotal = 0;
        m_priorityScanActive = false; m_hasPriorityCenter = false; m_priorityDirectionDirty = false; m_priorityScanCursor = 0;
        m_hasPreviousPlayerPosition = false; m_hasMovementDirection = false;
        clearCache();
    }
    if (!g_blockEspGetBlock.load(std::memory_order_acquire) && m_tickCount - m_lastResolveAttemptTick >= 40) {
        m_lastResolveAttemptTick = m_tickCount;
        resolveBlockGetBlock();
    }
    const auto getBlock = g_blockEspGetBlock.load(std::memory_order_acquire);
    if (!getBlock) return;

    const auto p = player->position();
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return;
    const bedrocktools::sdk::BlockPos current{
        static_cast<int>(std::floor(p.x)), static_cast<int>(std::floor(p.y)), static_cast<int>(std::floor(p.z))};

    // Infer horizontal movement from successive positions instead of relying on a
    // version-specific velocity accessor. This also works with Elytra and knockback.
    bool headingChanged = false;
    float horizontalMovement = 0.0f;
    if (m_hasPreviousPlayerPosition) {
        const float moveX = p.x - m_previousPlayerPosition.x;
        const float moveZ = p.z - m_previousPlayerPosition.z;
        horizontalMovement = std::hypot(moveX, moveZ);
        if (horizontalMovement > 0.5f) {
            const float nextX = moveX / horizontalMovement;
            const float nextZ = moveZ / horizontalMovement;
            if (m_hasMovementDirection) {
                const float directionDot = nextX * m_moveDirectionX + nextZ * m_moveDirectionZ;
                headingChanged = directionDot < 0.80f;
            }
            m_moveDirectionX = nextX;
            m_moveDirectionZ = nextZ;
            m_hasMovementDirection = true;
        }
    }
    m_previousPlayerPosition = p;
    m_hasPreviousPlayerPosition = true;
    if (headingChanged) m_priorityDirectionDirty = true;

    const int rx = maxScanRadius ? 160 : std::clamp(horizontalRadius, 20, 160);
    const int ry = maxScanRadius ? 160 : std::clamp(verticalRadius, 10, 160);
    const std::size_t scanBudget = static_cast<std::size_t>(std::clamp(scanBudgetPerTick, 256, 16384));

    // Cull cache entries that cannot be rendered from the current player position.
    // Without this, a long Elytra flight could fill the bounded cache with blocks
    // from old locations and prevent newly discovered blocks from being inserted.
    if (m_tickCount % 10u == 0u) {
        const int keepHorizontal = rx + 8;
        const int keepVertical = ry + 8;
        const int keepHorizontalSquared = keepHorizontal * keepHorizontal;
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        for (auto it = m_cache.begin(); it != m_cache.end();) {
            const std::int64_t dx = static_cast<std::int64_t>(it->second.position.x) - current.x;
            const std::int64_t dy = static_cast<std::int64_t>(it->second.position.y) - current.y;
            const std::int64_t dz = static_cast<std::int64_t>(it->second.position.z) - current.z;
            if (dx * dx + dz * dz > static_cast<std::int64_t>(keepHorizontalSquared) || std::abs(dy) > keepVertical)
                it = m_cache.erase(it);
            else
                ++it;
        }
    }

    // Each query contributes to a batched cache update. The old implementation
    // acquired the cache mutex for every queried block (often >16,000 locks/tick).
    // Batch erases/inserts once per tick instead, while keeping unloaded positions
    // untouched when BlockSource returns an invalid pointer.
    std::vector<PositionKey> scannedPositions;
    std::vector<Outline> foundOutlines;
    scannedPositions.reserve(scanBudget + 729u);
    foundOutlines.reserve(64u);
    auto inspectBlock = [&](const bedrocktools::sdk::BlockPos& pos) {
        void* raw = getBlock(source, pos);
        if (!raw || reinterpret_cast<std::uintptr_t>(raw) <= 0x1000) return;
        const auto* name = reinterpret_cast<const bedrocktools::sdk::Block*>(raw)->fullName();
        if (!name || name->empty()) return;

        const PositionKey key = keyFor(pos);
        scannedPositions.push_back(key);
        Kind kind{};
        if (!classify(*name, kind)) return;

        std::string normalizedName = stripNamespace(*name);
        if (normalizedName.size() >= 7 && normalizedName.ends_with("_button")) normalizedName = "block_button";
        if (normalizedName.ends_with("_pressure_plate")) normalizedName = "block_pressure_plate";
        foundOutlines.push_back(Outline{pos, colorForBlock(kind, normalizedName), kind, std::move(normalizedName)});
    };

    // Fast local refresh: nearby placements/removals remain responsive every
    // other tick, independent of how far the wide scan has progressed.
    constexpr int kLocalRefreshRadius = 4;
    if ((m_tickCount & 1u) == 0u) {
        for (int dx = -kLocalRefreshRadius; dx <= kLocalRefreshRadius; ++dx) {
            for (int dy = -kLocalRefreshRadius; dy <= kLocalRefreshRadius; ++dy) {
                for (int dz = -kLocalRefreshRadius; dz <= kLocalRefreshRadius; ++dz) {
                    inspectBlock(bedrocktools::sdk::BlockPos{current.x + dx, current.y + dy, current.z + dz});
                }
            }
        }
    }

    // The priority scan is a small rolling volume. When moving, it becomes a
    // forward corridor (24 blocks ahead, 5 blocks to either side and 8 vertically).
    // While stationary, it scans a center-out 17x17x17 cube. This pass is rebuilt
    // only after meaningful movement / a turn, never on each individual tick.
    bool movedFromPriorityCenter = false;
    if (m_hasPriorityCenter) {
        const int dx = current.x - m_priorityScanOrigin.x;
        const int dy = current.y - m_priorityScanOrigin.y;
        const int dz = current.z - m_priorityScanOrigin.z;
        movedFromPriorityCenter = dx * dx + dz * dz >= 25 || std::abs(dy) >= 5;
    }
    if (!m_priorityScanActive &&
        (!m_hasPriorityCenter || movedFromPriorityCenter || m_priorityDirectionDirty)) {
        m_priorityScanOrigin = current;
        m_priorityScanCursor = 0;
        m_priorityScanOffsets.clear();
        m_priorityScanOffsets.reserve(4913u);

        auto centerOutOffset = [](int index) -> int {
            if (index == 0) return 0;
            const int magnitude = (index + 1) / 2;
            return (index & 1) ? -magnitude : magnitude;
        };
        if (horizontalMovement > 0.5f && m_hasMovementDirection) {
            for (int forward = 0; forward <= 24; ++forward) {
                for (int lateralIndex = 0; lateralIndex <= 10; ++lateralIndex) {
                    const int lateral = centerOutOffset(lateralIndex);
                    for (int verticalIndex = 0; verticalIndex <= 16; ++verticalIndex) {
                        const int dy = centerOutOffset(verticalIndex);
                        const int dx = static_cast<int>(std::lround(
                            static_cast<float>(forward) * m_moveDirectionX -
                            static_cast<float>(lateral) * m_moveDirectionZ));
                        const int dz = static_cast<int>(std::lround(
                            static_cast<float>(forward) * m_moveDirectionZ +
                            static_cast<float>(lateral) * m_moveDirectionX));
                        m_priorityScanOffsets.push_back({dx, dy, dz});
                    }
                }
            }
        } else {
            for (int shell = 0; shell <= 8; ++shell) {
                for (int dx = -shell; dx <= shell; ++dx) {
                    for (int dy = -shell; dy <= shell; ++dy) {
                        for (int dz = -shell; dz <= shell; ++dz) {
                            if (std::max({std::abs(dx), std::abs(dy), std::abs(dz)}) != shell) continue;
                            m_priorityScanOffsets.push_back({dx, dy, dz});
                        }
                    }
                }
            }
        }
        m_priorityScanActive = !m_priorityScanOffsets.empty();
        m_hasPriorityCenter = true;
        m_priorityDirectionDirty = false;
    }

    // Start/recenter the wide background scan only when no wide pass is running.
    // Its cursor therefore keeps progressing during flight instead of restarting
    // every time the player crosses another block or chunk.
    constexpr int moveThreshold = 32;
    const bool movedFar = std::abs(current.x - m_lastCenter.x) >= moveThreshold ||
        std::abs(current.z - m_lastCenter.z) >= moveThreshold ||
        std::abs(current.y - m_lastCenter.y) >= moveThreshold;
    if (!m_hasCenter || (!m_scanActive && movedFar)) {
        m_scanCenter = current;
        m_lastCenter = current;
        m_hasCenter = true;
        m_scanCursor = 0;

        const std::size_t diameter = static_cast<std::size_t>(rx * 2 + 1);
        m_horizontalScanOffsets.clear();
        m_horizontalScanOffsets.reserve(diameter * diameter);
        m_horizontalScanOffsets.emplace_back(0, 0);
        // Precompute center-out rings once per background pass.
        for (int ring = 1; ring <= rx; ++ring) {
            for (int dx = -ring; dx <= ring; ++dx) {
                m_horizontalScanOffsets.emplace_back(dx, -ring);
                m_horizontalScanOffsets.emplace_back(dx, ring);
            }
            for (int dz = -ring + 1; dz < ring; ++dz) {
                m_horizontalScanOffsets.emplace_back(-ring, dz);
                m_horizontalScanOffsets.emplace_back(ring, dz);
            }
        }
        m_scanTotal = m_horizontalScanOffsets.size() * static_cast<std::size_t>(ry * 2 + 1);
        m_scanActive = true;
    }

    // Spend roughly one third of the configured query budget on the priority
    // volume (enough to finish it in a single tick at the default budget) and
    // the remainder on the broad scan. Low user-set budgets remain split fairly.
    const std::size_t priorityBudget = scanBudget >= 2048u ? scanBudget / 3u : scanBudget / 2u;
    std::size_t priorityUsed = 0;
    if (m_priorityScanActive) {
        const std::size_t end = std::min(m_priorityScanOffsets.size(), m_priorityScanCursor + priorityBudget);
        for (std::size_t index = m_priorityScanCursor; index < end; ++index) {
            const auto& offset = m_priorityScanOffsets[index];
            inspectBlock(bedrocktools::sdk::BlockPos{
                m_priorityScanOrigin.x + offset.x,
                m_priorityScanOrigin.y + offset.y,
                m_priorityScanOrigin.z + offset.z});
        }
        priorityUsed = end - m_priorityScanCursor;
        m_priorityScanCursor = end;
        if (m_priorityScanCursor >= m_priorityScanOffsets.size()) m_priorityScanActive = false;
    }

    const std::size_t wideBudget = scanBudget - priorityUsed;
    if (m_scanActive && wideBudget > 0u) {
        const std::size_t verticalCount = static_cast<std::size_t>(ry * 2 + 1);
        const std::size_t end = std::min(m_scanTotal, m_scanCursor + wideBudget);
        auto verticalOffset = [](std::size_t layer) -> int {
            if (layer == 0) return 0;
            const int magnitude = static_cast<int>((layer + 1) / 2);
            return (layer & 1u) ? -magnitude : magnitude;
        };
        for (std::size_t index = m_scanCursor; index < end; ++index) {
            const std::size_t horizontalIndex = index / verticalCount;
            const std::size_t layer = index % verticalCount;
            const auto [dx, dz] = m_horizontalScanOffsets[horizontalIndex];
            const int dy = verticalOffset(layer);
            inspectBlock(bedrocktools::sdk::BlockPos{
                m_scanCenter.x + dx, m_scanCenter.y + dy, m_scanCenter.z + dz});
        }
        m_scanCursor = end;
        if (m_scanCursor >= m_scanTotal) m_scanActive = false;
    }

    if (!scannedPositions.empty() || !foundOutlines.empty()) {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        for (const auto& key : scannedPositions) m_cache.erase(key);
        for (auto& outline : foundOutlines) {
            if (m_cache.size() >= kMaxBlockEspCache) break;
            m_cache.emplace(keyFor(outline.position), std::move(outline));
        }
    }
}
void BlockEspModule::getRenderSnapshot(const bedrocktools::sdk::Vec3& playerPosition,
                                       std::vector<Outline>& output) const {
    output.clear();
    if (!enabled || !masterEnabled || !keybindActive) return;
    const int rx = maxScanRadius ? 160 : std::clamp(horizontalRadius, 20, 160);
    const int ry = maxScanRadius ? 160 : std::clamp(verticalRadius, 10, 160);
    output.reserve(kMaxBlockEspRendered);

    struct Candidate {
        float distanceSquared = 0.0f;
        const Outline* outline = nullptr;
    };
    std::vector<Candidate> nearest;
    nearest.reserve(kMaxBlockEspRendered);
    // Heap top is the farthest retained block. This keeps selection O(N log K)
    // and ensures new nearby blocks are not hidden by arbitrary hash-map order.
    auto fartherHeap = [](const Candidate& a, const Candidate& b) {
        return a.distanceSquared < b.distanceSquared;
    };

    std::lock_guard<std::mutex> lock(m_cacheMutex);
    for (const auto& [key, outline] : m_cache) {
        (void)key;
        if (!isBlockEnabled(outline)) continue;
        const float dx = static_cast<float>(outline.position.x) + 0.5f - playerPosition.x;
        const float dy = static_cast<float>(outline.position.y) + 0.5f - playerPosition.y;
        const float dz = static_cast<float>(outline.position.z) + 0.5f - playerPosition.z;
        const float horizontalDistanceSquared = dx * dx + dz * dz;
        if (horizontalDistanceSquared > static_cast<float>(rx * rx) || std::fabs(dy) > static_cast<float>(ry)) continue;

        const Candidate candidate{horizontalDistanceSquared + dy * dy, &outline};
        if (nearest.size() < kMaxBlockEspRendered) {
            nearest.push_back(candidate);
            std::push_heap(nearest.begin(), nearest.end(), fartherHeap);
        } else if (candidate.distanceSquared < nearest.front().distanceSquared) {
            std::pop_heap(nearest.begin(), nearest.end(), fartherHeap);
            nearest.back() = candidate;
            std::push_heap(nearest.begin(), nearest.end(), fartherHeap);
        }
    }

    std::sort(nearest.begin(), nearest.end(), [](const Candidate& a, const Candidate& b) {
        return a.distanceSquared < b.distanceSquared;
    });
    for (const auto& candidate : nearest) {
        const auto& outline = *candidate.outline;
        output.push_back(Outline{
            outline.position, colorForBlock(outline.kind, outline.blockName), outline.kind, outline.blockName});
    }
}
void BlockEspModule::onMenuRegistered() {
    using namespace pl::modmenu;
    ConfigSchemaBuilder schema; nlohmann::json config; saveConfig(config);
    schema.defaultCategory("types")
        .category("types", "Block categories", "Toggle which useful block types are detected.")
        .category("tracer", "Tracer and outlines", "World-space outlines and lines pointing to detected blocks.")
        .category("scan", "Detection range", "Block scans are incremental to limit frame and tick spikes.")
        .category("colors", "Block colors", "HEX colors in #AARRGGBB or #RRGGBB format.");
    auto toggle = [&](const char* key, const char* title, const char* desc, bool def, const char* cat) {
        ConfigNodeV2 n; n.id=key; n.key=key; n.title=title; n.description=desc; n.category=cat;
        n.type=ConfigControlTypeV2::Toggle; n.defaultValue=def?"true":"false";
        n.currentValue=configText(config,key,n.defaultValue.c_str()); n.hasCurrentValue=true; schema.node(std::move(n));
    };
    toggle("showShulkers","Shulker boxes","All colored shulker boxes.",true,"types");
    toggle("showChests","Chests","Normal and trapped chests.",true,"types");
    toggle("showEnderChests","Ender chests","Ender chests.",true,"types");
    toggle("showVaults","Vaults","Vault blocks, including trial vaults.",true,"types");
    toggle("showSpawners","Spawners","Mob spawners, trial spawners and sculk shriekers.",true,"types");
    toggle("showHoppers","Hoppers","Hoppers.",true,"types");
    toggle("showDispensers","Dispensers","Dispensers.",true,"types");
    toggle("showDroppers","Droppers","Droppers.",true,"types");
    toggle("showBarrels","Barrels","Barrels.",true,"types");
    toggle("showFurnaces","Furnaces","Furnaces, blast furnaces and smokers.",true,"types");
    for (const auto& name : utilityBlockNames()) {
        const std::string key = "showBlock_" + name;
        const std::string title = utilityTitle(name);
        toggle(key.c_str(), title.c_str(), "Independently detect this block type.", true, "types");
    }
    toggle("showTracer","Show tracer","Draw a line from the camera to each detected block.",true,"tracer");
    auto slider = [&](const char* key,const char* title,const char* desc,const char* cat,int def,int min,int max,int step,const char* unit) {
        ConfigNodeV2 n; n.id=key; n.key=key; n.title=title; n.description=desc; n.category=cat;
        n.type=ConfigControlTypeV2::SliderInt; n.defaultValue=std::to_string(def);
        n.currentValue=configText(config,key,n.defaultValue.c_str()); n.hasCurrentValue=true;
        n.minValue=std::to_string(min); n.maxValue=std::to_string(max); n.step=std::to_string(step); n.unit=unit; schema.node(std::move(n));
    };
    slider("horizontalRadius","Horizontal radius","Horizontal scan and render radius (5x default).", "scan",80,20,160,1," blocks");
    slider("verticalRadius","Vertical radius","Vertical scan and render radius for deep underground blocks.", "scan",80,10,160,1," blocks");
    toggle("maxScanRadius","Max Scan Radius","Use the maximum supported horizontal and vertical scan radius (160 blocks each direction).",false,"scan");
    slider("scanBudgetPerTick","Scan budget","Block queries per tick; higher values load faster but use more CPU.", "scan",16384,256,16384,256," blocks/tick");
    slider("outlineThickness","Outline thickness","Approximate thickness of block outlines.", "tracer",4,1,10,1," units");
    slider("tracerThickness","Tracer thickness","World-space thickness of guide lines pointing to each block.", "tracer",4,1,10,1," units");
    auto color = [&](const char* key,const char* title,std::uint32_t def) {
        ConfigNodeV2 n; n.id=key; n.key=key; n.title=title; n.description="HEX color: #AARRGGBB (or #RRGGBB).";
        n.category="colors"; n.type=ConfigControlTypeV2::Text; n.defaultValue="";
        n.currentValue=configText(config,key,""); n.hasCurrentValue=true; n.placeholder="#AARRGGBB"; n.maxLength=9; schema.node(std::move(n));
        (void)def;
    };
    color("shulkerColor","Shulker box color",shulkerColor);
    color("chestColor","Chest color",chestColor);
    color("enderChestColor","Ender chest color",enderChestColor);
    color("hopperColor","Hopper color",hopperColor);
    color("dispenserColor","Dispenser color",dispenserColor);
    color("dropperColor","Dropper color",dropperColor);
    color("barrelColor","Barrel color",barrelColor);
    color("furnaceColor","Furnace color",furnaceColor);
    color("utilityColor","Utility block color",utilityColor);
    setConfigSchemaJson(moduleId,schema.toJson());
}
bool BlockEspModule::onMenuConfigChanged(std::string_view key, std::string_view value) {
    bool b=false;
    constexpr std::string_view utilityPrefix = "showBlock_";
    if (key.size() > utilityPrefix.size() && key.substr(0, utilityPrefix.size()) == utilityPrefix) {
        if (!parseBool(value, b)) return false;
        const std::string blockName(key.substr(utilityPrefix.size()));
        {
            std::lock_guard<std::mutex> lock(m_cacheMutex);
            auto it = m_utilityFilters.find(blockName);
            if (it == m_utilityFilters.end()) return false;
            it->second = b;
        }
        onMenuRegistered();
        return true;
    }
    if (key=="showTracer"||key=="showShulkers"||key=="showChests"||key=="showEnderChests"||key=="showVaults"||key=="showSpawners"||
        key=="showHoppers"||key=="showDispensers"||key=="showDroppers"||key=="showBarrels"||
        key=="showFurnaces"||key=="showUtilityBlocks") {
        if (!parseBool(value,b)) return false;
        if(key=="showTracer")showTracer=b; else if(key=="showShulkers")showShulkers=b;
        else if(key=="showChests")showChests=b; else if(key=="showEnderChests")showEnderChests=b;
        else if(key=="showVaults")showVaults=b; else if(key=="showSpawners")showSpawners=b;
        else if(key=="showHoppers")showHoppers=b; else if(key=="showDispensers")showDispensers=b;
        else if(key=="showDroppers")showDroppers=b; else if(key=="showBarrels")showBarrels=b;
        else if(key=="showFurnaces")showFurnaces=b; else showUtilityBlocks=b;
        onMenuRegistered(); return true;
    }
    int i=0;
    if(key=="maxScanRadius") { if(!parseBool(value,b)) return false; maxScanRadius=b; m_scanActive=false; m_hasCenter=false; onMenuRegistered(); return true; }
    if(key=="horizontalRadius"||key=="verticalRadius"||key=="scanBudgetPerTick"||key=="outlineThickness"||key=="tracerThickness") {
        if(!parseInt(value,i)) return false;
        if(key=="horizontalRadius")horizontalRadius=std::clamp(i,20,160);
        else if(key=="verticalRadius")verticalRadius=std::clamp(i,10,160);
        else if(key=="scanBudgetPerTick")scanBudgetPerTick=std::clamp(i,256,16384);
        else if(key=="outlineThickness")outlineThickness=static_cast<float>(std::clamp(i,1,10))/100.0f;
        else tracerThickness=static_cast<float>(std::clamp(i,1,10))/100.0f;
        m_scanActive=false; m_hasCenter=false; onMenuRegistered(); return true;
    }
    std::uint32_t c=0; if(!parseColor(value,c)) return false;
    if(key=="shulkerColor")shulkerColor=c; else if(key=="chestColor")chestColor=c;
    else if(key=="enderChestColor")enderChestColor=c; else if(key=="hopperColor")hopperColor=c;
    else if(key=="dispenserColor")dispenserColor=c; else if(key=="dropperColor")dropperColor=c;
    else if(key=="barrelColor")barrelColor=c; else if(key=="furnaceColor")furnaceColor=c;
    else if(key=="utilityColor")utilityColor=c;
    else return false;
    onMenuRegistered(); return true;
}
void BlockEspModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    showTracer=j.value("showTracer",showTracer); showShulkers=j.value("showShulkers",showShulkers);
    showChests=j.value("showChests",showChests); showEnderChests=j.value("showEnderChests",showEnderChests); showVaults=j.value("showVaults",showVaults); showSpawners=j.value("showSpawners",showSpawners);
    showHoppers=j.value("showHoppers",showHoppers); showDispensers=j.value("showDispensers",showDispensers);
    showDroppers=j.value("showDroppers",showDroppers); showBarrels=j.value("showBarrels",showBarrels);
    showFurnaces=j.value("showFurnaces",showFurnaces); showUtilityBlocks=j.value("showUtilityBlocks",showUtilityBlocks);
    maxScanRadius=j.value("maxScanRadius",maxScanRadius);
    for (const auto& name : utilityBlockNames()) {
        const std::string key = "showBlock_" + name;
        auto it = m_utilityFilters.find(name);
        if (it != m_utilityFilters.end()) it->second = j.value(key, showUtilityBlocks);
    }
    horizontalRadius=std::clamp(j.value("horizontalRadius",horizontalRadius),20,160);
    verticalRadius=std::clamp(j.value("verticalRadius",verticalRadius),10,160);
    scanBudgetPerTick=std::clamp(j.value("scanBudgetPerTick",scanBudgetPerTick),256,16384);
    outlineThickness=static_cast<float>(std::clamp(j.value("outlineThickness",4),1,10))/100.0f;
    tracerThickness=static_cast<float>(std::clamp(j.value("tracerThickness",4),1,10))/100.0f;
    loadColor(j,"shulkerColor",shulkerColor); loadColor(j,"chestColor",chestColor);
    loadColor(j,"enderChestColor",enderChestColor); loadColor(j,"hopperColor",hopperColor);
    loadColor(j,"dispenserColor",dispenserColor); loadColor(j,"dropperColor",dropperColor);
    loadColor(j,"barrelColor",barrelColor); loadColor(j,"furnaceColor",furnaceColor);
    loadColor(j,"utilityColor",utilityColor);
}
void BlockEspModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["showTracer"]=showTracer; j["showShulkers"]=showShulkers; j["showChests"]=showChests;
    j["showEnderChests"]=showEnderChests; j["showVaults"]=showVaults; j["showSpawners"]=showSpawners; j["showHoppers"]=showHoppers; j["showDispensers"]=showDispensers;
    j["showDroppers"]=showDroppers; j["showBarrels"]=showBarrels; j["showFurnaces"]=showFurnaces;
    j["showUtilityBlocks"]=showUtilityBlocks; j["maxScanRadius"]=maxScanRadius;
    for (const auto& [name, enabled] : m_utilityFilters) j["showBlock_" + name] = enabled;
    j["horizontalRadius"]=horizontalRadius;
    j["verticalRadius"]=verticalRadius; j["scanBudgetPerTick"]=scanBudgetPerTick;
    j["outlineThickness"]=static_cast<int>(std::lround(outlineThickness*100.0f));
    j["tracerThickness"]=static_cast<int>(std::lround(tracerThickness*100.0f));
    auto save=[&](const char* key,std::uint32_t c){char text[12];std::snprintf(text,sizeof(text),"#%08X",c);j[key]=std::string(text);};
    save("shulkerColor",shulkerColor); save("chestColor",chestColor); save("enderChestColor",enderChestColor);
    save("hopperColor",hopperColor); save("dispenserColor",dispenserColor); save("dropperColor",dropperColor);
    save("barrelColor",barrelColor); save("furnaceColor",furnaceColor); save("utilityColor",utilityColor);
}
