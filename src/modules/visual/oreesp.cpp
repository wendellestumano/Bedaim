#include "oreesp.hpp"

#include "../ModuleRegistry.hpp"
#include "../../core/Diagnostics.hpp"
#include "hitbox.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/offsets/Render.hpp>
#include <bedrocktools/sdk/offsets/World.hpp>
#include <bedrocktools/sdk/render/Block.hpp>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cctype>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace {
using BlockSourceGetBlockFn = void* (*)(void*, const bedrocktools::sdk::BlockPos&);
std::atomic<BlockSourceGetBlockFn> g_getBlock{nullptr};
constexpr std::size_t kMaxCachedOres = 8192;
constexpr std::size_t kMaxPendingBreaks = 16;
constexpr std::size_t kMaxQueuedScans = 8;
constexpr std::size_t kMaxRenderedOres = 768;

bool parseBool(std::string_view value, bool& result) {
    if (value == "true" || value == "1" || value == "on") { result = true; return true; }
    if (value == "false" || value == "0" || value == "off") { result = false; return true; }
    return false;
}

bool parseInteger(std::string_view value, int& result) {
    try {
        const std::string text(value);
        std::size_t consumed = 0;
        const int parsed = std::stoi(text, &consumed);
        while (consumed < text.size() && std::isspace(static_cast<unsigned char>(text[consumed]))) ++consumed;
        if (consumed != text.size()) return false;
        result = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

std::string configText(const nlohmann::json& json, const char* key, const char* fallback) {
    const auto it = json.find(key);
    if (it == json.end() || it->is_null()) return fallback;
    if (it->is_string()) return it->get<std::string>();
    if (it->is_boolean()) return it->get<bool>() ? "true" : "false";
    return it->dump();
}

std::uint64_t hashName(std::string_view value) noexcept {
    std::uint64_t hash = 14695981039346656037ull;
    for (const unsigned char ch : value) {
        hash ^= static_cast<std::uint64_t>(ch);
        hash *= 1099511628211ull;
    }
    return hash;
}

bool isAirName(std::string_view name) noexcept {
    name = bedaim::xray::stripNamespace(name);
    return name == "air" || name == "cave_air" || name == "void_air";
}

bool sameNeighborhood(const bedrocktools::sdk::BlockPos& a,
                      const bedrocktools::sdk::BlockPos& b) noexcept {
    return std::abs(a.x - b.x) <= 4 &&
           std::abs(a.y - b.y) <= 3 &&
           std::abs(a.z - b.z) <= 4;
}

void resolveGetBlock() {
    if (g_getBlock.load(std::memory_order_acquire)) return;
    const auto address = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::BlockSourceGetBlock);
    if (address) {
        g_getBlock.store(reinterpret_cast<BlockSourceGetBlockFn>(address), std::memory_order_release);
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Info, "Ore ESP",
            "block_query", "resolved", "BlockSourceGetBlock signature resolved; scanning enabled");
    } else {
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Warning, "Ore ESP",
            "block_query", "unresolved", "BlockSourceGetBlock signature did not match this Minecraft build; Ore ESP cannot scan yet");
    }
}
}

OreEspModule::OreEspModule()
    : Module("Ore ESP", "Cached colored ore outlines with budgeted initial scans and local refreshes after block changes.") {
    showInMenu = true;
}

std::size_t OreEspModule::PositionHash::operator()(const PositionKey& p) const noexcept {
    std::uint64_t h = static_cast<std::uint32_t>(p.x);
    h = (h ^ (static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.y)) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2)));
    h = (h ^ (static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.z)) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2)));
    return static_cast<std::size_t>(h);
}

OreEspModule::PositionKey OreEspModule::keyFor(const bedrocktools::sdk::BlockPos& pos) noexcept {
    return PositionKey{pos.x, pos.y, pos.z};
}

void OreEspModule::onInit() {
    resolveGetBlock();
    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [this](auto& event) { onPlayerTick(event.player); });
    bedrocktools::events::bus().subscribe<bedrocktools::events::GameModeActionEvent>(
        [this](auto& event) { onGameModeAction(event); });
    bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Info, "Ore ESP",
        "initialized; will run a budgeted initial scan and refresh cached regions after block changes or movement");
}

