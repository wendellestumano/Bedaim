#include "ultrakillhud.hpp"

#include "core/InventoryAccess.hpp"
#include "modules/ModuleRegistry.hpp"

#include <bedrocktools/events/AttackEvent.hpp>
#include <bedrocktools/events/AttackResultEvent.hpp>
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <pl/ModMenu.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {
using bedrocktools::sdk::Actor;
using bedrocktools::sdk::Player;
using Clock = std::chrono::steady_clock;

constexpr std::uint32_t kRed       = 0xFFFF3348;
constexpr std::uint32_t kCyan      = 0xFF58F2FF;
constexpr std::uint32_t kWhite     = 0xFFF4F4F0;
constexpr std::uint32_t kMuted     = 0xFFB5B4B1;
constexpr std::uint32_t kGold      = 0xFFFFD34E;
constexpr std::uint32_t kBlack     = 0xD908090B;
constexpr std::uint32_t kDeepBlack = 0xF20B0B0C;
constexpr std::uint32_t kTrack     = 0xFF252529;
constexpr std::uint32_t kShadow    = 0xFF050507;

const void* carriedItemStack(Player* player) {
    if (!player) return nullptr;
    auto** vtable = *reinterpret_cast<void***>(player);
    constexpr std::size_t index = bedrocktools::sdk::offsets::VTable::PlayerGetCarriedItem;
    if (!vtable || !vtable[index]) return nullptr;
    using GetCarriedItemFn = const void* (*)(void*);
    return reinterpret_cast<GetCarriedItemFn>(vtable[index])(player);
}

std::string itemLabel(std::string identifier) {
    if (identifier.empty()) return "EMPTY HAND";
    const auto colon = identifier.find(':');
    if (colon != std::string::npos) identifier.erase(0, colon + 1);
    // Bedrock description ids can be "item.minecraft.mace", "tile.stone", or
    // "minecraft:mace". Strip engine prefixes before presenting the label.
    if (identifier.rfind("item.", 0) == 0) identifier.erase(0, 5);
    if (identifier.rfind("tile.", 0) == 0) identifier.erase(0, 5);
    if (identifier.rfind("minecraft.", 0) == 0) identifier.erase(0, 10);
    for (char& c : identifier) {
        if (c == '_') c = ' ';
        else c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    constexpr std::size_t maxChars = 18;
    if (identifier.size() > maxChars) {
        identifier.resize(maxChars - 3);
        identifier += "...";
    }
    return identifier;
}

struct RankInfo {
    const char* letter;
    const char* name;
    std::uint32_t color;
};

RankInfo rankFor(std::uint64_t score) {
    if (score >= 18000) return {"ULTRA", "ULTRAKILL", kRed};
    if (score >= 14000) return {"SSS", "HOMICIDAL", kRed};
    if (score >= 9500) return {"SS", "SADISTIC", kRed};
    if (score >= 6000) return {"S", "SUPREME", kGold};
    if (score >= 3500) return {"A", "ANARCHIC", kRed};
    if (score >= 1800) return {"B", "BRUTAL", kRed};
    if (score >= 650) return {"C", "CHAOTIC", kCyan};
    return {"D", "DESTRUCTIVE", kWhite};
}

float rankProgressFor(std::uint64_t score) {
    constexpr std::uint64_t thresholds[] = {0, 650, 1800, 3500, 6000, 9500, 14000, 18000};
    for (std::size_t i = 1; i < 8; ++i) {
        if (score < thresholds[i]) {
            const auto lower = thresholds[i - 1];
            const auto upper = thresholds[i];
            return std::clamp(static_cast<float>(score - lower) /
                              static_cast<float>(upper - lower), 0.0f, 1.0f);
        }
    }
    return 1.0f;
}

float clamp01(float value) {
    return std::clamp(value, 0.0f, 1.0f);
}

std::uint32_t withAlpha(std::uint32_t rgb, int alpha) {
    const auto a = static_cast<std::uint32_t>(std::clamp(alpha, 0, 255));
    return (a << 24) | (rgb & 0x00FFFFFFu);
}
} // namespace

UltrakillHudModule::UltrakillHudModule()
    : Module("ULTRAKILL HUD",
             "ULTRAKILL-inspired HUD with confirmed mace-impact flash and hit pulse.") {
}

void UltrakillHudModule::onInit() {
    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [this](auto& event) { onLocalPlayerTick(event.player); });
    bedrocktools::events::bus().subscribe<bedrocktools::events::AttackEvent>(
        [this](auto& event) { onAttackAttempt(event.target); });
    bedrocktools::events::bus().subscribe<bedrocktools::events::AttackResultEvent>(
        [this](auto& event) { onAttackResult(event.target, event.result); });


}

