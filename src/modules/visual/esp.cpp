#include "esp.hpp"
#include "hitbox.hpp"
#include "../ModuleRegistry.hpp"
#include "core/Diagnostics.hpp"
#include <pl/ModMenu.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {
std::string cfgText(const nlohmann::json& j,const char* key,const char* fallback){
 auto it=j.find(key);if(it==j.end()||it->is_null())return fallback;
 if(it->is_string())return it->get<std::string>();if(it->is_boolean())return it->get<bool>()?"true":"false";return it->dump();
}
bool parseBool(std::string_view v,bool& out){
 if(v=="true"||v=="1"||v=="on"){out=true;return true;}
 if(v=="false"||v=="0"||v=="off"){out=false;return true;}return false;
}
bool parseFloat(std::string_view v,float& out){
 try{std::string s(v);std::size_t n=0;float f=std::stof(s,&n);
 while(n<s.size()&&std::isspace(static_cast<unsigned char>(s[n])))++n;
 if(n!=s.size()||!std::isfinite(f))return false;out=f;return true;}catch(...){return false;}
}
bool parseColor(std::string_view v,std::uint32_t& out){
 std::string s(v);if(!s.empty()&&s.front()=='#')s.erase(s.begin());
 if((s.size()!=6&&s.size()!=8)||!std::all_of(s.begin(),s.end(),[](unsigned char c){return std::isxdigit(c)!=0;}))return false;
 try{auto x=static_cast<std::uint32_t>(std::stoul(s,nullptr,16));out=s.size()==6?(0xFF000000u|x):x;return true;}catch(...){return false;}
}
void loadColor(const nlohmann::json& j,const char* key,std::uint32_t& v){
 if(!j.contains(key)||!j[key].is_string())return;std::uint32_t parsed=0;if(parseColor(j[key].get<std::string>(),parsed))v=parsed;
}
}

EspModule::EspModule():Module("ESP","Player boxes, tracers and target ring."){showInMenu=true;}

void EspModule::onInit(){
    bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Info,"ESP",
        "render_mode=world_aabb_boxes; tracers=available; offscreen_locator=retired");
}

void EspModule::onEnable() {
    auto* hitbox = static_cast<HitboxModule*>(ModuleRegistry::get().find("bedrocktools.Hitbox"));
    if (hitbox) {
        hitbox->ensureRenderHook();
    } else {
        bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Warning, "ESP",
            "renderlevel_hook=unavailable; Hitbox renderer module was not registered");
    }
}

void EspModule::onDisable(){
    {
        std::lock_guard<std::mutex> lock(m_projectedMutex);
        m_projectedBoxes.clear();
        m_projectedFrameTime = {};
    }
    pl::modmenu::submitDrawCommands(moduleId,std::span<const pl::modmenu::DrawCommand>{});
}

void EspModule::publishProjectedBoxes(std::vector<ProjectedBox> boxes){
    std::lock_guard<std::mutex> lock(m_projectedMutex);
    m_projectedBoxes = std::move(boxes);
    m_projectedFrameTime = std::chrono::steady_clock::now();
}

void EspModule::onFrame(){
    using namespace pl::modmenu;
    if (!enabled) {
        submitDrawCommands(moduleId,std::span<const DrawCommand>{});
        return;
    }
    const auto surface = getHudSurfaceSize();
    std::vector<ProjectedBox> boxes;
    {
        std::lock_guard<std::mutex> lock(m_projectedMutex);
        const auto now = std::chrono::steady_clock::now();
        if (m_projectedFrameTime.time_since_epoch().count() != 0 && now >= m_projectedFrameTime &&
            now - m_projectedFrameTime <= std::chrono::milliseconds(250)) boxes = m_projectedBoxes;
    }
    std::vector<DrawCommand> commands;
    commands.reserve(boxes.size() * (showTracers ? 5u : 4u));
    for (const auto& box : boxes) {
        if (!std::isfinite(box.left) || !std::isfinite(box.top) || !std::isfinite(box.right) ||
            !std::isfinite(box.bottom) || box.right - box.left < 1.0f || box.bottom - box.top < 1.0f) continue;
        auto line = [&](float x, float y, float dx, float dy) {
            DrawCommand c{}; c.type = DrawCommandType::Line; c.x = x; c.y = y; c.w = dx; c.h = dy;
            c.color = box.color; c.size = 1.6f; commands.push_back(c);
        };
        line(box.left, box.top, box.right - box.left, 0.0f);
        line(box.right, box.top, 0.0f, box.bottom - box.top);
        line(box.right, box.bottom, box.left - box.right, 0.0f);
        line(box.left, box.bottom, 0.0f, box.top - box.bottom);
        if (showTracers && std::isfinite(surface.width) && std::isfinite(surface.height) &&
            surface.width > 0.0f && surface.height > 0.0f) {
            DrawCommand c{}; c.type = DrawCommandType::Line;
            c.x = surface.width * 0.5f; c.y = surface.height - 2.0f;
            c.w = (box.left + box.right) * 0.5f - c.x; c.h = box.bottom - c.y;
            c.color = box.color; c.size = 1.2f; commands.push_back(c);
        }
    }
    submitDrawCommands(moduleId,commands);
}

