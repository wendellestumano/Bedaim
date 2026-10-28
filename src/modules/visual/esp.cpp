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
constexpr float kRadToDeg = 57.29577951308232f;
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
    // Serialize the empty submission with onFrame() so an older frame cannot
    // overwrite this clear after the module has been switched off.
    {
        std::lock_guard<std::mutex> lock(m_screenOverlayMutex);
        m_screenOverlays.clear();
        m_screenBuildingOverlays.clear();
        pl::modmenu::submitDrawCommands(moduleId,std::span<const pl::modmenu::DrawCommand>{});
    }
    {
        std::lock_guard<std::mutex> lock(m_hudTargetMutex);
        m_offscreenTargetValid=false;
        m_offscreenFrameTime={};
    }
}

void EspModule::beginScreenFrame(){
    std::lock_guard<std::mutex> lock(m_screenOverlayMutex);
    // Build into a private buffer; onFrame() continues to see the last complete frame.
    m_screenBuildingOverlays.clear();
}

void EspModule::endScreenFrame(){
    std::lock_guard<std::mutex> lock(m_screenOverlayMutex);
    // A render pass can finish after a concurrent UI toggle disabled ESP. Never
    // republish overlays from that stale pass after onDisable() has cleared them.
    if (!(enabled && masterEnabled && keybindActive)) {
        m_screenOverlays.clear();
        m_screenBuildingOverlays.clear();
        return;
    }
    m_screenOverlays.swap(m_screenBuildingOverlays);
    // The previous published frame is now the staging buffer and will be cleared by
    // beginScreenFrame() before the next render pass.
}

void EspModule::addScreenBox(float x,float y,float w,float h,std::uint32_t color,float thickness){
    if(!(enabled && masterEnabled && keybindActive))return;
    if(!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(w)||!std::isfinite(h)||
       !std::isfinite(thickness)||w<2.0f||h<2.0f)return;
    std::lock_guard<std::mutex> lock(m_screenOverlayMutex);
    if(m_screenBuildingOverlays.size()>=512)return;
    ScreenOverlay overlay;overlay.kind=ScreenOverlay::Kind::Box;overlay.x=x;overlay.y=y;
    overlay.w=w;overlay.h=h;overlay.color=color;overlay.thickness=std::clamp(thickness,1.0f,5.0f);
    m_screenBuildingOverlays.push_back(overlay);
}

void EspModule::addScreenRing(float x,float y,float radius,std::uint32_t color,float thickness){
    if(!(enabled && masterEnabled && keybindActive))return;
    if(!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(radius)||
       !std::isfinite(thickness)||radius<3.0f)return;
    std::lock_guard<std::mutex> lock(m_screenOverlayMutex);
    if(m_screenBuildingOverlays.size()>=512)return;
    ScreenOverlay overlay;overlay.kind=ScreenOverlay::Kind::Ring;overlay.x=x;overlay.y=y;
    overlay.radius=std::clamp(radius,3.0f,96.0f);overlay.color=color;
    overlay.thickness=std::clamp(thickness,1.0f,5.0f);m_screenBuildingOverlays.push_back(overlay);
}

