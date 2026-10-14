#include "ultrakillhud.hpp"

#include "core/InventoryAccess.hpp"
#include "modules/ModuleRegistry.hpp"

#include <bedrocktools/events/AttackEvent.hpp>
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <pl/ModMenu.hpp>

#include <algorithm>
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

struct RankInfo {
    const char* letter;
    const char* name;
    std::uint32_t color;
};

RankInfo rankFor(std::uint64_t score) {
    if (score >= 18000) return {"SS", "ULTRAKILL", kRed};
    if (score >= 10000) return {"S", "SUPREME", kGold};
    if (score >= 5500) return {"A", "ANARCHIC", kRed};
    if (score >= 2800) return {"B", "BRUTAL", kRed};
    if (score >= 1200) return {"C", "CHAOTIC", kRed};
    return {"D", "DESTRUCTIVE", kWhite};
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

bool UltrakillHudModule::isMaceEquipped(Player* player) const {
    if (!player) return false;

    auto** vtable = *reinterpret_cast<void***>(player);
    constexpr std::size_t index = bedrocktools::sdk::offsets::VTable::PlayerGetCarriedItem;
    if (!vtable || !vtable[index]) return false;

    using GetCarriedItemFn = const void* (*)(void*);
    const void* stack = reinterpret_cast<GetCarriedItemFn>(vtable[index])(player);
    return stack && bedrocktools::core::InventoryAccess::get().countStack(stack, "mace") > 0;
}

void UltrakillHudModule::onInit() {
    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [this](auto& event) { onLocalPlayerTick(event.player); });
    bedrocktools::events::bus().subscribe<bedrocktools::events::AttackEvent>(
        [this](auto& event) { onAttack(event.target); });
}

