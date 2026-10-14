#include "ultrakillhud.hpp"

#include "modules/ModuleRegistry.hpp"
#include <bedrocktools/events/AttackEvent.hpp>
#include <bedrocktools/events/EventBus.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace {

constexpr std::uint32_t kRed       = 0xFFFF344D;
constexpr std::uint32_t kCyan      = 0xFF52F3FF;
constexpr std::uint32_t kWhite     = 0xFFF2F5FA;
constexpr std::uint32_t kMuted     = 0xFF9BA8B9;
constexpr std::uint32_t kPanel     = 0xD90A0D14;
constexpr std::uint32_t kPanelDark = 0xE6070910;
constexpr std::uint32_t kDim       = 0x70323D4B;
constexpr std::uint32_t kOutline   = 0xC8202630;

} // namespace

UltrakillHudModule::UltrakillHudModule()
    : Module("ULTRAKILL HUD",
             "ULTRAKILL-inspired visual HUD. The parry panel is decorative and does not automate combat.") {
    hideInHudEditor = false;
}

void UltrakillHudModule::onInit() {
    // Count attack events for the style panel only. This does not change or cancel attacks.
    bedrocktools::events::bus().subscribe<bedrocktools::events::AttackEvent>([this](auto&) {
        if (!enabled || !m_showStyleCounter) return;
        m_actionCount.fetch_add(1, std::memory_order_relaxed);
    });
}

void UltrakillHudModule::onEnable() {
    m_actionCount.store(0, std::memory_order_relaxed);
}

void UltrakillHudModule::onDisable() {
    // Clear this module's last submitted overlay immediately when it is switched off.
    const std::vector<PLModMenu_DrawCommand> empty;
    submitDrawCommands(moduleId, empty);
}