void OreEspModule::onEnable() {
    // Enabling is cheap; the initial world scan is queued from the next player tick.
    m_pendingBreaks.clear();
    m_scanQueue.clear();
    m_scanActive = false;
    m_scanCursor = 0;
    m_scanTotal = 0;
    m_hasScannedAtLeastOnce = false;
    m_hasAutoScanCenter = false;
    m_lastAutoScanCenter = {};
    m_rescanRequested = false;
    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        m_oreCache.clear();
    }
    if (auto* hitbox = ModuleRegistry::get().find("bedrocktools.Hitbox")) {
        static_cast<class HitboxModule*>(hitbox)->ensureRenderHook();
    }
    bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Info, "Ore ESP",
        "enabled; starting a bounded initial scan, with local refreshes after block changes and movement");
}

void OreEspModule::onDisable() {
    m_pendingBreaks.clear();
    m_scanQueue.clear();
    m_scanActive = false;
    m_hasScannedAtLeastOnce = false;
    m_hasAutoScanCenter = false;
    m_lastAutoScanCenter = {};
    m_rescanRequested = false;
    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        m_oreCache.clear();
    }
    bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Info, "Ore ESP",
        "disabled; cleared cached ore outlines");
}

void OreEspModule::clearWorldState() {
    m_pendingBreaks.clear();
    m_scanQueue.clear();
    m_scanActive = false;
    m_scanCursor = 0;
    m_scanTotal = 0;
    m_hasScannedAtLeastOnce = false;
    m_hasAutoScanCenter = false;
    m_lastAutoScanCenter = {};
    m_rescanRequested = false;
    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        m_oreCache.clear();
    }
}

OreEspModule::BlockIdentity OreEspModule::inspectBlock(
    bedrocktools::sdk::BlockSource* source,
    const bedrocktools::sdk::BlockPos& pos) const {
    BlockIdentity identity{};
    if (!source) return identity;
    const auto getBlock = g_getBlock.load(std::memory_order_acquire);
    if (!getBlock) return identity;
    void* rawBlock = getBlock(source, pos);
    if (!rawBlock || reinterpret_cast<std::uintptr_t>(rawBlock) <= 0x1000) return identity;

    const auto* name = reinterpret_cast<const bedrocktools::sdk::Block*>(rawBlock)->fullName();
    if (!name || name->empty()) return identity;

    const std::string_view view(*name);
    identity.valid = true;
    identity.nameHash = hashName(view);
    identity.air = isAirName(view);
    identity.kind = bedaim::xray::kindForBlockName(view);
    return identity;
}

bool OreEspModule::isKindEnabled(bedaim::xray::OreKind kind) const noexcept {
    using bedaim::xray::OreKind;
    switch (kind) {
        case OreKind::Diamond: return showDiamonds;
        case OreKind::Emerald: return showEmeralds;
        case OreKind::Gold: return showGold;
        case OreKind::Iron: return showIron;
        case OreKind::Copper: return showCopper;
        case OreKind::Redstone: return showRedstone;
        case OreKind::Lapis: return showLapis;
        case OreKind::Quartz: return showQuartz;
        case OreKind::Coal: return showCoal;
        case OreKind::AncientDebris: return showAncientDebris;
        case OreKind::Other: return showOtherOres;
        case OreKind::None: return false;
    }
    return false;
}

void OreEspModule::onGameModeAction(bedrocktools::events::GameModeActionEvent& event) {
    using bedrocktools::events::GameModeAction;
    if (!enabled || event.action != GameModeAction::StartDestroyBlock || !event.hasBlockPosition) return;
    const auto pos = event.blockPosition;
    if (m_pendingBreaks.size() >= kMaxPendingBreaks) m_pendingBreaks.erase(m_pendingBreaks.begin());

    PendingBreak pending{};
    pending.position = pos;
    if (m_blockSource) {
        const auto before = inspectBlock(m_blockSource, pos);
        if (before.valid) {
            if (before.air) {
                // A successful native start that already sees air is the fast-break case.
                if (event.hasNativeResult && event.nativeResult) {
                    queueScan(pos);
                    eraseCachedPosition(pos);
                }
                return;
            }
            pending.initialNameHash = before.nameHash;
            pending.hasBaseline = true;
        }
    }

    for (auto& existing : m_pendingBreaks) {
        if (existing.position.x == pos.x && existing.position.y == pos.y && existing.position.z == pos.z) {
            if (pending.hasBaseline) {
                existing.initialNameHash = pending.initialNameHash;
                existing.hasBaseline = true;
            }
            existing.ageTicks = 0;
            return;
        }
    }
    m_pendingBreaks.push_back(pending);
}