void UltrakillHudModule::onLocalPlayerTick(Player* player) {
    if (!player) return;
    m_localPlayer.store(player, std::memory_order_release);
    if (!enabled) return;

    const std::string itemId = bedrocktools::core::InventoryAccess::get().stackIdentifier(
        carriedItemStack(player));
    const auto currentPosition = player->position();
    const float currentY = currentPosition.y;

    std::lock_guard lock(m_stateMutex);
    m_selectedItemId = itemId;
    if (std::isfinite(currentPosition.x) && std::isfinite(currentPosition.y) &&
        std::isfinite(currentPosition.z)) {
        if (m_hasLastPosition) {
            const float dx = currentPosition.x - m_lastPosition.x;
            const float dy = currentPosition.y - m_lastPosition.y;
            const float dz = currentPosition.z - m_lastPosition.z;
            const float instantMoveSpeed = std::sqrt(dx * dx + dy * dy + dz * dz) * 20.0f;
            if (std::isfinite(instantMoveSpeed) && instantMoveSpeed < 120.0f) {
                const float previous = m_movementSpeedBps.load(std::memory_order_relaxed);
                m_movementSpeedBps.store(previous * 0.35f + instantMoveSpeed * 0.65f,
                                          std::memory_order_relaxed);
            }
        } else {
            m_hasLastPosition = true;
        }
        m_lastPosition = currentPosition;
    } else {
        m_hasLastPosition = false;
        m_movementSpeedBps.store(0.0f, std::memory_order_relaxed);
    }

    if (m_hasLastY) {
        // Estimate vertical blocks/second from ticks. This is an estimate rather
        // than a native velocity attribute exposed by the SDK.
        const float instantSpeed = (currentY - m_lastY) * 20.0f;
        if (std::isfinite(instantSpeed) && std::fabs(instantSpeed) < 80.0f) {
            const float previous = m_fallSpeedBps.load(std::memory_order_relaxed);
            m_fallSpeedBps.store(previous * 0.35f + instantSpeed * 0.65f,
                                 std::memory_order_relaxed);
        }
    } else {
        m_hasLastY = true;
    }
    m_lastY = currentY;
}

void UltrakillHudModule::onAttackAttempt(Actor* target) {
    if (!enabled || !target) return;
    const float signedFallSpeed = m_fallSpeedBps.load(std::memory_order_relaxed);
    const auto now = Clock::now();
    std::lock_guard lock(m_stateMutex);
    if (!m_enableParryFlash) return;
    m_flashStarted = now;
    m_impactFallSpeedBps = std::isfinite(signedFallSpeed)
        ? std::max(0.0f, -signedFallSpeed) : 0.0f;
    m_flashActive = true;
}

