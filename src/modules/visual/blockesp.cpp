#include "blockesp.hpp"
#include "../core/Diagnostics.hpp"
#include <pl/ModMenu.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>

namespace {
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
bool parseFloat(std::string_view value, float& out) {
    try {
        std::string s(value); std::size_t n = 0; float f = std::stof(s, &n);
        while (n < s.size() && std::isspace(static_cast<unsigned char>(s[n]))) ++n;
        if (n != s.size() || !std::isfinite(f)) return false;
        out = f; return true;
    } catch (...) { return false; }
}
bool parseColor(std::string_view value, std::uint32_t& out) {
    std::string s(value); if (!s.empty() && s.front() == '#') s.erase(s.begin());
    if (s.size() != 6 && s.size() != 8) return false;
    if (!std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isxdigit(c) != 0; })) return false;
    try { auto n = static_cast<std::uint32_t>(std::stoul(s, nullptr, 16)); out = s.size() == 6 ? (0xFF000000u | n) : n; return true; }
    catch (...) { return false; }
}
void loadColor(const nlohmann::json& j, const char* key, std::uint32_t& value) {
    if (!j.contains(key) || !j[key].is_string()) return;
    std::uint32_t parsed = 0;
    if (parseColor(j[key].get<std::string>(), parsed)) value = parsed;
}
}
BlockEspModule::BlockEspModule() : Module("BlockESP", "Player box ESP, independent from the locator arrow and target ring.") { showInMenu = true; }

void BlockEspModule::onMenuRegistered() {
    using namespace pl::modmenu;
    ConfigSchemaBuilder schema; nlohmann::json config; saveConfig(config);
    schema.defaultCategory("players")
        .category("players", "Players", "BlockESP visibility and range")
        .category("appearance", "Box appearance", "2D and 3D player-box rendering")
        .category("colors", "Game-mode colors", "ARGB colors used for player boxes");
    auto toggle = [&](const char* key, const char* title, const char* desc, bool def, const char* category) {
        ConfigNodeV2 n; n.id=key; n.key=key; n.title=title; n.description=desc; n.category=category;
        n.type=ConfigControlTypeV2::Toggle; n.defaultValue=def?"true":"false";
        n.currentValue=configText(config,key,n.defaultValue.c_str()); n.hasCurrentValue=true; schema.node(std::move(n));
    };
    auto slider = [&](const char* key, const char* title, const char* desc, const char* category,
                      float def, float min, float max, float step, const char* unit) {
        ConfigNodeV2 n; n.id=key; n.key=key; n.title=title; n.description=desc; n.category=category;
        n.type=ConfigControlTypeV2::SliderFloat; n.defaultValue=std::to_string(def);
        n.currentValue=configText(config,key,n.defaultValue.c_str()); n.hasCurrentValue=true;
        n.minValue=std::to_string(min); n.maxValue=std::to_string(max); n.step=std::to_string(step); n.unit=unit;
        schema.node(std::move(n));
    };
    toggle("showPlayers","Show players","Draw box ESP around other players.",true,"players");
    toggle("showInvisible","Show invisible players","Include invisible players in BlockESP.",false,"players");
    toggle("infiniteRange","Infinite BlockESP range","Include all loaded players in box ESP.",true,"players");
    toggle("box2D","2D ESP box","Render a camera-facing 2D box. Off renders a 3D world-space box.",false,"appearance");
    slider("range","BlockESP range","Maximum box ESP distance when infinite range is off.","players",30.0f,8.0f,80.0f,1.0f," blocks");
    slider("box2DThickness","2D boxes thickness","Stroke width for the 2D player box.","appearance",2.0f,1.0f,10.0f,1.0f," px");
    slider("projectionFov","2D projection FOV","Vertical FOV used to scale the 2D box with camera depth.","appearance",70.0f,40.0f,120.0f,1.0f,"°");
    slider("outlineThickness","3D box thickness","World-space outline thickness for 3D player boxes.","appearance",0.025f,0.0f,0.10f,0.005f," blocks");
    auto color = [&](const char* key, const char* title) {
        ConfigNodeV2 n; n.id=key; n.key=key; n.title=title; n.description="HEX color: #AARRGGBB (or #RRGGBB).";
        n.category="colors"; n.type=ConfigControlTypeV2::Text; n.defaultValue="";
        n.currentValue=configText(config,key,""); n.hasCurrentValue=true; n.placeholder="#AARRGGBB"; n.maxLength=9;
        schema.node(std::move(n));
    };
    color("survivalColor","Survival color"); color("creativeColor","Creative color");
    color("spectatorColor","Spectator color"); color("adventureColor","Adventure color"); color("unknownColor","Unknown-mode color");
    setConfigSchemaJson(moduleId,schema.toJson());
}

