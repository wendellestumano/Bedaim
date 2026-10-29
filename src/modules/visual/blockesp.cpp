#include "blockesp.hpp"
#include "../ModuleRegistry.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/world/BlockSource.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>
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
constexpr std::size_t kMaxBlockEspCache = 4096;
constexpr std::size_t kMaxBlockEspRendered = 512;

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
}

BlockEspModule::BlockEspModule()
    : Module("BlockESP", "Colored outlines and tracers for useful blocks: shulkers, storage, redstone and workstations.") {
    showInMenu = true;
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
bool BlockEspModule::classify(std::string_view raw, Kind& kind) noexcept {
    const std::string name = stripNamespace(raw);
    if (name.find("shulker_box") != std::string::npos) kind = Kind::Shulker;
    else if (name == "ender_chest") kind = Kind::EnderChest;
    else if (name == "trapped_chest" || name == "chest") kind = Kind::Chest;
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
             name == "redstone_block" || name == "lever" || name.find("button") != std::string::npos ||
             name.find("pressure_plate") != std::string::npos || name == "target") kind = Kind::Utility;
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
        case Kind::Hopper: return showHoppers;
        case Kind::Dispenser: return showDispensers;
        case Kind::Dropper: return showDroppers;
        case Kind::Barrel: return showBarrels;
        case Kind::Furnace: return showFurnaces;
        case Kind::Utility: return showUtilityBlocks;
        default: return false;
    }
}
std::uint32_t BlockEspModule::colorFor(Kind kind) const noexcept {
    switch (kind) {
        case Kind::Shulker: return shulkerColor;
        case Kind::Chest: return chestColor;
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
    m_blockSource = nullptr; m_scanActive = false; m_hasCenter = false; m_scanCursor = 0; m_scanTotal = 0;
    clearCache();
}
void BlockEspModule::onDisable() {
    m_scanActive = false; m_hasCenter = false; m_scanCursor = 0; m_scanTotal = 0;
    clearCache();
}
void BlockEspModule::onPlayerTick(bedrocktools::sdk::Player* player) {
    ++m_tickCount;
    if (!enabled || !player) return;
    auto* dimension = player->dimension();
    auto* source = dimension ? dimension->blockSource() : nullptr;
    if (!source) return;
    if (source != m_blockSource) {
        m_blockSource = source; m_scanActive = false; m_hasCenter = false; clearCache();
    }
    if (!g_blockEspGetBlock.load(std::memory_order_acquire) && m_tickCount - m_lastResolveAttemptTick >= 40) {
        m_lastResolveAttemptTick = m_tickCount; resolveBlockGetBlock();
    }
    const auto getBlock = g_blockEspGetBlock.load(std::memory_order_acquire);
    if (!getBlock) return;

    const auto p = player->position();
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return;
    const bedrocktools::sdk::BlockPos current{
        static_cast<int>(std::floor(p.x)), static_cast<int>(std::floor(p.y)), static_cast<int>(std::floor(p.z))};
    const int rx = std::clamp(horizontalRadius, 4, 32);
    const int ry = std::clamp(verticalRadius, 2, 16);
    const int moveThreshold = std::max(4, rx / 2);
    if (!m_hasCenter || (!m_scanActive &&
        (std::abs(current.x - m_lastCenter.x) >= moveThreshold ||
         std::abs(current.z - m_lastCenter.z) >= moveThreshold ||
         std::abs(current.y - m_lastCenter.y) >= std::max(2, ry / 2)))) {
        m_scanCenter = current; m_lastCenter = current; m_hasCenter = true;
        m_scanCursor = 0;
        const std::size_t diameter = static_cast<std::size_t>(rx * 2 + 1);
        m_scanTotal = diameter * diameter * static_cast<std::size_t>(ry * 2 + 1);
        m_scanActive = true;
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        for (auto it = m_cache.begin(); it != m_cache.end();) {
            if (std::abs(it->second.position.x - current.x) > rx * 2 ||
                std::abs(it->second.position.z - current.z) > rx * 2 ||
                std::abs(it->second.position.y - current.y) > ry * 2) it = m_cache.erase(it);
            else ++it;
        }
    }
    if (!m_scanActive) return;

    const std::size_t diameter = static_cast<std::size_t>(rx * 2 + 1);
    const std::size_t plane = diameter * diameter;
    const std::size_t end = std::min(m_scanTotal, m_scanCursor +
        static_cast<std::size_t>(std::clamp(scanBudgetPerTick, 128, 2048)));
    for (std::size_t index = m_scanCursor; index < end; ++index) {
        const int dx = static_cast<int>(index % diameter) - rx;
        const int dz = static_cast<int>((index / diameter) % diameter) - rx;
        const int dy = static_cast<int>(index / plane) - ry;
        const bedrocktools::sdk::BlockPos pos{m_scanCenter.x + dx, m_scanCenter.y + dy, m_scanCenter.z + dz};
        const PositionKey key = keyFor(pos);
        void* raw = getBlock(source, pos);
        if (!raw || reinterpret_cast<std::uintptr_t>(raw) <= 0x1000) continue;
        const auto* name = reinterpret_cast<const bedrocktools::sdk::Block*>(raw)->fullName();
        if (!name || name->empty()) continue;
        Kind kind{};
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        m_cache.erase(key);
        if (classify(*name, kind) && m_cache.size() < kMaxBlockEspCache) {
            m_cache.emplace(key, Outline{pos, colorFor(kind), kind});
        }
    }
    m_scanCursor = end;
    if (m_scanCursor >= m_scanTotal) m_scanActive = false;
}
void BlockEspModule::getRenderSnapshot(const bedrocktools::sdk::Vec3& playerPosition,
                                       std::vector<Outline>& output) const {
    output.clear();
    if (!enabled || !masterEnabled || !keybindActive) return;
    const int rx = std::clamp(horizontalRadius, 4, 32);
    const int ry = std::clamp(verticalRadius, 2, 16);
    output.reserve(kMaxBlockEspRendered);
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    for (const auto& [key, outline] : m_cache) {
        (void)key;
        if (!isKindEnabled(outline.kind)) continue;
        const float dx = static_cast<float>(outline.position.x) + 0.5f - playerPosition.x;
        const float dy = static_cast<float>(outline.position.y) + 0.5f - playerPosition.y;
        const float dz = static_cast<float>(outline.position.z) + 0.5f - playerPosition.z;
        if (dx * dx + dz * dz > static_cast<float>(rx * rx) || std::fabs(dy) > static_cast<float>(ry)) continue;
        output.push_back(Outline{outline.position, colorFor(outline.kind), outline.kind});
        if (output.size() >= kMaxBlockEspRendered) break;
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
    toggle("showHoppers","Hoppers","Hoppers.",true,"types");
    toggle("showDispensers","Dispensers","Dispensers.",true,"types");
    toggle("showDroppers","Droppers","Droppers.",true,"types");
    toggle("showBarrels","Barrels","Barrels.",true,"types");
    toggle("showFurnaces","Furnaces","Furnaces, blast furnaces and smokers.",true,"types");
    toggle("showUtilityBlocks","Other useful blocks","Workstations and selected redstone utility blocks.",true,"types");
    toggle("showTracer","Show tracer","Draw a line from the camera to each detected block.",true,"tracer");
    auto slider = [&](const char* key,const char* title,const char* desc,const char* cat,int def,int min,int max,int step,const char* unit) {
        ConfigNodeV2 n; n.id=key; n.key=key; n.title=title; n.description=desc; n.category=cat;
        n.type=ConfigControlTypeV2::SliderInt; n.defaultValue=std::to_string(def);
        n.currentValue=configText(config,key,n.defaultValue.c_str()); n.hasCurrentValue=true;
        n.minValue=std::to_string(min); n.maxValue=std::to_string(max); n.step=std::to_string(step); n.unit=unit; schema.node(std::move(n));
    };
    slider("horizontalRadius","Horizontal radius","Horizontal scan and render radius.", "scan",16,4,32,1," blocks");
    slider("verticalRadius","Vertical radius","Vertical scan and render radius.", "scan",8,2,16,1," blocks");
    slider("scanBudgetPerTick","Scan budget","Block queries performed per tick.", "scan",1024,128,2048,128," blocks/tick");
    slider("outlineThickness","Outline thickness","Approximate thickness of block outlines.", "tracer",4,1,10,1," units");
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
    color("tracerColor","Tracer color",tracerColor);
    setConfigSchemaJson(moduleId,schema.toJson());
}
bool BlockEspModule::onMenuConfigChanged(std::string_view key, std::string_view value) {
    bool b=false;
    if (key=="showTracer"||key=="showShulkers"||key=="showChests"||key=="showEnderChests"||
        key=="showHoppers"||key=="showDispensers"||key=="showDroppers"||key=="showBarrels"||
        key=="showFurnaces"||key=="showUtilityBlocks") {
        if (!parseBool(value,b)) return false;
        if(key=="showTracer")showTracer=b; else if(key=="showShulkers")showShulkers=b;
        else if(key=="showChests")showChests=b; else if(key=="showEnderChests")showEnderChests=b;
        else if(key=="showHoppers")showHoppers=b; else if(key=="showDispensers")showDispensers=b;
        else if(key=="showDroppers")showDroppers=b; else if(key=="showBarrels")showBarrels=b;
        else if(key=="showFurnaces")showFurnaces=b; else showUtilityBlocks=b;
        onMenuRegistered(); return true;
    }
    int i=0;
    if(key=="horizontalRadius"||key=="verticalRadius"||key=="scanBudgetPerTick"||key=="outlineThickness") {
        if(!parseInt(value,i)) return false;
        if(key=="horizontalRadius")horizontalRadius=std::clamp(i,4,32);
        else if(key=="verticalRadius")verticalRadius=std::clamp(i,2,16);
        else if(key=="scanBudgetPerTick")scanBudgetPerTick=std::clamp(i,128,2048);
        else outlineThickness=static_cast<float>(std::clamp(i,1,10))/100.0f;
        m_scanActive=false; m_hasCenter=false; onMenuRegistered(); return true;
    }
    std::uint32_t c=0; if(!parseColor(value,c)) return false;
    if(key=="shulkerColor")shulkerColor=c; else if(key=="chestColor")chestColor=c;
    else if(key=="enderChestColor")enderChestColor=c; else if(key=="hopperColor")hopperColor=c;
    else if(key=="dispenserColor")dispenserColor=c; else if(key=="dropperColor")dropperColor=c;
    else if(key=="barrelColor")barrelColor=c; else if(key=="furnaceColor")furnaceColor=c;
    else if(key=="utilityColor")utilityColor=c; else if(key=="tracerColor")tracerColor=c;
    else return false;
    onMenuRegistered(); return true;
}
void BlockEspModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    showTracer=j.value("showTracer",showTracer); showShulkers=j.value("showShulkers",showShulkers);
    showChests=j.value("showChests",showChests); showEnderChests=j.value("showEnderChests",showEnderChests);
    showHoppers=j.value("showHoppers",showHoppers); showDispensers=j.value("showDispensers",showDispensers);
    showDroppers=j.value("showDroppers",showDroppers); showBarrels=j.value("showBarrels",showBarrels);
    showFurnaces=j.value("showFurnaces",showFurnaces); showUtilityBlocks=j.value("showUtilityBlocks",showUtilityBlocks);
    horizontalRadius=std::clamp(j.value("horizontalRadius",horizontalRadius),4,32);
    verticalRadius=std::clamp(j.value("verticalRadius",verticalRadius),2,16);
    scanBudgetPerTick=std::clamp(j.value("scanBudgetPerTick",scanBudgetPerTick),128,2048);
    outlineThickness=static_cast<float>(std::clamp(j.value("outlineThickness",4),1,10))/100.0f;
    loadColor(j,"shulkerColor",shulkerColor); loadColor(j,"chestColor",chestColor);
    loadColor(j,"enderChestColor",enderChestColor); loadColor(j,"hopperColor",hopperColor);
    loadColor(j,"dispenserColor",dispenserColor); loadColor(j,"dropperColor",dropperColor);
    loadColor(j,"barrelColor",barrelColor); loadColor(j,"furnaceColor",furnaceColor);
    loadColor(j,"utilityColor",utilityColor); loadColor(j,"tracerColor",tracerColor);
}
void BlockEspModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["showTracer"]=showTracer; j["showShulkers"]=showShulkers; j["showChests"]=showChests;
    j["showEnderChests"]=showEnderChests; j["showHoppers"]=showHoppers; j["showDispensers"]=showDispensers;
    j["showDroppers"]=showDroppers; j["showBarrels"]=showBarrels; j["showFurnaces"]=showFurnaces;
    j["showUtilityBlocks"]=showUtilityBlocks; j["horizontalRadius"]=horizontalRadius;
    j["verticalRadius"]=verticalRadius; j["scanBudgetPerTick"]=scanBudgetPerTick;
    j["outlineThickness"]=static_cast<int>(std::lround(outlineThickness*100.0f));
    auto save=[&](const char* key,std::uint32_t c){char text[12];std::snprintf(text,sizeof(text),"#%08X",c);j[key]=std::string(text);};
    save("shulkerColor",shulkerColor); save("chestColor",chestColor); save("enderChestColor",enderChestColor);
    save("hopperColor",hopperColor); save("dispenserColor",dispenserColor); save("dropperColor",dropperColor);
    save("barrelColor",barrelColor); save("furnaceColor",furnaceColor); save("utilityColor",utilityColor); save("tracerColor",tracerColor);
}