void OreEspModule::onPlayerTick(bedrocktools::sdk::Player* player) {
    ++m_tickCount;
    if (!player) return;
    auto* dimension = player->dimension();
    auto* source = dimension ? dimension->blockSource() : nullptr;
    if (source != m_blockSource) {
        m_blockSource = source;
        clearWorldState();
    }
    if (!enabled || !source) return;

    if (!g_getBlock.load(std::memory_order_acquire) &&
        m_tickCount - m_lastResolveAttemptTick >= 40) {
        m_lastResolveAttemptTick = m_tickCount;
        resolveGetBlock();
    }
    if (!g_getBlock.load(std::memory_order_acquire)) return;

    if (m_rescanRequested) {
        // Radius changes invalidate the active region; restart cleanly around the
        // player instead of letting duplicate-neighborhood coalescing discard it.
        m_scanQueue.clear();
        m_scanActive = false;
        m_scanCursor = 0;
        m_scanTotal = 0;
        const auto position = player->position();
        if (std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z)) {
            queueScan({static_cast<int>(std::floor(position.x)),
                       static_cast<int>(std::floor(position.y)),
                       static_cast<int>(std::floor(position.z))});
        }
        if (std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z)) {
            m_lastAutoScanCenter = {static_cast<int>(std::floor(position.x)),
                                    static_cast<int>(std::floor(position.y)),
                                    static_cast<int>(std::floor(position.z))};
            m_hasAutoScanCenter = true;
        }
        m_rescanRequested = false;
    }

    // Seed the cache on first enable/world entry. After that, refresh around the
    // player only after meaningful movement and only when no scan is already queued
    // or running. Mining still queues a smaller local refresh immediately.
    const auto playerPosition = player->position();
    if (std::isfinite(playerPosition.x) && std::isfinite(playerPosition.y) &&
        std::isfinite(playerPosition.z)) {
        const bedrocktools::sdk::BlockPos playerBlock{
            static_cast<int>(std::floor(playerPosition.x)),
            static_cast<int>(std::floor(playerPosition.y)),
            static_cast<int>(std::floor(playerPosition.z))};
        if (!m_hasAutoScanCenter) {
            queueScan(playerBlock);
            m_lastAutoScanCenter = playerBlock;
            m_hasAutoScanCenter = true;
        } else if (!m_scanActive && m_scanQueue.empty()) {
            const int dx = playerBlock.x - m_lastAutoScanCenter.x;
            const int dy = playerBlock.y - m_lastAutoScanCenter.y;
            const int dz = playerBlock.z - m_lastAutoScanCenter.z;
            const int horizontalStep = std::max(4, std::clamp(horizontalRadius, 4, 48) / 2);
            const int verticalStep = std::max(2, std::clamp(verticalRadius, 2, 24) / 2);
            if (dx * dx + dz * dz >= horizontalStep * horizontalStep ||
                std::abs(dy) >= verticalStep) {
                queueScan(playerBlock);
                m_lastAutoScanCenter = playerBlock;
            }
        }
    }

    for (auto it = m_pendingBreaks.begin(); it != m_pendingBreaks.end();) {
        ++it->ageTicks;
        const auto now = inspectBlock(source, it->position);
        bool changed = false;
        if (now.valid) {
            if (!it->hasBaseline) {
                if (now.air) {
                    changed = true;
                } else {
                    it->initialNameHash = now.nameHash;
                    it->hasBaseline = true;
                }
            } else {
                changed = now.nameHash != it->initialNameHash;
            }
        }
        if (changed) {
            queueScan(it->position);
            eraseCachedPosition(it->position);
            it = m_pendingBreaks.erase(it);
        } else if (it->ageTicks > 300) {
            it = m_pendingBreaks.erase(it);
        } else {
            ++it;
        }
    }

    processScanBatch(source);
}

void OreEspModule::queueScan(const bedrocktools::sdk::BlockPos& center) {
    if (!enabled) return;
    if (m_scanActive && sameNeighborhood(center, m_scanCenter)) return;
    for (const auto& queued : m_scanQueue) {
        if (sameNeighborhood(center, queued.center)) return;
    }
    if (m_scanQueue.size() >= kMaxQueuedScans) m_scanQueue.pop_front();
    m_scanQueue.push_back(ScanJob{center});
}

