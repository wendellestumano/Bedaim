#include "esp.hpp"
#include <algorithm>
#include <cstdio>
#include <string>

EspModule::EspModule()
    : Module("ESP", "Draws player outlines and colors them by detected game mode.") {
    showInMenu = true;
}

static void loadEspColor(const nlohmann::json& j, const char* key, uint32_t& color) {
    if (!j.contains(key) || !j[key].is_string()) return;
    std::string value = j[key].get<std::string>();
    if (!value.empty() && value.front() == '#') value.erase(value.begin());
    try {
        const auto parsed = std::stoul(value, nullptr, 16);
        if (value.size() <= 8) color = static_cast<uint32_t>(parsed);
    } catch (...) {}
}

void EspModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    showPlayers = j.value("showPlayers", showPlayers);
    showInvisible = j.value("showInvisible", showInvisible);
    range = std::clamp(j.value("range", range), 8.0f, 80.0f);
    loadEspColor(j, "survivalColor", survivalColor);
    loadEspColor(j, "creativeColor", creativeColor);
    loadEspColor(j, "spectatorColor", spectatorColor);
    loadEspColor(j, "adventureColor", adventureColor);
    loadEspColor(j, "unknownColor", unknownColor);
}

void EspModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["showPlayers"] = showPlayers;
    j["showInvisible"] = showInvisible;
    j["range"] = std::clamp(range, 8.0f, 80.0f);

    char color[12];
    auto saveColor = [&](const char* key, uint32_t value) {
        std::snprintf(color, sizeof(color), "#%08X", value);
        j[key] = std::string(color);
    };
    saveColor("survivalColor", survivalColor);
    saveColor("creativeColor", creativeColor);
    saveColor("spectatorColor", spectatorColor);
    saveColor("adventureColor", adventureColor);
    saveColor("unknownColor", unknownColor);
}