std::vector<EspModule::ScreenOverlay> EspModule::screenOverlaySnapshot() const{
    std::lock_guard<std::mutex> lock(m_screenOverlayMutex);
    return m_screenOverlays;
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
       !std::isfinite(distance)||distance<0.0f||distance>(infiniteRange?100000.0f:std::clamp(range,8.0f,80.0f)))return;
    std::lock_guard<std::mutex> lock(m_hudTargetMutex);
    if(m_offscreenFrameTime.time_since_epoch().count()==0)return;

    // Keep the nearest ESP target selected even while it is visible. The HUD
    // projection decides whether to draw the arrow, so it disappears for that same
    // target instead of jumping to a different off-screen player.
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
    if(!(enabled && masterEnabled && keybindActive)){
        {
            std::lock_guard<std::mutex> lock(m_screenOverlayMutex);
            m_screenOverlays.clear();
            m_screenBuildingOverlays.clear();
            pl::modmenu::submitDrawCommands(moduleId,std::span<const pl::modmenu::DrawCommand>{});
        }
        {
            std::lock_guard<std::mutex> lock(m_hudTargetMutex);
            m_offscreenTargetValid=false;
            m_offscreenFrameTime={};
        }
        return;
    }
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
    commands.reserve(1);
    const float cx=surface.width*0.5f, cy=surface.height*0.5f;
    const auto now=std::chrono::steady_clock::now();
    const float seconds=std::chrono::duration<float>(now.time_since_epoch()).count();

    // The player reticle is rendered around each player's world-space AABB by
    // the existing RenderLevel hook. Never place a fixed reticle at screen center.

    for(const auto& overlay:screenOverlaySnapshot()){
        if(overlay.kind==ScreenOverlay::Kind::Box){
            pl::modmenu::DrawCommand box{};
            box.type=pl::modmenu::DrawCommandType::Rect;
            box.x=overlay.x;box.y=overlay.y;box.w=overlay.w;box.h=overlay.h;
            box.color=overlay.color;box.size=overlay.thickness;
            commands.push_back(std::move(box));
        }else{
            constexpr int segments=48;
            const float phase=animatedIndicators?seconds*targetSpeed*0.85f:0.0f;
            const float tau=6.2831853071795864769f;
            for(int i=0;i<segments;++i){
                if((i%12)>=10)continue;
                const float a0=tau*static_cast<float>(i)/segments+phase;
                const float a1=tau*static_cast<float>(i+1)/segments+phase;
                const float r0=overlay.radius*(0.94f+0.06f*std::cos(4.0f*(a0-phase)));
                const float r1=overlay.radius*(0.94f+0.06f*std::cos(4.0f*(a1-phase)));
                pl::modmenu::DrawCommand line{};
                line.type=pl::modmenu::DrawCommandType::Line;
                line.x=overlay.x+std::cos(a0)*r0;line.y=overlay.y+std::sin(a0)*r0;
                line.w=overlay.x+std::cos(a1)*r1;line.h=overlay.y+std::sin(a1)*r1;
                line.color=overlay.color;line.size=overlay.thickness;
                commands.push_back(std::move(line));
            }
        }
    }
    if(showOffscreenIndicators){
        bedrocktools::sdk::Vec3 target{},local{};
        bedrocktools::sdk::Vec2 rotation{};
        float distance=0.0f;
        if(offscreenTargetSnapshot(target,local,rotation,distance)){
            float dx=target.x-local.x;
            float dy=target.y-(local.y+1.62f);
            float dz=target.z-local.z;
            const float horizontal=std::sqrt(dx*dx+dz*dz);
            constexpr float kDegToRad=0.01745329251994329577f;
            if(std::sqrt(dx*dx+dy*dy+dz*dz)>0.001f){
                // Project the target vector through the same camera basis used by the
                // 2D box renderer. This avoids yaw/pitch sign flips and tracks the
                // actual on-screen bearing even for targets above, below, or behind.
                const float yaw=rotation.y*kDegToRad;
                const float pitch=rotation.x*kDegToRad;
                const float sy=std::sin(yaw),cy=std::cos(yaw);
                const float sp=std::sin(pitch),cp=std::cos(pitch);
                const float cameraX=dx*cy+dz*sy;
                const float cameraY=dx*(-sy*sp)+dy*cp+dz*(cy*sp);
                const float cameraDepth=dx*(-sy*cp)+dy*(-sp)+dz*(cy*cp);
                const float aspect=std::clamp(surface.width/surface.height,0.5f,3.2f);
                const float tanHalfVertical=std::tan(std::clamp(locatorFov,60.0f,140.0f)*0.5f*kDegToRad);
                const bool inFront=cameraDepth>0.05f;
                const float safeDepth=std::max(0.05f,cameraDepth);
                const float projectedX=cameraX/(safeDepth*std::max(0.001f,tanHalfVertical)*aspect);
                const float projectedY=-cameraY/(safeDepth*std::max(0.001f,tanHalfVertical));
                const bool targetOnScreen=inFront&&std::isfinite(projectedX)&&std::isfinite(projectedY)&&
                                          std::fabs(projectedX)<=1.0f&&std::fabs(projectedY)<=1.0f;
                if(!targetOnScreen){
                float dirX=0.0f,dirY=0.0f;
                if(inFront){
                    dirX=projectedX;
                    dirY=projectedY;
                }else{
                    dirX=cameraX;
                    dirY=-cameraY;
                }
                const float directionLength=std::sqrt(dirX*dirX+dirY*dirY);
                if(directionLength<0.0001f){
                    // A target almost exactly behind the player has no stable
                    // 2D projection. Choose the corresponding horizontal edge.
                    dirX=1.0f;
                    dirY=0.0f;
                }else{
                    dirX/=directionLength;
                    dirY/=directionLength;
                }

                const float size=std::clamp(locatorSize,24.0f,160.0f)*
                    (animatedIndicators?1.0f+0.045f*std::sin(seconds*4.0f*targetSpeed+0.6f):1.0f);
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
    }
    // Recheck while serialized with onDisable(): a stale onFrame invocation must
    // not replace the empty command list after a concurrent module toggle.
    {
        std::lock_guard<std::mutex> lock(m_screenOverlayMutex);
        if (!(enabled && masterEnabled && keybindActive)) {
            m_screenOverlays.clear();
            m_screenBuildingOverlays.clear();
            pl::modmenu::submitDrawCommands(moduleId,std::span<const pl::modmenu::DrawCommand>{});
            return;
        }
        pl::modmenu::submitDrawCommands(moduleId,commands);
    }
}

void EspModule::onMenuRegistered(){
 using namespace pl::modmenu;ConfigSchemaBuilder schema;nlohmann::json config;saveConfig(config);
 schema.defaultCategory("players").category("players","Players","Player outline visibility and distance")
  .category("indicators","HUD indicators","Lock in Aim and off-screen player direction")
  .category("appearance","ESP appearance","World-space ESP and hitbox outline thickness")
  .category("colors","Game-mode colors","Hexadecimal ARGB outline colors");
 auto toggle=[&](const char* key,const char* title,const char* desc,bool d,const char* category="players"){
  ConfigNodeV2 n;n.id=key;n.key=key;n.title=title;n.description=desc;n.category=category;n.type=ConfigControlTypeV2::Toggle;
  n.defaultValue=d?"true":"false";n.currentValue=cfgText(config,key,n.defaultValue.c_str());n.hasCurrentValue=true;schema.node(std::move(n));
 };
 toggle("showPlayers","Show players","Draw outlines around other players.",true);
 toggle("showInvisible","Show invisible players","Include invisible players in ESP.",false);
 toggle("infiniteRange","Infinite ESP range","Show ESP and locator targets at any distance among entities loaded by the game.",true,"players");
 toggle("box2D","2D ESP box","Render the ESP box in screen space. Off renders it in the 3D world.",false,"appearance");
 toggle("showLockInAim","Target ring","Draw an animated ring above each in-range player, independent of Aim Assist.",true,"indicators");
 toggle("ring2D","2D target ring","Render the target ring in screen space. Off renders it in the 3D world.",false,"indicators");
 toggle("showOffscreenIndicators","Off-screen indicators","Point to the nearest player outside the view.",true,"indicators");
 toggle("animatedIndicators","Animated indicators","Pulse the Lock in Aim and direction arrow subtly.",true,"indicators");
 ConfigNodeV2 size;size.id="indicatorSize";size.key="indicatorSize";size.title="Target ring size";
 size.description="Scale of the rotating target ring.";size.category="indicators";
 size.type=ConfigControlTypeV2::SliderFloat;size.defaultValue="48";size.currentValue=cfgText(config,"indicatorSize","48");
 size.hasCurrentValue=true;size.minValue="24";size.maxValue="160";size.step="4";size.unit=" relative";schema.node(std::move(size));
 auto slider=[&](const char* key,const char* title,const char* desc,const char* category,
                  float defaultValue,float minimum,float maximum,float step,const char* unit){
  ConfigNodeV2 n;n.id=key;n.key=key;n.title=title;n.description=desc;n.category=category;
  n.type=ConfigControlTypeV2::SliderFloat;n.defaultValue=std::to_string(defaultValue);
  n.currentValue=cfgText(config,key,n.defaultValue.c_str());n.hasCurrentValue=true;
  n.minValue=std::to_string(minimum);n.maxValue=std::to_string(maximum);
  n.step=std::to_string(step);n.unit=unit;schema.node(std::move(n));
 };
 slider("locatorFov","Locator FOV","Vertical FOV threshold used to decide when the target is outside the visible area.", "indicators",90.0f,60.0f,140.0f,1.0f,"°");
 slider("locatorSize","Locator size","Size of the off-screen locator arrow in pixels.","indicators",48.0f,24.0f,160.0f,4.0f," px");
 slider("targetYOffset","Target Y offset","Vertical offset from the native player-box center. Negative moves the ring down.", "indicators",0.0f,-1.5f,1.5f,0.05f," blocks");
 slider("targetSpeed","Target animation speed","Speed multiplier for the target-ring rotation and animated indicator pulse.", "indicators",1.0f,0.1f,4.0f,0.1f,"x");
 slider("targetThickness","Target ring thickness","Number of adjacent world-space strokes used to thicken the ring (1–5).", "indicators",2.0f,1.0f,5.0f,1.0f," layers");
 slider("outlineThickness","ESP box thickness","Approximate outline thickness for normal player ESP boxes. Zero uses one line.", "appearance",0.025f,0.0f,0.10f,0.005f," blocks");
 slider("hitboxThickness","BedrockBounds outline thickness","Approximate thickness for the separate expanded-hitbox outline.", "appearance",0.025f,0.0f,0.10f,0.005f," blocks");
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
    key=="showOffscreenIndicators"||key=="animatedIndicators"||key=="infiniteRange"||
    key=="box2D"||key=="ring2D"){
  bool b=false;if(!parseBool(value,b))return false;
  if(key=="showPlayers")showPlayers=b;
  else if(key=="showInvisible")showInvisible=b;
  else if(key=="infiniteRange")infiniteRange=b;
  else if(key=="box2D")box2D=b;
  else if(key=="ring2D")ring2D=b;
  else if(key=="showLockInAim")showLockInAim=b;
  else if(key=="showOffscreenIndicators")showOffscreenIndicators=b;
  else animatedIndicators=b;
  return true;
 }
 if(key=="range"){float f=0;if(!parseFloat(value,f))return false;range=std::clamp(f,8.0f,80.0f);return true;}
 if(key=="indicatorSize"){float f=0;if(!parseFloat(value,f))return false;indicatorSize=std::clamp(f,24.0f,160.0f);return true;}
 if(key=="locatorSize"){float f=0;if(!parseFloat(value,f))return false;locatorSize=std::clamp(f,24.0f,160.0f);return true;}
 if(key=="locatorFov"){float f=0;if(!parseFloat(value,f))return false;locatorFov=std::clamp(f,60.0f,140.0f);return true;}
 if(key=="targetYOffset"||key=="targetSpeed"||key=="targetThickness"||
    key=="outlineThickness"||key=="hitboxThickness"){
  float f=0;if(!parseFloat(value,f))return false;
  if(key=="targetYOffset")targetYOffset=std::clamp(f,-1.5f,1.5f);
  else if(key=="targetSpeed")targetSpeed=std::clamp(f,0.1f,4.0f);
  else if(key=="targetThickness")targetThickness=std::clamp(std::round(f),1.0f,5.0f);
  else if(key=="outlineThickness")outlineThickness=std::clamp(f,0.0f,0.10f);
  else hitboxThickness=std::clamp(f,0.0f,0.10f);
  return true;
 }
 std::uint32_t c=0;if(!parseColor(value,c))return false;
 if(key=="survivalColor")survivalColor=c;else if(key=="creativeColor")creativeColor=c;
 else if(key=="spectatorColor")spectatorColor=c;else if(key=="adventureColor")adventureColor=c;
 else if(key=="unknownColor")unknownColor=c;else return false;return true;
}
void EspModule::loadConfig(const nlohmann::json& j){
 Module::loadConfig(j);showPlayers=j.value("showPlayers",showPlayers);showInvisible=j.value("showInvisible",showInvisible);
 showLockInAim=j.value("showLockInAim",showLockInAim);
 showOffscreenIndicators=j.value("showOffscreenIndicators",showOffscreenIndicators);
 infiniteRange=j.value("infiniteRange",infiniteRange);
 box2D=j.value("box2D",box2D);ring2D=j.value("ring2D",ring2D);
 animatedIndicators=j.value("animatedIndicators",animatedIndicators);
 indicatorSize=std::clamp(j.value("indicatorSize",indicatorSize),24.0f,160.0f);
 locatorSize=std::clamp(j.value("locatorSize",locatorSize),24.0f,160.0f);
 locatorFov=std::clamp(j.value("locatorFov",locatorFov),60.0f,140.0f);
 targetYOffset=std::clamp(j.value("targetYOffset",targetYOffset),-1.5f,1.5f);
 targetSpeed=std::clamp(j.value("targetSpeed",targetSpeed),0.1f,4.0f);
 targetThickness=std::clamp(std::round(j.value("targetThickness",targetThickness)),1.0f,5.0f);
 outlineThickness=std::clamp(j.value("outlineThickness",outlineThickness),0.0f,0.10f);
 hitboxThickness=std::clamp(j.value("hitboxThickness",hitboxThickness),0.0f,0.10f);
 range=std::clamp(j.value("range",range),8.0f,80.0f);
 loadColor(j,"survivalColor",survivalColor);loadColor(j,"creativeColor",creativeColor);
 loadColor(j,"spectatorColor",spectatorColor);loadColor(j,"adventureColor",adventureColor);loadColor(j,"unknownColor",unknownColor);
}
void EspModule::saveConfig(nlohmann::json& j){
 Module::saveConfig(j);j["showPlayers"]=showPlayers;j["showInvisible"]=showInvisible;
 j["showLockInAim"]=showLockInAim;j["showOffscreenIndicators"]=showOffscreenIndicators;
 j["infiniteRange"]=infiniteRange;j["box2D"]=box2D;j["ring2D"]=ring2D;
 j["animatedIndicators"]=animatedIndicators;j["indicatorSize"]=std::clamp(indicatorSize,24.0f,160.0f);
 j["locatorSize"]=std::clamp(locatorSize,24.0f,160.0f);
 j["locatorFov"]=std::clamp(locatorFov,60.0f,140.0f);
 j["targetYOffset"]=std::clamp(targetYOffset,-1.5f,1.5f);
 j["targetSpeed"]=std::clamp(targetSpeed,0.1f,4.0f);
 j["targetThickness"]=std::clamp(std::round(targetThickness),1.0f,5.0f);
 j["outlineThickness"]=std::clamp(outlineThickness,0.0f,0.10f);
 j["hitboxThickness"]=std::clamp(hitboxThickness,0.0f,0.10f);
 j["range"]=std::clamp(range,8.0f,80.0f);
 char color[12];auto save=[&](const char* key,std::uint32_t v){std::snprintf(color,sizeof(color),"#%08X",v);j[key]=std::string(color);};
 save("survivalColor",survivalColor);save("creativeColor",creativeColor);save("spectatorColor",spectatorColor);
 save("adventureColor",adventureColor);save("unknownColor",unknownColor);
}