void OreEspModule::startNextScan() {
    if (m_scanActive || m_scanQueue.empty()) return;
    m_scanCenter = m_scanQueue.front().center;
    m_scanQueue.pop_front();
    m_scanRadiusHorizontal = std::clamp(horizontalRadius, 4, 48);
    m_scanRadiusVertical = std::clamp(verticalRadius, 2, 24);
    const std::size_t diameter = static_cast<std::size_t>(2 * m_scanRadiusHorizontal + 1);
    const std::size_t height = static_cast<std::size_t>(2 * m_scanRadiusVertical + 1);
    m_scanTotal = diameter * diameter * height;
    m_scanCursor = 0;
    m_scanActive = true;

    const int rx = m_scanRadiusHorizontal;
    const int ry = m_scanRadiusVertical;
    const int keepHorizontal = rx * 2;
    const int keepVertical = ry * 2;
    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        // Retain overlapping cached regions while scanning so movement-triggered
        // refreshes do not make already-known ores disappear for several seconds.
        // Prune distant regions to keep the bounded cache available for new chunks.
        for (auto it = m_oreCache.begin(); it != m_oreCache.end();) {
            const auto& p = it->second.position;
            const bool outsideRetention = std::abs(p.x - m_scanCenter.x) > keepHorizontal ||
                                          std::abs(p.y - m_scanCenter.y) > keepVertical ||
                                          std::abs(p.z - m_scanCenter.z) > keepHorizontal;
            if (outsideRetention) it = m_oreCache.erase(it);
            else ++it;
        }
    }
    bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Debug, "Ore ESP",
        "scan_started; positions=" + std::to_string(m_scanTotal) +
        "; budget_per_tick=" + std::to_string(std::clamp(scanBudgetPerTick, 64, 2048)));
}

void OreEspModule::processScanBatch(bedrocktools::sdk::BlockSource* source) {
    startNextScan();
    if (!m_scanActive || !source) return;

    const int rx = m_scanRadiusHorizontal;
    const int ry = m_scanRadiusVertical;
    const std::size_t diameter = static_cast<std::size_t>(2 * rx + 1);
    const std::size_t plane = diameter * diameter;
    const std::size_t budget = static_cast<std::size_t>(std::clamp(scanBudgetPerTick, 64, 1024));
    const std::size_t end = std::min(m_scanTotal, m_scanCursor + budget);
    std::vector<Outline> found;
    found.reserve(std::min<std::size_t>(budget / 8 + 1, 96));
    std::vector<PositionKey> scannedPositions;
    scannedPositions.reserve(end - m_scanCursor);

    for (std::size_t index = m_scanCursor; index < end; ++index) {
        const int dx = static_cast<int>(index % diameter) - rx;
        const int dz = static_cast<int>((index / diameter) % diameter) - rx;
        const int dy = static_cast<int>(index / plane) - ry;
        const bedrocktools::sdk::BlockPos pos{
            m_scanCenter.x + dx, m_scanCenter.y + dy, m_scanCenter.z + dz
        };
        scannedPositions.push_back(keyFor(pos));
        const auto identity = inspectBlock(source, pos);
        if (!identity.valid || identity.kind == bedaim::xray::OreKind::None) continue;
        found.push_back(Outline{pos, bedaim::xray::colorForOreKind(identity.kind), identity.kind});
    }
    m_scanCursor = end;

    if (!scannedPositions.empty() || !found.empty()) {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        // Reconcile only positions visited in this batch. This removes stale ore
        // entries incrementally without blanking the whole overlapping region.
        for (const auto& key : scannedPositions) m_oreCache.erase(key);
        for (const auto& outline : found) {
            const auto key = keyFor(outline.position);
            if (m_oreCache.size() >= kMaxCachedOres && m_oreCache.find(key) == m_oreCache.end()) continue;
            m_oreCache[key] = outline;
        }
    }

    if (m_scanCursor >= m_scanTotal) {
        m_scanActive = false;
        m_hasScannedAtLeastOnce = true;
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Info, "Ore ESP",
            "scan_finished; cached_ore_blocks=" + std::to_string(m_oreCache.size()));
    }
}

void OreEspModule::eraseCachedPosition(const bedrocktools::sdk::BlockPos& pos) {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    m_oreCache.erase(keyFor(pos));
}