void EspModule::onMenuRegistered(){
 using namespace pl::modmenu;ConfigSchemaBuilder schema;nlohmann::json config;saveConfig(config);
 schema.defaultCategory("players").category("players","Players","Player outline visibility and distance")
  .category("indicators","Visual indicators","Target ring and player tracers")
  .category("colors","Game-mode colors","Hexadecimal ARGB outline colors");
 auto toggle=[&](const char* key,const char* title,const char* desc,bool d,const char* category="players"){
  ConfigNodeV2 n;n.id=key;n.key=key;n.title=title;n.description=desc;n.category=category;n.type=ConfigControlTypeV2::Toggle;
  n.defaultValue=d?"true":"false";n.currentValue=cfgText(config,key,n.defaultValue.c_str());n.hasCurrentValue=true;schema.node(std::move(n));
 };
 toggle("showPlayers","Show players","Draw outlines around other players.",true);
 toggle("showInvisible","Show invisible players","Include invisible players in ESP.",false);
 toggle("showLockInAim","Target ring","Draw an animated ring above each in-range player, independent of Aim Assist.",true,"indicators");
 toggle("showTracers","Tracers","Draw a line from the camera to each in-range player.",true,"indicators");
 toggle("animatedIndicators","Animated indicators","Animate the target ring.",true,"indicators");
 ConfigNodeV2 size;size.id="indicatorSize";size.key="indicatorSize";size.title="Indicator size";
 size.description="Size of the target ring above players.";size.category="indicators";
 size.type=ConfigControlTypeV2::SliderFloat;size.defaultValue="48";size.currentValue=cfgText(config,"indicatorSize","48");
 size.hasCurrentValue=true;size.minValue="24";size.maxValue="96";size.step="2";size.unit=" px";schema.node(std::move(size));
 ConfigNodeV2 r;r.id="range";r.key="range";r.title="ESP range";r.description="Maximum distance for player boxes, tracers and target rings.";
 r.category="players";r.type=ConfigControlTypeV2::SliderFloat;r.defaultValue="30";r.currentValue=cfgText(config,"range","30");
 r.hasCurrentValue=true;r.minValue="8";r.maxValue="80";r.step="1";r.unit=" blocks";schema.node(std::move(r));
 auto color=[&](const char* key,const char* title){
  ConfigNodeV2 n;n.id=key;n.key=key;n.title=title;n.description="HEX color: #AARRGGBB (or #RRGGBB).";
  n.category="colors";n.type=ConfigControlTypeV2::Text;n.defaultValue="";n.currentValue=cfgText(config,key,"");
  n.hasCurrentValue=true;n.placeholder="#AARRGGBB";n.maxLength=9;schema.node(std::move(n));
 };
 color("survivalColor","Survival color");color("creativeColor","Creative color");color("spectatorColor","Spectator color");
 color("adventureColor","Adventure color");color("unknownColor","Unknown-mode color");
 setConfigSchemaJson(moduleId,schema.toJson());
}
bool EspModule::onMenuConfigChanged(std::string_view key,std::string_view value){
 if(key=="showPlayers"||key=="showInvisible"||key=="showLockInAim"||
    key=="showTracers"||key=="animatedIndicators"){
  bool b=false;if(!parseBool(value,b))return false;
  if(key=="showPlayers")showPlayers=b;
  else if(key=="showInvisible")showInvisible=b;
  else if(key=="showLockInAim")showLockInAim=b;
  else if(key=="showTracers")showTracers=b;
  else animatedIndicators=b;
  return true;
 }
 if(key=="range"){float f=0;if(!parseFloat(value,f))return false;range=std::clamp(f,8.0f,80.0f);return true;}
 if(key=="indicatorSize"){float f=0;if(!parseFloat(value,f))return false;indicatorSize=std::clamp(f,24.0f,96.0f);return true;}
 std::uint32_t c=0;if(!parseColor(value,c))return false;
 if(key=="survivalColor")survivalColor=c;else if(key=="creativeColor")creativeColor=c;
 else if(key=="spectatorColor")spectatorColor=c;else if(key=="adventureColor")adventureColor=c;
 else if(key=="unknownColor")unknownColor=c;else return false;return true;
}
void EspModule::loadConfig(const nlohmann::json& j){
 Module::loadConfig(j);showPlayers=j.value("showPlayers",showPlayers);showInvisible=j.value("showInvisible",showInvisible);
 showLockInAim=j.value("showLockInAim",showLockInAim);
 showTracers=j.value("showTracers",showTracers);
 animatedIndicators=j.value("animatedIndicators",animatedIndicators);
 indicatorSize=std::clamp(j.value("indicatorSize",indicatorSize),24.0f,96.0f);
 range=std::clamp(j.value("range",range),8.0f,80.0f);
 loadColor(j,"survivalColor",survivalColor);loadColor(j,"creativeColor",creativeColor);
 loadColor(j,"spectatorColor",spectatorColor);loadColor(j,"adventureColor",adventureColor);loadColor(j,"unknownColor",unknownColor);
}
void EspModule::saveConfig(nlohmann::json& j){
 Module::saveConfig(j);j["showPlayers"]=showPlayers;j["showInvisible"]=showInvisible;
 j["showLockInAim"]=showLockInAim;j["showTracers"]=showTracers;
 j["animatedIndicators"]=animatedIndicators;j["indicatorSize"]=std::clamp(indicatorSize,24.0f,96.0f);
 j["range"]=std::clamp(range,8.0f,80.0f);
 char color[12];auto save=[&](const char* key,std::uint32_t v){std::snprintf(color,sizeof(color),"#%08X",v);j[key]=std::string(color);};
 save("survivalColor",survivalColor);save("creativeColor",creativeColor);save("spectatorColor",spectatorColor);
 save("adventureColor",adventureColor);save("unknownColor",unknownColor);
}
