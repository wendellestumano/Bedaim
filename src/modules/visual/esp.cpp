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

EspModule::EspModule():Module("ESP","Player outlines, Lock in Aim and off-screen direction indicators."){showInMenu=true;}

void EspModule::onInit(){
    bedaim::espassets::registerImages();
    bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Info,"ESP",
        "hud_assets=registered; lock-in sprite and 16 directional arrow rotations");
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

    // Use a conservative full-viewport limit (110-degree vertical FOV ceiling),
    // scaled by the real HUD aspect ratio. This deliberately errs toward showing
    // an indicator late rather than while a player is still visible.
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
    if(!enabled)return;
    const auto surface=pl::modmenu::getHudSurfaceSize();
    if(!std::isfinite(surface.width)||!std::isfinite(surface.height)||surface.width<64.0f||surface.height<64.0f){
        pl::modmenu::submitDrawCommands(moduleId,std::span<const pl::modmenu::DrawCommand>{});
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_hudTargetMutex);
        m_hudViewportAspect=std::clamp(surface.width/surface.height,0.5f,3.2f);
    }
    std::vector<pl::modmenu::DrawCommand> commands;
    commands.reserve(2);
    const float cx=surface.width*0.5f, cy=surface.height*0.5f;
    const auto now=std::chrono::steady_clock::now();
    const float seconds=std::chrono::duration<float>(now.time_since_epoch()).count();

    // Target rings are rendered in world space above each player by the shared RenderLevel hook.
    // Do not draw a fixed screen-center reticle here: it is misleading when Aim Assist is idle.

    if(showOffscreenIndicators){
        bedrocktools::sdk::Vec3 target{},local{};
        bedrocktools::sdk::Vec2 rotation{};
        float distance=0.0f;
        if(offscreenTargetSnapshot(target,local,rotation,distance)){
            float dx=target.x-local.x;
            float dy=target.y-(local.y+1.62f);
            float dz=target.z-local.z;
            const float worldLength=std::sqrt(dx*dx+dy*dy+dz*dz);
            if(worldLength>0.001f){
                dx/=worldLength;dy/=worldLength;dz/=worldLength;
                constexpr float kDegToRad=0.01745329251994329577f;
                const float yaw=rotation.y*kDegToRad;
                const float pitch=rotation.x*kDegToRad;

                // Project the world-space target direction onto camera right/up axes.
                // The old formula had the horizontal sign reversed and added a fixed
                // downward bias to rear targets, making the arrow point away from players.
                const float rightX=std::cos(yaw);
                const float rightZ=std::sin(yaw);
                const float upX=-std::sin(pitch)*std::sin(yaw);
                const float upY=std::cos(pitch);
                const float upZ=std::sin(pitch)*std::cos(yaw);
                float dirX=dx*rightX+dz*rightZ;
                float dirY=-(dx*upX+dy*upY+dz*upZ);
                const float directionLength=std::sqrt(dirX*dirX+dirY*dirY);
                if(directionLength<0.0001f){dirX=0.0f;dirY=1.0f;}
                else{dirX/=directionLength;dirY/=directionLength;}

                const float size=std::clamp(indicatorSize,24.0f,96.0f)*
                    (animatedIndicators?1.0f+0.045f*std::sin(seconds*4.0f+0.6f):1.0f);
                const float edgeMargin=size*0.5f+12.0f;
                const float halfW=std::max(1.0f,cx-edgeMargin);
                const float halfH=std::max(1.0f,cy-edgeMargin);
                const float tx=std::fabs(dirX)>0.0001f?halfW/std::fabs(dirX):std::numeric_limits<float>::infinity();
                const float ty=std::fabs(dirY)>0.0001f?halfH/std::fabs(dirY):std::numeric_limits<float>::infinity();
                const float t=std::min(tx,ty);
                float px=cx+dirX*t-size*0.5f;
                float py=cy+dirY*t-size*0.5f;
                px=std::clamp(px,12.0f,std::max(12.0f,surface.width-size-12.0f));
                py=std::clamp(py,12.0f,std::max(12.0f,surface.height-size-12.0f));

                constexpr float kRadToDeg=57.29577951308232f;
                float angle=std::atan2(dirY,dirX)*kRadToDeg;
                if(angle<0.0f)angle+=360.0f;
                int sprite=static_cast<int>(std::lround(angle/22.5f))%16;
                pl::modmenu::DrawCommand arrow{};
                arrow.type=pl::modmenu::DrawCommandType::Image;
                arrow.x=px;arrow.y=py;arrow.w=size;arrow.h=size;
                arrow.color=0xFFFFFFFFu;
                arrow.imageId="bedaim.esp.arrow."+std::to_string(sprite);
                commands.push_back(std::move(arrow));
            }
        }
    }
    pl::modmenu::submitDrawCommands(moduleId,commands);
}

