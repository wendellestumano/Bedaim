#include "esp.hpp"
#include "esp_assets.hpp"
#include "hitbox.hpp"
#include "../ModuleRegistry.hpp"
#include "../player/aimassist.hpp"
#include "core/Diagnostics.hpp"
#include <pl/ModMenu.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <limits>
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
float normalizeDegrees(float value){
 while(value>180.0f)value-=360.0f;
 while(value< -180.0f)value+=360.0f;
 return value;
}
}

EspModule::EspModule():Module("ESP","Configurable player boxes and tracers."){showInMenu=true;}

void EspModule::onInit(){
    bedaim::espassets::registerImages();
    bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Info,"ESP",
        "hud_assets=registered; standard box/tracer ESP enabled; off-screen locator retired");
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
    pl::modmenu::submitDrawCommands(moduleId,std::span<const pl::modmenu::DrawCommand>{});
    std::lock_guard<std::mutex> lock(m_hudTargetMutex);
    m_offscreenTargetValid=false;
    m_offscreenFrameTime={};
}

void EspModule::beginOffscreenFrame(const bedrocktools::sdk::Vec3& localPosition,
                                    const bedrocktools::sdk::Vec2& localRotation){
    std::lock_guard<std::mutex> lock(m_hudTargetMutex);
    m_hudLocalPosition=localPosition;
    m_hudLocalRotation=localRotation;
    m_offscreenTargetDistance=std::numeric_limits<float>::max();
    m_offscreenTargetValid=false;
    m_offscreenFrameTime=std::chrono::steady_clock::now();
}

void EspModule::considerOffscreenCandidate(const bedrocktools::sdk::Vec3& targetCenter,float distance){
    if(!std::isfinite(targetCenter.x)||!std::isfinite(targetCenter.y)||!std::isfinite(targetCenter.z)||
       !std::isfinite(distance)||distance<0.0f||distance>std::clamp(range,8.0f,80.0f))return;
    std::lock_guard<std::mutex> lock(m_hudTargetMutex);
    if(m_offscreenFrameTime.time_since_epoch().count()==0)return;

    const float dx=targetCenter.x-m_hudLocalPosition.x;
    const float dz=targetCenter.z-m_hudLocalPosition.z;
    const float dy=targetCenter.y-(m_hudLocalPosition.y+1.62f);
    const float horizontal=std::sqrt(dx*dx+dz*dz);
    if(horizontal<0.001f)return;
    constexpr float kRadToDeg=57.29577951308232f;
    constexpr float kMaxVerticalFov=110.0f;
    const float targetYaw=std::atan2(-dx,dz)*kRadToDeg;
    const float yawDelta=normalizeDegrees(targetYaw-m_hudLocalRotation.y);
    const float targetPitch=-std::atan2(dy,horizontal)*kRadToDeg;
    const float pitchDelta=targetPitch-m_hudLocalRotation.x;

    // Use a high-end viewport bound, rather than the old 42°/28° thresholds,
    // so the locator waits until the player's bearing is genuinely off-screen.
    const float halfVertical=kMaxVerticalFov*0.5f+1.5f;
    const float aspect=std::clamp(m_hudViewportAspect,0.5f,3.2f);
    const float halfHorizontal=std::atan(std::tan(kMaxVerticalFov*0.5f/kRadToDeg)*aspect)*kRadToDeg+2.0f;
    if(std::fabs(yawDelta)<=halfHorizontal&&std::fabs(pitchDelta)<=halfVertical)return;
    if(m_offscreenTargetValid&&distance>=m_offscreenTargetDistance)return;
    m_offscreenTargetCenter=targetCenter;
    m_offscreenTargetDistance=distance;
    m_offscreenTargetValid=true;
}

bool EspModule::offscreenTargetSnapshot(bedrocktools::sdk::Vec3& targetCenter,
                                        bedrocktools::sdk::Vec3& localPosition,
                                        bedrocktools::sdk::Vec2& localRotation,
                                        float& distance) const {
    std::lock_guard<std::mutex> lock(m_hudTargetMutex);
    const auto now=std::chrono::steady_clock::now();
    if(!m_offscreenTargetValid||m_offscreenFrameTime.time_since_epoch().count()==0||
       now<m_offscreenFrameTime||now-m_offscreenFrameTime>std::chrono::milliseconds(250))return false;
    targetCenter=m_offscreenTargetCenter;
    localPosition=m_hudLocalPosition;
    localRotation=m_hudLocalRotation;
    distance=m_offscreenTargetDistance;
    return true;
}

void EspModule::onFrame(){
    if (!enabled) return;
    pl::modmenu::submitDrawCommands(moduleId, std::span<const pl::modmenu::DrawCommand>{});
}