void UltrakillHudModule::onAttackResult(Actor* target, bool result) {
    if (!enabled || !target || !result) return;
    Player* player = m_localPlayer.load(std::memory_order_acquire);
    if (!player) return;
    // Style score tracks every confirmed hit, not only mace attacks. Restricting
    // this handler to a mace made the meter appear frozen while other items were used.

    const auto now = Clock::now();
    std::lock_guard lock(m_stateMutex);
    if (m_lastConfirmedHit.time_since_epoch().count() != 0 &&
        now - m_lastConfirmedHit < std::chrono::milliseconds(1500)) {
        ++m_combo;
    } else {
        m_combo = 1;
    }
    m_styleScore = std::min<std::uint64_t>(999999ULL, m_styleScore + (250ULL * m_combo));
    m_lastConfirmedHit = now;
    m_flashStarted = now;
    m_impactFallSpeedBps = std::max(
        0.0f, -m_fallSpeedBps.load(std::memory_order_relaxed));
    m_flashActive = true;

}

void UltrakillHudModule::onEnable() {
    std::lock_guard lock(m_stateMutex);
    m_styleScore = 0;
    m_combo = 0;
    m_lastConfirmedHit = {};
    m_flashStarted = {};
    m_impactFallSpeedBps = 0.0f;
    m_flashActive = false;
    m_hasLastY = false;
    m_hasLastPosition = false;
    m_lastPosition = {};
    m_fallSpeedBps.store(0.0f, std::memory_order_relaxed);
    m_movementSpeedBps.store(0.0f, std::memory_order_relaxed);
}

void UltrakillHudModule::onDisable() {
    {
        std::lock_guard lock(m_stateMutex);
        m_flashActive = false;
        m_hitStopFrames = 0;
        m_hitStopPending = false;
    }
    const std::vector<PLModMenu_DrawCommand> empty;
    submitDrawCommands(moduleId, empty);
}