void EspModule::onMenuRegistered(){
 using namespace pl::modmenu;ConfigSchemaBuilder schema;nlohmann::json config;saveConfig(config);
 schema.defaultCategory("players").category("players","Players","Player outline visibility and distance")
  .category("indicators","HUD indicators","Lock in Aim and off-screen player direction")
  .category("colors","Game-mode colors","Hexadecimal ARGB outline colors");
 auto toggle=[&](const char* key,const char* title,const char* desc,bool d,const char* category="players"){
  ConfigNodeV2 n;n.id=key;n.key=key;n.title=title;n.description=desc;n.category=category;n.type=ConfigControlTypeV2::Toggle;
  n.defaultValue=d?"true":"false";n.currentValue=cfgText(config,key,n.defaultValue.c_str());n.hasCurrentValue=true;schema.node(std::move(n));
 };
 toggle("showPlayers","Show players","Draw outlines around other players.",true);
 toggle("showInvisible","Show invisible players","Include invisible players in ESP.",false);
 toggle("showLockInAim","Target ring","Draw a ring above every visible player while ESP is enabled, independent of Aim Assist.",true,"indicators");
 toggle("showOffscreenIndicators","Off-screen indicators","Point to the nearest player outside the view.",true,"indicators");
 toggle("animatedIndicators","Animated indicators","Pulse the Lock in Aim and direction arrow subtly.",true,"indicators");
 ConfigNodeV2 size;size.id="indicatorSize";size.key="indicatorSize";size.title="Indicator size";
 size.description="Size of the screen-edge arrow and the player target ring." ;size.category="indicators";
 size.type=ConfigControlTypeV2::SliderFloat;size.defaultValue="48";size.currentValue=cfgText(config,"indicatorSize","48");
 size.hasCurrentValue=true;size.minValue="24";size.maxValue="96";size.step="2";size.unit=" px";schema.node(std::move(size));
 ConfigNodeV2 r;r.id="range";r.key="range";r.title="ESP range";r.description="Maximum range for ESP outlines and direction indicators.";
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
    key=="showOffscreenIndicators"||key=="animatedIndicators"){
  bool b=false;if(!parseBool(value,b))return false;
  if(key=="showPlayers")showPlayers=b;
  else if(key=="showInvisible")showInvisible=b;
  else if(key=="showLockInAim")showLockInAim=b;
  else if(key=="showOffscreenIndicators")showOffscreenIndicators=b;
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
 showOffscreenIndicators=j.value("showOffscreenIndicators",showOffscreenIndicators);
 animatedIndicators=j.value("animatedIndicators",animatedIndicators);
 indicatorSize=std::clamp(j.value("indicatorSize",indicatorSize),24.0f,96.0f);
 range=std::clamp(j.value("range",range),8.0f,80.0f);
 loadColor(j,"survivalColor",survivalColor);loadColor(j,"creativeColor",creativeColor);
 loadColor(j,"spectatorColor",spectatorColor);loadColor(j,"adventureColor",adventureColor);loadColor(j,"unknownColor",unknownColor);
}
void EspModule::saveConfig(nlohmann::json& j){
 Module::saveConfig(j);j["showPlayers"]=showPlayers;j["showInvisible"]=showInvisible;
 j["showLockInAim"]=showLockInAim;j["showOffscreenIndicators"]=showOffscreenIndicators;
 j["animatedIndicators"]=animatedIndicators;j["indicatorSize"]=std::clamp(indicatorSize,24.0f,96.0f);
 j["range"]=std::clamp(range,8.0f,80.0f);
 char color[12];auto save=[&](const char* key,std::uint32_t v){std::snprintf(color,sizeof(color),"#%08X",v);j[key]=std::string(color);};
 save("survivalColor",survivalColor);save("creativeColor",creativeColor);save("spectatorColor",spectatorColor);
 save("adventureColor",adventureColor);save("unknownColor",unknownColor);
}