void EspModule::onMenuRegistered(){
 using namespace pl::modmenu;ConfigSchemaBuilder schema;nlohmann::json config;saveConfig(config);
 schema.defaultCategory("players").category("players","Players","Player outline visibility and distance")
  .category("appearance","Appearance","Box and tracer colors.");
 auto toggle=[&](const char* key,const char* title,const char* desc,bool d,const char* category="players"){
  ConfigNodeV2 n;n.id=key;n.key=key;n.title=title;n.description=desc;n.category=category;n.type=ConfigControlTypeV2::Toggle;
  n.defaultValue=d?"true":"false";n.currentValue=cfgText(config,key,n.defaultValue.c_str());n.hasCurrentValue=true;schema.node(std::move(n));
 };
 toggle("showPlayers","Show players","Draw outlines around other players.",true);
 toggle("showInvisible","Show invisible players","Include invisible players in ESP.",false);
 toggle("showBoxes","Player boxes","Draw a 3D bounding box around each player.",true);
 toggle("showTracers","Tracers","Draw a line from the camera to each player.",true);
 toggle("showLockInAim","Target ring","Draw a ring above each in-range player.",false);
 ConfigNodeV2 r;r.id="range";r.key="range";r.title="ESP range";r.description="Maximum range for ESP outlines and direction indicators.";
 r.category="players";r.type=ConfigControlTypeV2::SliderFloat;r.defaultValue="30";r.currentValue=cfgText(config,"range","30");
 r.hasCurrentValue=true;r.minValue="8";r.maxValue="80";r.step="1";r.unit=" blocks";schema.node(std::move(r));
 auto color=[&](const char* key,const char* title){
  ConfigNodeV2 n;n.id=key;n.key=key;n.title=title;n.description="HEX color: #AARRGGBB (or #RRGGBB).";
  n.category="appearance";n.type=ConfigControlTypeV2::Text;n.defaultValue="#FFFFFFFF";
  n.currentValue=cfgText(config,key,"#FFFFFFFF");n.hasCurrentValue=true;
  n.placeholder="#AARRGGBB";n.maxLength=9;schema.node(std::move(n));
 };
 color("outlineColor","Box color");
 color("tracerColor","Tracer color");
 setConfigSchemaJson(moduleId,schema.toJson());
}
bool EspModule::onMenuConfigChanged(std::string_view key,std::string_view value){
 if(key=="showPlayers"||key=="showInvisible"||key=="showBoxes"||key=="showTracers"||key=="showLockInAim"){
  bool b=false;if(!parseBool(value,b))return false;
  if(key=="showPlayers")showPlayers=b;
  else if(key=="showInvisible")showInvisible=b;
  else if(key=="showBoxes")showBoxes=b;
  else if(key=="showTracers")showTracers=b;
  else showLockInAim=b;
  return true;
 }
 if(key=="range"){float f=0;if(!parseFloat(value,f))return false;range=std::clamp(f,8.0f,80.0f);return true;}
 std::uint32_t c=0;
 if(key=="outlineColor"&&parseColor(value,c)){outlineColor=c;return true;}
 if(key=="tracerColor"&&parseColor(value,c)){tracerColor=c;return true;}
 return false;
}
void EspModule::loadConfig(const nlohmann::json& j){
 Module::loadConfig(j);showPlayers=j.value("showPlayers",showPlayers);showInvisible=j.value("showInvisible",showInvisible);
 showBoxes=j.value("showBoxes",showBoxes);showTracers=j.value("showTracers",showTracers);
 showLockInAim=j.value("showLockInAim",showLockInAim);
 range=std::clamp(j.value("range",range),8.0f,80.0f);
 // Migrate old per-mode config to its previous Survival color, without querying game modes.
 if(j.contains("outlineColor"))loadColor(j,"outlineColor",outlineColor);
 else loadColor(j,"survivalColor",outlineColor);
 loadColor(j,"tracerColor",tracerColor);
}
void EspModule::saveConfig(nlohmann::json& j){
 Module::saveConfig(j);j["showPlayers"]=showPlayers;j["showInvisible"]=showInvisible;
 j["showBoxes"]=showBoxes;j["showTracers"]=showTracers;j["showLockInAim"]=showLockInAim;
 j["range"]=std::clamp(range,8.0f,80.0f);
 char color[12];std::snprintf(color,sizeof(color),"#%08X",outlineColor);
 j["outlineColor"]=std::string(color);
 std::snprintf(color,sizeof(color),"#%08X",tracerColor);j["tracerColor"]=std::string(color);
}

[executed on device: localhost (e1d4d221-df7a-4b7f-8115-0ecb8c62d73c)]