void UltrakillHudModule::onLocalPlayerTick(Player* player) {
    if (!player) return;
    m_localPlayer.store(player, std::memory_order_release);
    if (!enabled) return;

    const bool hasMace = isMaceEquipped(player);
    const float currentY = player->position().y;
    m_maceEquipped.store(hasMace, std::memory_order_relaxed);

    std::lock_guard lock(m_stateMutex);
    if (m_hasLastY) {
        // Estimate blocks/second from local-player tick positions. This is an estimate,
        // not a native velocity attribute exposed by the SDK.
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

void UltrakillHudModule::onAttack(Actor* target) {
    if (!enabled || !target) return;

    Player* player = m_localPlayer.load(std::memory_order_acquire);
    if (!player || !isMaceEquipped(player)) return;

    // AttackEvent is emitted just before the native attack. Keep a short-lived candidate,
    // then require the target's hurtTime to increase before counting/rendering an impact.
    const int hurtBefore = target->hurtTime();
    const auto now = Clock::now();

    std::lock_guard lock(m_stateMutex);
    m_pendingTarget = target;
    m_pendingHurtTime = hurtBefore;
    m_pendingDeadline = now + std::chrono::milliseconds(240);
    m_hasPendingTarget = true;
}

void UltrakillHudModule::onEnable() {
    std::lock_guard lock(m_stateMutex);
    m_styleScore = 0;
    m_combo = 0;
    m_lastConfirmedHit = {};
    m_flashStarted = {};
    m_impactFallSpeedBps = 0.0f;
    m_flashActive = false;
    m_hasPendingTarget = false;
    m_pendingTarget = nullptr;
    m_hasLastY = false;
    m_fallSpeedBps.store(0.0f, std::memory_order_relaxed);
}

void UltrakillHudModule::onDisable() {
    {
        std::lock_guard lock(m_stateMutex);
        m_flashActive = false;
        m_hasPendingTarget = false;
        m_pendingTarget = nullptr;
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

    {
        std::lock_guard lock(m_stateMutex);

        if (m_hasPendingTarget) {
            if (now > m_pendingDeadline) {
                m_hasPendingTarget = false;
                m_pendingTarget = nullptr;
            } else if (m_pendingTarget) {
                const int hurtAfter = m_pendingTarget->hurtTime();
                if (hurtAfter > 0 && hurtAfter > m_pendingHurtTime) {
                    m_hasPendingTarget = false;
                    m_pendingTarget = nullptr;

                    if (m_lastConfirmedHit.time_since_epoch().count() != 0 &&
                        now - m_lastConfirmedHit < std::chrono::milliseconds(1500)) {
                        ++m_combo;
                    } else {
                        m_combo = 1;
                    }
                    m_styleScore = std::min<std::uint64_t>(
                        999999ULL, m_styleScore + (250ULL * m_combo));
                    m_lastConfirmedHit = now;
                    m_flashStarted = now;
                    m_impactFallSpeedBps =
                        std::max(0.0f, -m_fallSpeedBps.load(std::memory_order_relaxed));
                    m_flashActive = true;
                }
            }
        }

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
    const bool hasMace = m_maceEquipped.load(std::memory_order_relaxed);
    const float signedFallSpeed = m_fallSpeedBps.load(std::memory_order_relaxed);
    const float downwardSpeed = std::max(0.0f, -signedFallSpeed);
    const float hitAge = flashActive
        ? std::chrono::duration<float>(now - flashStarted).count()
        : 100.0f;
    const bool impactVisible = flashActive && hitAge >= 0.0f && hitAge < 0.25f;
    const float impactProgress = impactVisible ? clamp01(1.0f - hitAge / 0.25f) : 0.0f;

    float shakeX = 0.0f;
    float shakeY = 0.0f;
    if (m_enableImpactShake && impactVisible) {
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
        const int baseAlpha = impactFallSpeed >= 4.0f ? 228 : 190;
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

    // Lower-left weapon/status widget. The fall-speed meter is live (sampled from
    // player position); unlike the old mockup, it does not claim to show fake HP.
    if (m_showMaceStatus) {
        const float panelW = 244.0f * scale;
        const float panelH = 78.0f * scale;
        const float panelX = 18.0f * scale + shakeX;
        const float panelY = std::max(8.0f * scale, screenH - panelH - 18.0f * scale) + shakeY;

        addRect(panelX, panelY, panelW, panelH, kBlack, 1.0f * scale);
        addRect(panelX, panelY, 3.0f * scale, panelH, hasMace ? kCyan : kRed);
        addText(panelX + 12.0f * scale, panelY + 7.0f * scale,
                "MACE / PRIMARY", 9.5f * scale, kMuted);
        addText(panelX + 12.0f * scale, panelY + 20.0f * scale,
                hasMace ? "EQUIPPED" : "NOT EQUIPPED",
                15.0f * scale, hasMace ? kCyan : kWhite);

        const float iconX = panelX + panelW - 38.0f * scale;
        const float iconY = panelY + 13.0f * scale;
        addLine(iconX - 7.0f * scale, iconY + 25.0f * scale,
                21.0f * scale, -21.0f * scale, 4.0f * scale, kMuted);
        addRect(iconX + 6.0f * scale, iconY - 2.0f * scale,
                15.0f * scale, 15.0f * scale, kRed, 1.0f * scale);
        addLine(iconX + 8.0f * scale, iconY - 3.0f * scale,
                11.0f * scale, 0.0f, 1.0f * scale, kWhite);

        addText(panelX + 12.0f * scale, panelY + 42.0f * scale,
                "FALL SPEED", 7.5f * scale, kMuted);
        std::ostringstream speedStream;
        speedStream << std::fixed << std::setprecision(1) << downwardSpeed << " b/s";
        addText(panelX + 78.0f * scale, panelY + 40.0f * scale,
                speedStream.str(), 9.0f * scale, kWhite);

        const float barX = panelX + 12.0f * scale;
        const float barY = panelY + 59.0f * scale;
        const float barW = panelW - 24.0f * scale;
        const float barH = 5.0f * scale;
        addRect(barX, barY, barW, barH, kTrack);
        addRect(barX, barY, barW * clamp01(downwardSpeed / 12.0f), barH,
                downwardSpeed >= 4.0f ? kRed : kCyan);
    }

    // Upper-right style meter: rank, white progress, and gold FRESH tag.
    if (m_showStyleCounter) {
        const float cardW = 226.0f * scale;
        const float cardH = 76.0f * scale;
        const float cardX = std::max(8.0f * scale, screenW - cardW - 18.0f * scale) + shakeX;
        const float cardY = std::clamp(screenH * 0.20f, 18.0f * scale,
                                       std::max(18.0f * scale, screenH - cardH - 18.0f * scale)) + shakeY;

        const RankInfo rank = rankFor(score);
        const float age = lastHit.time_since_epoch().count() == 0
            ? 99.0f : std::chrono::duration<float>(now - lastHit).count();
        const float freshness = age < 2.4f ? clamp01(1.0f - age / 2.4f) : 0.0f;
        const float meter = score == 0 ? 0.12f : 0.20f + freshness * 0.80f;
        const float multiplier = 1.0f + 0.5f * freshness;

        addRect(cardX, cardY, cardW, cardH, kBlack, 1.0f * scale);
        addText(cardX + 8.0f * scale, cardY - 2.0f * scale,
                rank.letter, 34.0f * scale, rank.color);
        addText(cardX + 55.0f * scale, cardY + 7.0f * scale,
                rank.name, 13.0f * scale, rank.color);
        addText(cardX + 55.0f * scale, cardY + 25.0f * scale,
                std::to_string(score) + "  /  COMBO " + std::to_string(combo),
                7.8f * scale, kMuted);

        const float barX = cardX + 55.0f * scale;
        const float barY = cardY + 40.0f * scale;
        const float barW = cardW - 66.0f * scale;
        const float barH = 6.0f * scale;
        addRect(barX, barY, barW, barH, kTrack);
        addRect(barX, barY, barW * meter, barH, kWhite);

        const float tagY = cardY + 53.0f * scale;
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

    if (m_showReticle) {
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

    if (m_showImpactText && impactVisible) {
        const float cx = screenW * 0.5f;
        const float cy = screenH * 0.5f;
        const char* label = impactFallSpeed >= 4.0f ? "MACE IMPACT" : "MACE HIT";
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
    if (j.contains("hudPosX")) hudPosX = j["hudPosX"].get<float>();
    if (j.contains("hudPosY")) hudPosY = j["hudPosY"].get<float>();
    if (j.contains("isHudModule")) isHudModule = j["isHudModule"].get<bool>();
    if (j.contains("m_showMaceStatus")) m_showMaceStatus = j["m_showMaceStatus"].get<bool>();
    if (j.contains("m_showStyleCounter")) m_showStyleCounter = j["m_showStyleCounter"].get<bool>();
    if (j.contains("m_showReticle")) m_showReticle = j["m_showReticle"].get<bool>();
    if (j.contains("m_enableParryFlash")) m_enableParryFlash = j["m_enableParryFlash"].get<bool>();
    if (j.contains("m_enableImpactShake")) m_enableImpactShake = j["m_enableImpactShake"].get<bool>();
    if (j.contains("m_showImpactText")) m_showImpactText = j["m_showImpactText"].get<bool>();
}

void UltrakillHudModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["hudPosX"] = hudPosX;
    j["hudPosY"] = hudPosY;
    j["isHudModule"] = isHudModule;
    j["m_showMaceStatus"] = m_showMaceStatus;
    j["m_showStyleCounter"] = m_showStyleCounter;
    j["m_showReticle"] = m_showReticle;
    j["m_enableParryFlash"] = m_enableParryFlash;
    j["m_enableImpactShake"] = m_enableImpactShake;
    j["m_showImpactText"] = m_showImpactText;
}