void UltrakillHudModule::onFrame() {
    if (!enabled) return;

    std::vector<PLModMenu_DrawCommand> cmds;
    cmds.reserve(48);

    // Keep text storage alive until submitDrawCommands has consumed the commands.
    std::vector<std::string> textStore;
    textStore.reserve(16);

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
        cmds.push_back(cmd);
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
        cmds.push_back(cmd);
    };

    auto addCircle = [&](float x, float y, float radius, std::uint32_t color) {
        PLModMenu_DrawCommand cmd = {};
        cmd.type = PL_DRAW_CIRCLE_FILLED;
        cmd.x = x;
        cmd.y = y;
        cmd.size = radius;
        cmd.color = color;
        cmds.push_back(cmd);
    };

    auto addText = [&](float x, float y, const std::string& text,
                       float size, std::uint32_t color) {
        textStore.push_back(text);
        PLModMenu_DrawCommand cmd = {};
        cmd.type = PL_DRAW_TEXT;
        cmd.x = x;
        cmd.y = y;
        cmd.w = 0.0f;
        cmd.h = 0.0f;
        cmd.color = color;
        cmd.size = size;
        cmd.text = textStore.back().c_str();
        cmds.push_back(cmd);
    };

    const float x = hudPosX;
    const float y = hudPosY;

    if (m_showStatusPanel) {
        constexpr float panelW = 252.0f;
        constexpr float panelH = 100.0f;

        addRect(x, y, panelW, panelH, kOutline, 3.0f);
        addRect(x + 1.0f, y + 1.0f, panelW - 2.0f, panelH - 2.0f, kPanel, 3.0f);
        addRect(x + 2.0f, y + 2.0f, 82.0f, 2.0f, kRed);
        addRect(x + 84.0f, y + 2.0f, panelW - 86.0f, 2.0f, kCyan);

        addText(x + 10.0f, y + 8.0f, "ULTRAKILL // V1", 12.0f, kCyan);
        addText(x + 10.0f, y + 24.0f, "COMBAT INTERFACE", 8.0f, kRed);
        addLine(x + 10.0f, y + 35.0f, panelW - 20.0f, 0.0f, 1.0f, kOutline);

        addText(x + 10.0f, y + 41.0f, "VITALS", 8.0f, kWhite);
        addText(x + 10.0f, y + 63.0f, "CHARGE", 8.0f, kWhite);

        // Decorative segmented bars; they are deliberately not presented as live player stats.
        for (int i = 0; i < 14; ++i) {
            const float segmentX = x + 70.0f + static_cast<float>(i) * 12.0f;
            const std::uint32_t vitalsColor = i < 11 ? kRed : kDim;
            const std::uint32_t chargeColor = i < 9 ? kCyan : kDim;
            addRect(segmentX, y + 42.0f, 9.0f, 6.0f, vitalsColor, 1.0f);
            addRect(segmentX, y + 64.0f, 9.0f, 6.0f, chargeColor, 1.0f);
        }

        addText(x + 10.0f, y + 84.0f, "VISUAL PREVIEW // NOT LIVE STATS", 7.0f, kMuted);
    }

    if (m_showStyleCounter) {
        const float cardY = y + (m_showStatusPanel ? 108.0f : 0.0f);
        const std::uint32_t actions = m_actionCount.load(std::memory_order_relaxed);
        const std::uint64_t score = std::min<std::uint64_t>(
            static_cast<std::uint64_t>(actions) * 250ULL, 999999ULL);

        const char* rank = "D";
        if (score >= 9000ULL) rank = "S";
        else if (score >= 5000ULL) rank = "A";
        else if (score >= 2500ULL) rank = "B";
        else if (score >= 1000ULL) rank = "C";

        addRect(x, cardY, 206.0f, 52.0f, kOutline, 3.0f);
        addRect(x + 1.0f, cardY + 1.0f, 204.0f, 50.0f, kPanel, 3.0f);
        addRect(x + 2.0f, cardY + 2.0f, 42.0f, 2.0f, kRed);
        addText(x + 10.0f, cardY + 8.0f, "STYLE RANK", 8.0f, kMuted);

        const std::string scoreText = std::string(rank) + "   /   " + std::to_string(score);
        addText(x + 10.0f, cardY + 20.0f, scoreText, 15.0f, kRed);
        addText(x + 10.0f, cardY + 39.0f, "ATTACK EVENTS, NOT HIT CONFIRMATION", 6.5f, kMuted);
    }

    if (m_showParryWidget) {
        float parryY = y;
        if (m_showStatusPanel) parryY += 108.0f;
        if (m_showStyleCounter) parryY += 60.0f;
        addRect(x, parryY, 190.0f, 52.0f, kOutline, 3.0f);
        addRect(x + 1.0f, parryY + 1.0f, 188.0f, 50.0f, kPanelDark, 3.0f);
        addRect(x + 2.0f, parryY + 2.0f, 188.0f, 2.0f, kCyan);

        // This is a visual mock button only. No input is sent and no parry is performed.
        addCircle(x + 22.0f, parryY + 27.0f, 12.0f, kRed);
        addCircle(x + 22.0f, parryY + 27.0f, 8.0f, kPanelDark);
        addCircle(x + 22.0f, parryY + 27.0f, 3.0f, kCyan);
        addText(x + 42.0f, parryY + 11.0f, "PARRY", 10.0f, kWhite);
        addText(x + 42.0f, parryY + 28.0f, "VISUAL ONLY", 7.0f, kCyan);
        for (int i = 0; i < 5; ++i) {
            addRect(x + 120.0f + static_cast<float>(i) * 11.0f, parryY + 29.0f,
                    7.0f, 5.0f, i < 3 ? kCyan : kDim, 1.0f);
        }
    }

    if (m_showReticle) {
        // LeviLauncher's draw layer supports a screen-centre anchor at (-20000, -20000).
        constexpr float cx = -20000.0f;
        constexpr float cy = -20000.0f;
        addLine(cx, cy, -12.0f, 0.0f, 3.0f, kRed);
        addLine(cx, cy,  12.0f, 0.0f, 3.0f, kRed);
        addLine(cx, cy, 0.0f, -12.0f, 3.0f, kRed);
        addLine(cx, cy, 0.0f,  12.0f, 3.0f, kRed);
        addLine(cx, cy, -8.0f, 0.0f, 1.5f, kCyan);
        addLine(cx, cy,  8.0f, 0.0f, 1.5f, kCyan);
        addLine(cx, cy, 0.0f, -8.0f, 1.5f, kCyan);
        addLine(cx, cy, 0.0f,  8.0f, 1.5f, kCyan);
        addCircle(cx, cy, 2.0f, kWhite);
    }

    submitDrawCommands(moduleId, cmds);
}

void UltrakillHudModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    if (j.contains("hudPosX")) hudPosX = j["hudPosX"].get<float>();
    if (j.contains("hudPosY")) hudPosY = j["hudPosY"].get<float>();
    if (j.contains("isHudModule")) isHudModule = j["isHudModule"].get<bool>();
    if (j.contains("m_showStatusPanel")) m_showStatusPanel = j["m_showStatusPanel"].get<bool>();
    if (j.contains("m_showReticle")) m_showReticle = j["m_showReticle"].get<bool>();
    if (j.contains("m_showStyleCounter")) m_showStyleCounter = j["m_showStyleCounter"].get<bool>();
    if (j.contains("m_showParryWidget")) m_showParryWidget = j["m_showParryWidget"].get<bool>();
}

void UltrakillHudModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["hudPosX"] = hudPosX;
    j["hudPosY"] = hudPosY;
    j["isHudModule"] = isHudModule;
    j["m_showStatusPanel"] = m_showStatusPanel;
    j["m_showReticle"] = m_showReticle;
    j["m_showStyleCounter"] = m_showStyleCounter;
    j["m_showParryWidget"] = m_showParryWidget;
}