bool BlockEspModule::onMenuConfigChanged(std::string_view key, std::string_view value) {
    if (key=="showPlayers" || key=="showInvisible" || key=="infiniteRange" || key=="box2D") {
        bool b=false; if (!parseBool(value,b)) return false;
        if (key=="showPlayers") showPlayers=b;
        else if (key=="showInvisible") showInvisible=b;
        else if (key=="infiniteRange") infiniteRange=b;
        else box2D=b;
        return true;
    }
    if (key=="range" || key=="box2DThickness" || key=="projectionFov" || key=="outlineThickness") {
        float f=0; if (!parseFloat(value,f)) return false;
        if (key=="range") range=std::clamp(f,8.0f,80.0f);
        else if (key=="box2DThickness") box2DThickness=std::clamp(f,1.0f,10.0f);
        else if (key=="projectionFov") projectionFov=std::clamp(f,40.0f,120.0f);
        else outlineThickness=std::clamp(f,0.0f,0.10f);
        return true;
    }
    std::uint32_t c=0; if (!parseColor(value,c)) return false;
    if (key=="survivalColor") survivalColor=c;
    else if (key=="creativeColor") creativeColor=c;
    else if (key=="spectatorColor") spectatorColor=c;
    else if (key=="adventureColor") adventureColor=c;
    else if (key=="unknownColor") unknownColor=c;
    else return false;
    return true;
}
void BlockEspModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    showPlayers=j.value("showPlayers",showPlayers); showInvisible=j.value("showInvisible",showInvisible);
    infiniteRange=j.value("infiniteRange",infiniteRange); box2D=j.value("box2D",box2D);
    range=std::clamp(j.value("range",range),8.0f,80.0f);
    box2DThickness=std::clamp(j.value("box2DThickness",box2DThickness),1.0f,10.0f);
    projectionFov=std::clamp(j.value("projectionFov",projectionFov),40.0f,120.0f);
    outlineThickness=std::clamp(j.value("outlineThickness",outlineThickness),0.0f,0.10f);
    loadColor(j,"survivalColor",survivalColor); loadColor(j,"creativeColor",creativeColor);
    loadColor(j,"spectatorColor",spectatorColor); loadColor(j,"adventureColor",adventureColor); loadColor(j,"unknownColor",unknownColor);
}
void BlockEspModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["showPlayers"]=showPlayers; j["showInvisible"]=showInvisible; j["infiniteRange"]=infiniteRange; j["box2D"]=box2D;
    j["range"]=std::clamp(range,8.0f,80.0f); j["box2DThickness"]=std::clamp(box2DThickness,1.0f,10.0f);
    j["projectionFov"]=std::clamp(projectionFov,40.0f,120.0f); j["outlineThickness"]=std::clamp(outlineThickness,0.0f,0.10f);
    char color[12]; auto save=[&](const char* key,std::uint32_t c){std::snprintf(color,sizeof(color),"#%08X",c);j[key]=std::string(color);};
    save("survivalColor",survivalColor); save("creativeColor",creativeColor); save("spectatorColor",spectatorColor);
    save("adventureColor",adventureColor); save("unknownColor",unknownColor);
}