void OreEspModule::getRenderSnapshot(const bedrocktools::sdk::Vec3& playerPosition,
                                     std::vector<Outline>& output) const {
    output.clear();
    // Defensive render gate: never expose cached ore geometry if the module's
    // effective state is off, even if a stale cache or hook survives a toggle.
    if (!enabled || !masterEnabled || !keybindActive) return;
    constexpr std::size_t maxRenderedOres = 768;
    output.reserve(maxRenderedOres);
    const int rx = std::clamp(horizontalRadius, 4, 48);
    const int ry = std::clamp(verticalRadius, 2, 24);
    const float maxHorizontalSq = static_cast<float>(rx * rx);
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    for (const auto& [key, outline] : m_oreCache) {
        (void)key;
        if (!isKindEnabled(outline.kind)) continue;
        const float dx = static_cast<float>(outline.position.x) + 0.5f - playerPosition.x;
        const float dz = static_cast<float>(outline.position.z) + 0.5f - playerPosition.z;
        const float dy = static_cast<float>(outline.position.y) + 0.5f - playerPosition.y;
        if (dx * dx + dz * dz > maxHorizontalSq || std::fabs(dy) > static_cast<float>(ry)) continue;
        output.push_back(outline);
        if (output.size() >= maxRenderedOres) break;
    }
}

void OreEspModule::onMenuRegistered() {
    using namespace pl::modmenu;
    ConfigSchemaBuilder schema;
    nlohmann::json config;
    saveConfig(config);
    schema.defaultCategory("ores")
        .category("ores", "Ore types", "Select which ore outlines are visible.")
        .category("scan", "Scan limits", "An initial scan is spread across ticks. Later scans are triggered by confirmed block changes, meaningful movement, or radius changes.")
        .category("render", "Outline rendering", "Adjust the visual thickness of ore outlines.");

    auto toggle = [&](const char* key, const char* title, const char* description, bool defaultValue) {
        ConfigNodeV2 node;
        node.id = key; node.key = key; node.title = title; node.description = description;
        node.category = "ores"; node.type = ConfigControlTypeV2::Toggle;
        node.defaultValue = defaultValue ? "true" : "false";
        node.currentValue = configText(config, key, node.defaultValue.c_str());
        node.hasCurrentValue = true;
        schema.node(std::move(node));
    };
    toggle("showDiamonds", "Diamonds", "Diamond and deepslate diamond ore.", true);
    toggle("showEmeralds", "Emeralds", "Emerald ore.", true);
    toggle("showGold", "Gold", "Gold and nether gold ore.", true);
    toggle("showIron", "Iron", "Iron ore.", true);
    toggle("showCopper", "Copper", "Copper ore.", true);
    toggle("showRedstone", "Redstone", "Redstone ore.", true);
    toggle("showLapis", "Lapis Lazuli", "Lapis lazuli ore.", true);
    toggle("showQuartz", "Nether Quartz", "Nether quartz ore.", true);
    toggle("showCoal", "Coal", "Coal ore; disabled by default to reduce clutter.", false);
    toggle("showAncientDebris", "Ancient Debris", "Ancient debris in the Nether.", true);
    toggle("showOtherOres", "Other ores", "Unknown ore IDs ending in _ore, colored white.", true);

    auto slider = [&](const char* key, const char* title, const char* description,
                      const char* minimum, const char* maximum, const char* step,
                      const char* unit, const char* defaultValue) {
        ConfigNodeV2 node;
        node.id = key; node.key = key; node.title = title; node.description = description;
        node.category = (std::string_view(key) == "outlineThickness") ? "render" : "scan"; node.type = ConfigControlTypeV2::SliderInt;
        node.defaultValue = defaultValue;
        node.currentValue = (std::string_view(key) == "outlineThickness")
            ? std::to_string(static_cast<int>(std::lround(outlineThickness * 100.0f)))
            : configText(config, key, defaultValue);
        node.hasCurrentValue = true; node.minValue = minimum; node.maxValue = maximum;
        node.step = step; node.unit = unit;
        schema.node(std::move(node));
    };
    slider("horizontalRadius", "Horizontal radius", "Scan/draw radius across X/Z (3× previous default).", "4", "48", "1", " blocks", "48");
    slider("verticalRadius", "Vertical radius", "Scan/draw radius across Y (3× previous default).", "2", "24", "1", " blocks", "24");
    slider("scanBudgetPerTick", "Scan budget", "Maximum blocks queried per game tick; lower values reduce spikes but take longer.", "64", "2048", "64", " blocks/tick", "512");
    slider("outlineThickness", "Outline thickness", "Increase the spacing of parallel edge strokes for a bolder outline.", "1", "10", "1", " units", "4");
    setConfigSchemaJson(moduleId, schema.toJson());
}