void UltrakillHudModule::onFrame() {
    if (!enabled) return;

    const auto now = Clock::now();
    std::uint64_t score = 0;
    unsigned int combo = 0;
    Clock::time_point lastHit{};
    Clock::time_point flashStarted{};
    float impactFallSpeed = 0.0f;
    bool flashActive = false;
    std::string selectedItemId;
    bool showMaceStatus = true;
    bool showStyleCounter = true;
    bool showReticle = true;
    bool enableImpactShake = true;
    bool enableHitStop = true;
    bool showImpactText = true;

    {
        std::lock_guard lock(m_stateMutex);
        selectedItemId = m_selectedItemId;
        showMaceStatus = m_showMaceStatus;
        showStyleCounter = m_showStyleCounter;
        showReticle = m_showReticle;
        enableImpactShake = m_enableImpactShake;
        enableHitStop = m_enableHitStop;
        showImpactText = m_showImpactText;

        if (m_flashActive && now - m_flashStarted >= std::chrono::milliseconds(250)) {
            m_flashActive = false;
        }
        if (m_lastConfirmedHit.time_since_epoch().count() != 0 &&
            now - m_lastConfirmedHit >= std::chrono::milliseconds(1500)) {
            m_combo = 0;
        }

        score = m_styleScore;
        combo = m_combo;
        lastHit = m_lastConfirmedHit;
        flashStarted = m_flashStarted;
        impactFallSpeed = m_impactFallSpeedBps;
        flashActive = m_flashActive;
    }

    const auto surface = pl::modmenu::getHudSurfaceSize();
    const float screenW = surface.width;
    const float screenH = surface.height;
    if (!(screenW > 0.0f && screenH > 0.0f)) {
        const std::vector<PLModMenu_DrawCommand> empty;
        submitDrawCommands(moduleId, empty);
        return;
    }

    const float scale = std::clamp(screenW / 1500.0f, 0.70f, 1.15f);
    const bool hasSelectedItem = !selectedItemId.empty();
    const float signedFallSpeed = m_fallSpeedBps.load(std::memory_order_relaxed);
    const float downwardSpeed = std::max(0.0f, -signedFallSpeed);
    const float movementSpeed = m_movementSpeedBps.load(std::memory_order_relaxed);
    const float hitAge = flashActive
        ? std::chrono::duration<float>(now - flashStarted).count()
        : 100.0f;
    const float impactDuration = enableHitStop ? 0.36f : 0.25f;
    const bool impactVisible = flashActive && hitAge >= 0.0f && hitAge < impactDuration;
    const float impactProgress = impactVisible ? clamp01(1.0f - hitAge / impactDuration) : 0.0f;

    float shakeX = 0.0f;
    float shakeY = 0.0f;
    if (enableImpactShake && impactVisible) {
        shakeX = std::sin(hitAge * 520.0f) * 5.0f * scale * impactProgress;
        shakeY = std::cos(hitAge * 410.0f) * 3.0f * scale * impactProgress;
    }

    std::vector<PLModMenu_DrawCommand> cmds;
    cmds.reserve(56);

    auto addRect = [&](float x, float y, float w, float h,
                       std::uint32_t color, float radius = 0.0f) {
        PLModMenu_DrawCommand cmd = {};
        cmd.type = PL_DRAW_RECT_FILLED;
        cmd.x = x;
        cmd.y = y;
        cmd.w = w;
        cmd.h = h;
        cmd.x3 = radius;
        cmd.color = color;
        cmds.push_back(std::move(cmd));
    };

    auto addLine = [&](float x, float y, float dx, float dy,
                       float thickness, std::uint32_t color) {
        PLModMenu_DrawCommand cmd = {};
        cmd.type = PL_DRAW_LINE;
        cmd.x = x;
        cmd.y = y;
        cmd.w = dx;
        cmd.h = dy;
        cmd.size = thickness;
        cmd.color = color;
        cmds.push_back(std::move(cmd));
    };

    auto addCircle = [&](float x, float y, float radius, std::uint32_t color) {
        PLModMenu_DrawCommand cmd = {};
        cmd.type = PL_DRAW_CIRCLE_FILLED;
        cmd.x = x;
        cmd.y = y;
        cmd.size = radius;
        cmd.color = color;
        cmds.push_back(std::move(cmd));
    };

    auto addText = [&](float x, float y, const std::string& text,
                       float size, std::uint32_t color) {
        PLModMenu_DrawCommand cmd = {};
        cmd.type = PL_DRAW_TEXT;
        cmd.x = x;
        cmd.y = y;
        cmd.color = color;
        cmd.size = size;
        cmd.text = text;
        cmds.push_back(std::move(cmd));
    };

    // ULTRAKILL-inspired white impact flash. This is a visual hit-stop pulse:
    // the game simulation is not paused and no artificial frame drops are introduced.
    if (m_enableParryFlash && impactVisible) {
        const float fade = std::exp(-std::max(0.0f, hitAge) / 0.052f);
        const int baseAlpha = impactFallSpeed >= 4.0f ? 255 : 232;
        const int alpha = static_cast<int>(static_cast<float>(baseAlpha) * fade);
        if (alpha >= 5) addRect(0.0f, 0.0f, screenW, screenH, withAlpha(kWhite, alpha));

        const float cx = screenW * 0.5f;
        const float cy = screenH * 0.5f;
        const float spread = (10.0f + (1.0f - impactProgress) * 22.0f) * scale;
        const int spokeAlpha = static_cast<int>(220.0f * impactProgress);
        addLine(cx - spread, cy, -spread * 0.55f, -spread * 0.35f,
                2.0f * scale, withAlpha(kRed, spokeAlpha));
        addLine(cx + spread, cy, spread * 0.55f, -spread * 0.35f,
                2.0f * scale, withAlpha(kCyan, spokeAlpha));
        addLine(cx, cy - spread, -spread * 0.35f, -spread * 0.55f,
                2.0f * scale, withAlpha(kWhite, spokeAlpha));
        addLine(cx, cy + spread, spread * 0.35f, spread * 0.55f,
                2.0f * scale, withAlpha(kWhite, spokeAlpha));
    }

    // Lower-left weapon panel: live held-item identifier and a horizontal outline glyph.
    if (showMaceStatus) {
        const float panelW = 286.0f * scale;
        const float panelH = 104.0f * scale;
        const float panelX = 16.0f * scale + shakeX;
        const float panelY = std::max(8.0f * scale, screenH - panelH - 16.0f * scale) + shakeY;
        const auto accent = hasSelectedItem ? kCyan : kRed;
        addRect(panelX, panelY, panelW, panelH, kBlack, 1.0f * scale);
        addRect(panelX, panelY, 3.0f * scale, panelH, accent);
        addText(panelX + 11.0f * scale, panelY + 6.0f * scale,
                "WEAPON / ITEM", 8.0f * scale, kMuted);
        addText(panelX + 11.0f * scale, panelY + 19.0f * scale,
                itemLabel(selectedItemId), 12.0f * scale, hasSelectedItem ? kWhite : kMuted);
        addText(panelX + 11.0f * scale, panelY + 42.0f * scale,
                "FALL SPEED", 7.0f * scale, kMuted);
        std::ostringstream fallSpeedStream;
        fallSpeedStream << std::fixed << std::setprecision(1) << downwardSpeed << " B/S";
        addText(panelX + 77.0f * scale, panelY + 40.0f * scale,
                fallSpeedStream.str(), 8.5f * scale, kWhite);
        addText(panelX + 11.0f * scale, panelY + 58.0f * scale,
                "MOVE SPEED", 7.0f * scale, kMuted);
        std::ostringstream movementSpeedStream;
        movementSpeedStream << std::fixed << std::setprecision(1) << movementSpeed << " B/S";
        addText(panelX + 77.0f * scale, panelY + 56.0f * scale,
                movementSpeedStream.str(), 8.5f * scale, kCyan);
        const float barX = panelX + 11.0f * scale;
        const float barW = panelW - 22.0f * scale;
        const float barH = 4.0f * scale;

        // Separate live meters for fall velocity and total movement velocity.
        const float fallBarY = panelY + 77.0f * scale;
        addRect(barX, fallBarY, barW, barH, kTrack);
        addRect(barX, fallBarY, barW * clamp01(downwardSpeed / 12.0f), barH,
                downwardSpeed >= 4.0f ? kRed : kCyan);

        const float moveBarY = panelY + 88.0f * scale;
        addRect(barX, moveBarY, barW, barH, kTrack);
        addRect(barX, moveBarY, barW * clamp01(movementSpeed / 12.0f), barH,
                movementSpeed >= 8.0f ? kGold : kCyan);
        // Scanline and tick marks add a compact retro HUD texture without image assets.
        for (int tick = 1; tick < 12; ++tick) {
            const float tickX = barX + barW * static_cast<float>(tick) / 12.0f;
            addRect(tickX, fallBarY, 1.0f * scale, barH, withAlpha(kDeepBlack, 180));
            addRect(tickX, moveBarY, 1.0f * scale, barH, withAlpha(kDeepBlack, 180));
        }
        // Pixel-scan texture gives the panel a subtle game-interface finish without
        // requiring image assets or introducing draw commands unsupported by Levi.
        for (int stripe = 0; stripe < 10; ++stripe) {
            const float stripeX = panelX + panelW - (static_cast<float>(stripe) * 13.0f + 9.0f) * scale;
            addRect(stripeX, panelY + 3.0f * scale, 5.0f * scale,
                    1.0f * scale, withAlpha(stripe % 2 ? kCyan : kRed, 110));
        }

        const float iconX = panelX + 190.0f * scale;
        const float iconY = panelY + 13.0f * scale;
        auto drawItemGlyph = [&](float ox, float oy, std::uint32_t color, float thickness) {
            auto line = [&](float x, float y, float dx, float dy) {
                addLine(ox + x * scale, oy + y * scale, dx * scale, dy * scale,
                        thickness * scale, color);
            };
            const auto& id = selectedItemId;
            if (id.find("sword") != std::string::npos || id.find("katana") != std::string::npos) {
                line(10, 34, 35, -22); line(38, 10, 13, -2); line(16, 29, 9, 8);
                line(8, 34, 8, 8); line(5, 42, 8, -8);
            } else if (id.find("pickaxe") != std::string::npos) {
                line(12, 41, 30, -30); line(28, 10, 24, -4); line(34, 7, 4, 7); line(52, 6, -3, 8);
            } else if (id.find("axe") != std::string::npos) {
                line(12, 41, 31, -31); line(37, 9, 15, -3); line(52, 6, -4, 17); line(48, 23, -12, -2);
            } else if (id.find("shovel") != std::string::npos || id.find("hoe") != std::string::npos) {
                line(11, 41, 32, -31); line(38, 8, 13, 0); line(51, 8, -2, 10); line(36, 18, 15, 0);
            } else if (id.find("mace") != std::string::npos || id.find("hammer") != std::string::npos) {
                line(9, 40, 34, -23); line(37, 8, 15, 0); line(52, 8, 0, 14); line(52, 22, -15, 0); line(37, 22, 0, -14);
            } else if (id.find("trident") != std::string::npos) {
                line(8, 37, 42, -17); line(42, 23, 10, -4); line(52, 19, -1, -9); line(51, 10, -6, 8);
                line(45, 18, -4, -7); line(41, 11, -5, 7);
            } else if (id.find("spear") != std::string::npos) {
                line(8, 42, 37, -34); line(40, 8, 12, -4); line(52, 4, -3, 13);
                line(49, 17, -10, -1); line(18, 33, 7, 7); line(13, 37, 8, 2);
            } else if (id.find("wind_charge") != std::string::npos) {
                line(13, 25, 9, -11); line(22, 14, 13, 0); line(35, 14, 9, 10);
                line(44, 24, -6, 12); line(38, 36, -13, 3); line(25, 39, -10, -7);
                line(15, 32, 1, -7); line(27, 22, 9, 1); line(36, 23, -2, 7);
            } else if (id.find("ender_pearl") != std::string::npos || id.find("snowball") != std::string::npos) {
                line(20, 12, 15, -4); line(35, 8, 10, 7); line(45, 15, 4, 14);
                line(49, 29, -8, 11); line(41, 40, -15, 1); line(26, 41, -11, -8);
                line(15, 33, 5, -14); line(20, 19, 11, -6); line(31, 13, 10, 7);
            } else if (id.find("totem") != std::string::npos) {
                line(17, 8, 25, 0); line(42, 8, 0, 28); line(42, 36, -25, 0);
                line(17, 36, 0, -28); line(17, 8, 8, 8); line(25, 16, 9, 0);
                line(34, 16, 8, -8); line(25, 16, 0, 12); line(25, 28, 9, 0);
            } else if (id.find("firework") != std::string::npos || id.find("rocket") != std::string::npos) {
                line(25, 41, 0, -18); line(25, 23, -7, 7); line(25, 23, 7, 7);
                line(18, 30, 0, -12); line(18, 18, 7, -8); line(25, 10, 7, 8);
                line(32, 18, 0, 12); line(22, 41, -4, 5); line(28, 41, 4, 5);
            } else if (id.find("arrow") != std::string::npos) {
                line(8, 40, 40, -29); line(36, 10, 12, -2); line(48, 8, -3, 12);
                line(8, 40, 3, -10); line(8, 40, 10, 3);
            } else if (id.find("bow") != std::string::npos || id.find("crossbow") != std::string::npos) {
                line(12, 9, 9, 8); line(21, 17, 0, 13); line(21, 30, -9, 9); line(12, 9, 0, 30); line(5, 24, 48, -2);
                if (id.find("crossbow") != std::string::npos) { line(22, 18, 15, 0); line(37, 18, 0, 9); line(28, 27, 9, 0); }
            } else if (id.find("shield") != std::string::npos) {
                line(12, 9, 34, 0); line(46, 9, -3, 22); line(43, 31, -14, 11); line(29, 42, -14, -11); line(15, 31, -3, -22);
            } else if (id.find("potion") != std::string::npos || id.find("bottle") != std::string::npos) {
                line(24, 8, 12, 0); line(26, 8, 0, 8); line(34, 8, 0, 8); line(26, 16, -7, 7); line(34, 16, 7, 7);
                line(19, 23, 0, 13); line(41, 23, 0, 13); line(19, 36, 22, 0);
            } else if (id.find("apple") != std::string::npos || id.find("bread") != std::string::npos ||
                       id.find("carrot") != std::string::npos || id.find("potato") != std::string::npos ||
                       id.find("meat") != std::string::npos || id.find("food") != std::string::npos) {
                line(15, 18, 8, -8); line(23, 10, 13, 0); line(36, 10, 10, 8); line(46, 18, -2, 14);
                line(44, 32, -12, 8); line(32, 40, -13, -4); line(19, 36, -4, -10);
            } else if (id.find("block") != std::string::npos || id.find("planks") != std::string::npos ||
                       id.find("stone") != std::string::npos || id.find("dirt") != std::string::npos) {
                line(14, 13, 17, -7); line(31, 6, 19, 7); line(50, 13, -17, 8); line(33, 21, -19, -8);
                line(14, 13, 0, 20); line(14, 33, 19, 9); line(33, 42, 17, -9); line(50, 33, 0, -20); line(33, 21, 0, 21);
            } else {
                line(12, 28, 15, -15); line(27, 13, 15, 15); line(42, 28, -15, 15); line(27, 43, -15, -15); line(27, 13, 0, 30);
            }
        };
        // Offset, thicker glyph gives the live item emblem a readable drop shadow.
        drawItemGlyph(3.0f * scale, 3.0f * scale, 0xCC000000u, 5.0f);
        drawItemGlyph(0.0f, 0.0f, accent, 2.0f);
    }

    // Upper-right style meter: rank, white progress, and gold FRESH tag.
    if (showStyleCounter) {
        const float cardW = 244.0f * scale;
        const float cardH = 70.0f * scale;
        const float cardX = std::max(8.0f * scale, screenW - cardW - 16.0f * scale) + shakeX;
        const float cardY = std::max(14.0f * scale, screenH * 0.055f) + shakeY;

        const RankInfo rank = rankFor(score);
        const float age = lastHit.time_since_epoch().count() == 0
            ? 99.0f : std::chrono::duration<float>(now - lastHit).count();
        const float freshness = age < 2.4f ? clamp01(1.0f - age / 2.4f) : 0.0f;
        const float meter = rankProgressFor(score);
        const float multiplier = 1.0f + 0.5f * freshness;

        addRect(cardX, cardY, cardW, cardH, kBlack, 1.0f * scale);
        addRect(cardX, cardY, cardW, 2.0f * scale, rank.color);
        addText(cardX + 8.0f * scale, cardY + 3.0f * scale,
                rank.letter, 28.0f * scale, rank.color);
        addText(cardX + 82.0f * scale, cardY + 6.0f * scale,
                rank.name, 13.0f * scale, kWhite);
        addText(cardX + 82.0f * scale, cardY + 24.0f * scale,
                std::to_string(score) + "  /  COMBO " + std::to_string(combo),
                7.8f * scale, kMuted);

        const float barX = cardX + 82.0f * scale;
        const float barY = cardY + 40.0f * scale;
        const float barW = cardW - 94.0f * scale;
        const float barH = 5.0f * scale;
        addRect(barX, barY, barW, barH, kTrack);
        addRect(barX, barY, barW * meter, barH, kWhite);

        const float tagY = cardY + 52.0f * scale;
        const float tagW = 80.0f * scale;
        addRect(barX, tagY, tagW, 14.0f * scale, kGold);
        const int hundredths = static_cast<int>(std::round(multiplier * 100.0f));
        const std::string freshText = std::string("FRESH: ") +
            std::to_string(hundredths / 100) + "." +
            (hundredths % 100 < 10 ? "0" : "") +
            std::to_string(hundredths % 100) + "x";
        addText(barX + 4.0f * scale, tagY + 2.0f * scale,
                freshText, 7.0f * scale, kDeepBlack);
    }

    if (showReticle) {
        const float cx = screenW * 0.5f + shakeX;
        const float cy = screenH * 0.5f + shakeY;
        const float arm = (impactVisible ? 8.0f + (1.0f - impactProgress) * 6.0f : 7.0f) * scale;
        const float gap = 4.0f * scale;
        const float thickness = (impactVisible ? 2.1f : 1.35f) * scale;
        const std::uint32_t color = impactVisible
            ? withAlpha(kWhite, static_cast<int>(255.0f * impactProgress))
            : kWhite;
        addLine(cx - gap, cy, -arm, 0.0f, thickness, kRed);
        addLine(cx + gap, cy, arm, 0.0f, thickness, kRed);
        addLine(cx, cy - gap, 0.0f, -arm, thickness, kCyan);
        addLine(cx, cy + gap, 0.0f, arm, thickness, kCyan);
        addCircle(cx, cy, 1.3f * scale, color);
    }

    if (showImpactText && impactVisible) {
        const float cx = screenW * 0.5f;
        const float cy = screenH * 0.5f;
        const bool selectedMace = selectedItemId.find("mace") != std::string::npos;
        const char* label = impactFallSpeed >= 4.0f
            ? "GROUND SLAM" : (selectedMace ? "MACE HIT" : "HIT CONFIRMED");
        addText(cx + 19.0f * scale, cy - 28.0f * scale,
                label, 12.0f * scale,
                withAlpha(impactFallSpeed >= 4.0f ? kRed : kWhite,
                          static_cast<int>(255.0f * impactProgress)));
        addText(cx + 19.0f * scale, cy - 13.0f * scale,
                "IMPACT FLASH", 7.0f * scale,
                withAlpha(kCyan, static_cast<int>(240.0f * impactProgress)));
    }

    submitDrawCommands(moduleId, cmds);
}

void UltrakillHudModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    std::lock_guard lock(m_stateMutex);
    if (j.contains("hudPosX")) hudPosX = j["hudPosX"].get<float>();
    if (j.contains("hudPosY")) hudPosY = j["hudPosY"].get<float>();
    if (j.contains("isHudModule")) isHudModule = j["isHudModule"].get<bool>();
    if (j.contains("m_showMaceStatus")) m_showMaceStatus = j["m_showMaceStatus"].get<bool>();
    if (j.contains("m_showStyleCounter")) m_showStyleCounter = j["m_showStyleCounter"].get<bool>();
    if (j.contains("m_showReticle")) m_showReticle = j["m_showReticle"].get<bool>();
    if (j.contains("m_enableParryFlash")) m_enableParryFlash = j["m_enableParryFlash"].get<bool>();
    if (j.contains("m_enableImpactShake")) m_enableImpactShake = j["m_enableImpactShake"].get<bool>();
    if (j.contains("m_enableHitStop")) m_enableHitStop = j["m_enableHitStop"].get<bool>();
    if (j.contains("m_showImpactText")) m_showImpactText = j["m_showImpactText"].get<bool>();
}

void UltrakillHudModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    std::lock_guard lock(m_stateMutex);
    j["hudPosX"] = hudPosX;
    j["hudPosY"] = hudPosY;
    j["isHudModule"] = isHudModule;
    j["m_showMaceStatus"] = m_showMaceStatus;
    j["m_showStyleCounter"] = m_showStyleCounter;
    j["m_showReticle"] = m_showReticle;
    j["m_enableParryFlash"] = m_enableParryFlash;
    j["m_enableImpactShake"] = m_enableImpactShake;
    j["m_enableHitStop"] = m_enableHitStop;
    j["m_showImpactText"] = m_showImpactText;
}