bool OreEspModule::onMenuConfigChanged(std::string_view key, std::string_view value) {
    if (key == "showDiamonds" || key == "showEmeralds" || key == "showGold" ||
        key == "showIron" || key == "showCopper" || key == "showRedstone" ||
        key == "showLapis" || key == "showQuartz" || key == "showCoal" ||
        key == "showAncientDebris" || key == "showOtherOres") {
        bool parsed = false;
        if (!parseBool(value, parsed)) return false;
        if (key == "showDiamonds") showDiamonds = parsed;
        else if (key == "showEmeralds") showEmeralds = parsed;
        else if (key == "showGold") showGold = parsed;
        else if (key == "showIron") showIron = parsed;
        else if (key == "showCopper") showCopper = parsed;
        else if (key == "showRedstone") showRedstone = parsed;
        else if (key == "showLapis") showLapis = parsed;
        else if (key == "showQuartz") showQuartz = parsed;
        else if (key == "showCoal") showCoal = parsed;
        else if (key == "showAncientDebris") showAncientDebris = parsed;
        else showOtherOres = parsed;
        onMenuRegistered(); // Rebuild currentValue fields so reopening the menu reflects the changed settings.
        return true;
    }
    int parsed = 0;
    if (!parseInteger(value, parsed)) return false;
    if (key == "horizontalRadius") {
        horizontalRadius = std::clamp(parsed, 4, 48);
        if (enabled && (m_scanActive || m_hasScannedAtLeastOnce)) m_rescanRequested = true;
    } else if (key == "verticalRadius") {
        verticalRadius = std::clamp(parsed, 2, 24);
        if (enabled && (m_scanActive || m_hasScannedAtLeastOnce)) m_rescanRequested = true;
    } else if (key == "scanBudgetPerTick") {
        scanBudgetPerTick = std::clamp(parsed, 64, 2048);
    } else if (key == "outlineThickness") {
        outlineThickness = std::clamp(static_cast<float>(parsed) / 100.0f, 0.01f, 0.10f);
    } else {
        return false;
    }
    onMenuRegistered(); // Keep menu values in sync after sliders change.
    return true;
}

void OreEspModule::loadConfig(const nlohmann::json& json) {
    Module::loadConfig(json);
    showDiamonds = json.value("showDiamonds", showDiamonds);
    showEmeralds = json.value("showEmeralds", showEmeralds);
    showGold = json.value("showGold", showGold);
    showIron = json.value("showIron", showIron);
    showCopper = json.value("showCopper", showCopper);
    showRedstone = json.value("showRedstone", showRedstone);
    showLapis = json.value("showLapis", showLapis);
    showQuartz = json.value("showQuartz", showQuartz);
    showCoal = json.value("showCoal", showCoal);
    showAncientDebris = json.value("showAncientDebris", showAncientDebris);
    showOtherOres = json.value("showOtherOres", showOtherOres);
    horizontalRadius = std::clamp(json.value("horizontalRadius", horizontalRadius), 4, 32);
    verticalRadius = std::clamp(json.value("verticalRadius", verticalRadius), 2, 24);
    scanBudgetPerTick = std::clamp(json.value("scanBudgetPerTick", scanBudgetPerTick), 64, 2048);
    outlineThickness = std::clamp(json.value("outlineThickness", outlineThickness), 0.01f, 0.10f);
}

void OreEspModule::saveConfig(nlohmann::json& json) {
    Module::saveConfig(json);
    json["showDiamonds"] = showDiamonds;
    json["showEmeralds"] = showEmeralds;
    json["showGold"] = showGold;
    json["showIron"] = showIron;
    json["showCopper"] = showCopper;
    json["showRedstone"] = showRedstone;
    json["showLapis"] = showLapis;
    json["showQuartz"] = showQuartz;
    json["showCoal"] = showCoal;
    json["showAncientDebris"] = showAncientDebris;
    json["showOtherOres"] = showOtherOres;
    json["horizontalRadius"] = std::clamp(horizontalRadius, 4, 48);
    json["verticalRadius"] = std::clamp(verticalRadius, 2, 24);
    json["scanBudgetPerTick"] = std::clamp(scanBudgetPerTick, 64, 2048);
    json["outlineThickness"] = std::clamp(outlineThickness, 0.01f, 0.10f);
}