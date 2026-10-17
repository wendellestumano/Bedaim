#include "elytramacing.hpp"

#include "../ModuleRegistry.hpp"
#include "aimassist.hpp"
#include "MaceFallPolicy.hpp"
#include "../../core/Diagnostics.hpp"
#include "../../core/GameHooks.hpp"
#include "../../core/InventoryAccess.hpp"

#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/LocalPlayerTickEvent.hpp>
#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <pl/ModMenu.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

namespace {
using bedrocktools::sdk::AABB;
using bedrocktools::sdk::Player;
using bedrocktools::sdk::Vec3;

bool finiteVec(const Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
bool validBounds(const AABB& b) {
    if (!finiteVec(b.min) || !finiteVec(b.max)) return false;
    const float w=b.max.x-b.min.x, h=b.max.y-b.min.y, d=b.max.z-b.min.z;
    return w>0.0f && w<=5.0f && h>=0.25f && h<=8.0f && d>0.0f && d<=5.0f;
}
float distanceSquared(const Vec3& a,const Vec3& b) {
    const float x=a.x-b.x,y=a.y-b.y,z=a.z-b.z; return x*x+y*y+z*z;
}
Vec3 eyePosition(Player* p) {
    const Vec3 pos=p->position(); const AABB b=p->bounds();
    if(validBounds(b)) return {(b.min.x+b.max.x)*0.5f,b.min.y+(b.max.y-b.min.y)*0.85f,(b.min.z+b.max.z)*0.5f};
    return {pos.x,pos.y+1.5f,pos.z};
}

// Same Bedrock entity-data encoding as the established Aim Assist helper,
// but every pointer and scalar read is validated with the SDK memory probe.
bool safelyGliding(const void* actor) {
    using namespace bedrocktools::sdk;
    if (!actor) return false;
    const auto base=reinterpret_cast<std::uintptr_t>(actor);
    const auto wrapper=base+offsets::Actor::mEntityData;
    if(wrapper<base || !isReadableMemory(reinterpret_cast<const void*>(wrapper),sizeof(void*))) return false;
    void* component=nullptr;
    std::memcpy(&component,reinterpret_cast<const void*>(wrapper),sizeof(component));
    if(!component) return false;
    const auto ca=reinterpret_cast<std::uintptr_t>(component);
    if(!isReadableMemory(reinterpret_cast<const void*>(ca),2*sizeof(void*))) return false;
    void** begin=nullptr; void** end=nullptr;
    std::memcpy(&begin,reinterpret_cast<const void*>(ca),sizeof(begin));
    std::memcpy(&end,reinterpret_cast<const void*>(ca+sizeof(void*)),sizeof(end));
    if(!begin||!end) return false;
    const auto b=reinterpret_cast<std::uintptr_t>(begin), e=reinterpret_cast<std::uintptr_t>(end);
    if(e<=b || (e-b)%sizeof(void*)!=0 || (e-b)/sizeof(void*)>192 || !isReadableMemory(begin,sizeof(void*))) return false;
    void* item=nullptr; std::memcpy(&item,begin,sizeof(item)); if(!item) return false;
    const auto ia=reinterpret_cast<std::uintptr_t>(item);
    const auto ta=ia+offsets::DataItem::mType, ida=ia+offsets::DataItem::mId, va=ia+offsets::DataItem::mValue;
    if(ta<ia||ida<ia||va<ia ||
       !isReadableMemory(reinterpret_cast<const void*>(ta),sizeof(std::uint8_t)) ||
       !isReadableMemory(reinterpret_cast<const void*>(ida),sizeof(std::uint16_t)) ||
       !isReadableMemory(reinterpret_cast<const void*>(va),sizeof(std::uint64_t))) return false;
    std::uint8_t type=0; std::uint16_t id=0; std::uint64_t flags=0;
    std::memcpy(&type,reinterpret_cast<const void*>(ta),sizeof(type));
    std::memcpy(&id,reinterpret_cast<const void*>(ida),sizeof(id));
    std::memcpy(&flags,reinterpret_cast<const void*>(va),sizeof(flags));
    return type==7 && id==0 && (flags&(std::uint64_t{1}<<32))!=0;
}
}

ElytraMacingModule::ElytraMacingModule()
    : Module("ElytraMacing","Independent elytra mace timing/settings; target source is the current BedrockAim snapshot.") {
    keybind=0;
}
void ElytraMacingModule::onInit() {
    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [this](auto& e){ processPlayerTick(e.player); });
    bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Info,
        "ElytraMacing","lifecycle","initialized","independent settings; does not write shared module state");
}
void ElytraMacingModule::onEnable() {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_pendingAttack=false; m_pendingPlayer=nullptr; m_pendingTarget=nullptr;
    m_pendingMaceSlot=m_pendingPreviousSlot=-1;
    m_hasLastPlayerY=m_hasPeakY=false; m_nextAttack={}; m_gliding.store(false,std::memory_order_release);
}
void ElytraMacingModule::onDisable() {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    if(m_pendingAttack&&m_restorePreviousSlot&&m_pendingPlayer&&m_pendingMaceSlot>=0) {
        auto& inv=bedrocktools::core::InventoryAccess::get();
        if(inv.selectedHotbarSlot(m_pendingPlayer)==m_pendingMaceSlot&&m_pendingPreviousSlot>=0)
            inv.selectHotbarSlot(m_pendingPlayer,m_pendingPreviousSlot);
    }
    m_pendingAttack=false; m_pendingPlayer=nullptr; m_pendingTarget=nullptr;
    m_pendingMaceSlot=m_pendingPreviousSlot=-1;
    m_hasLastPlayerY=m_hasPeakY=false; m_gliding.store(false,std::memory_order_release);
}
void ElytraMacingModule::processPlayerTick(Player* player) {
    using namespace bedrocktools::sdk;
    std::lock_guard<std::mutex> lock(m_stateMutex);
    auto& inv=bedrocktools::core::InventoryAccess::get();
    auto finish=[&](bool restore) {
        if(restore&&m_restorePreviousSlot&&m_pendingPlayer&&m_pendingMaceSlot>=0&&
           inv.selectedHotbarSlot(m_pendingPlayer)==m_pendingMaceSlot&&m_pendingPreviousSlot>=0)
            inv.selectHotbarSlot(m_pendingPlayer,m_pendingPreviousSlot);
        m_pendingAttack=false; m_pendingPlayer=nullptr; m_pendingTarget=nullptr;
        m_pendingMaceSlot=m_pendingPreviousSlot=-1;
    };
    if(!player) {
        m_hasLastPlayerY=m_hasPeakY=false; m_gliding.store(false,std::memory_order_release);
        finish(false); return;
    }
    const bool gliding=safelyGliding(player);
    m_gliding.store(gliding,std::memory_order_release);
    const Vec3 pos=player->position();
    if(!finiteVec(pos)) { m_hasLastPlayerY=m_hasPeakY=false; finish(true); return; }
    const float dy=m_hasLastPlayerY?pos.y-m_lastPlayerY:0.0f;
    m_lastPlayerY=pos.y; m_hasLastPlayerY=true;
    if(!m_hasPeakY||dy>=-0.025f) { m_peakY=pos.y; m_hasPeakY=true; }
    if(pos.y>m_peakY) m_peakY=pos.y;
    const float fallDistance=std::max(0.0f,m_peakY-pos.y);

    if(m_pendingAttack) {
        const bool stillMace=inv.selectedHotbarSlot(player)==m_pendingMaceSlot;
        auto restore=[&](){finish(true);};
        if(player!=m_pendingPlayer||!stillMace||(m_requireGliding&&!gliding)) { restore(); return; }
        auto* aimBase=ModuleRegistry::get().find("bedrocktools.Aim Assist");
        if(!aimBase||!aimBase->enabled) { restore(); return; }
        auto* aim=static_cast<AimAssistModule*>(aimBase);
        void* target=nullptr; AABB bounds{}; float aimRange=0.0f;
        if(!aim->currentTargetSnapshot(target,bounds,aimRange)||target!=m_pendingTarget||!validBounds(bounds)||!std::isfinite(aimRange)||aimRange<=0.0f) {
            restore(); return;
        }
        const Vec3 eye=eyePosition(player);
        const Vec3 center{(bounds.min.x+bounds.max.x)*0.5f,(bounds.min.y+bounds.max.y)*0.5f,(bounds.min.z+bounds.max.z)*0.5f};
        const float range=std::min(m_attackRange,aimRange);
        if(!std::isfinite(range)||range<=0.0f||distanceSquared(eye,center)>range*range||
           (m_requireCrosshair&&!aim->currentTargetInCrosshair(player,target,m_crosshairTolerance))) { restore(); return; }
        void* gm=bedrocktools::core::gamehooks::gameMode();
        if(!gm) { restore(); return; }
        const Vec3 hit{std::clamp(eye.x,bounds.min.x,bounds.max.x),
            std::clamp(eye.y,bounds.min.y,bounds.max.y),std::clamp(eye.z,bounds.min.z,bounds.max.z)};
        const bool attacked=bedrocktools::core::gamehooks::attack(gm,target,true,&hit);
        bedrocktools::diagnostics::probe(attacked?bedrocktools::diagnostics::Level::Info:bedrocktools::diagnostics::Level::Warning,
            "ElytraMacing","attack_result",attacked?"accepted":"rejected");
        finish(true);
        if(attacked) m_nextAttack=std::chrono::steady_clock::now()+
            std::chrono::milliseconds(static_cast<int>(std::clamp(m_attackIntervalMs,80.0f,1000.0f)));
        return;
    }
    if(!enabled||!m_behaviorEnabled||(m_requireGliding&&!gliding)||
       !bedaim::macefall::shouldBeginMaceAction(dy,fallDistance,m_minimumFallDistance)) return;
    const auto now=std::chrono::steady_clock::now();
    if(m_nextAttack.time_since_epoch().count()!=0&&now<m_nextAttack) return;
    auto* aimBase=ModuleRegistry::get().find("bedrocktools.Aim Assist");
    if(!aimBase||!aimBase->enabled) return;
    auto* aim=static_cast<AimAssistModule*>(aimBase);
    void* target=nullptr; AABB bounds{}; float aimRange=0.0f;
    if(!aim->currentTargetSnapshot(target,bounds,aimRange)||!validBounds(bounds)||
       (m_requireCrosshair&&!aim->currentTargetInCrosshair(player,target,m_crosshairTolerance))) return;
    const Vec3 eye=eyePosition(player);
    const Vec3 targetPoint{(bounds.min.x+bounds.max.x)*0.5f,
        bounds.min.y+(bounds.max.y-bounds.min.y)*m_targetAimHeight,
        (bounds.min.z+bounds.max.z)*0.5f};
    const float dist=std::sqrt(distanceSquared(eye,targetPoint));
    const float range=std::min(m_attackRange,aimRange);
    if(!std::isfinite(dist)||!std::isfinite(range)||dist>range) return;
    if(!m_equipMace) {
        void* gm=bedrocktools::core::gamehooks::gameMode(); if(!gm) return;
        const Vec3 hit{std::clamp(eye.x,bounds.min.x,bounds.max.x),
            std::clamp(eye.y,bounds.min.y,bounds.max.y),std::clamp(eye.z,bounds.min.z,bounds.max.z)};
        if(bedrocktools::core::gamehooks::attack(gm,target,true,&hit))
            m_nextAttack=now+std::chrono::milliseconds(static_cast<int>(std::clamp(m_attackIntervalMs,80.0f,1000.0f)));
        return;
    }
    const int mace=inv.findHotbarSlot(player,"mace"), previous=inv.selectedHotbarSlot(player);
    if(mace<0||previous<0) return;
    if(previous!=mace&&!inv.selectHotbarSlot(player,mace)) return;
    m_pendingPlayer=player; m_pendingTarget=target; m_pendingMaceSlot=mace; m_pendingPreviousSlot=previous; m_pendingAttack=true;
}
void ElytraMacingModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j); std::lock_guard<std::mutex> lock(m_stateMutex);
    m_behaviorEnabled=j.value("m_behaviorEnabled",m_behaviorEnabled);
    m_requireGliding=j.value("m_requireGliding",m_requireGliding);
    m_equipMace=j.value("m_equipMace",m_equipMace);
    m_restorePreviousSlot=j.value("m_restorePreviousSlot",m_restorePreviousSlot);
    m_requireCrosshair=j.value("m_requireCrosshair",m_requireCrosshair);
    m_attackRange=std::clamp(j.value("m_attackRange",m_attackRange),2.0f,12.0f);
    m_minimumFallDistance=std::clamp(j.value("m_minimumFallDistance",m_minimumFallDistance),0.0f,15.0f);
    m_attackIntervalMs=std::clamp(j.value("m_attackIntervalMs",m_attackIntervalMs),80.0f,1000.0f);
    m_targetAimHeight=std::clamp(j.value("m_targetAimHeight",m_targetAimHeight),0.1f,0.95f);
    m_crosshairTolerance=std::clamp(j.value("m_crosshairTolerance",m_crosshairTolerance),0.05f,0.8f);
}
void ElytraMacingModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j); std::lock_guard<std::mutex> lock(m_stateMutex);
    j["m_behaviorEnabled"]=m_behaviorEnabled; j["m_requireGliding"]=m_requireGliding;
    j["m_equipMace"]=m_equipMace; j["m_restorePreviousSlot"]=m_restorePreviousSlot;
    j["m_requireCrosshair"]=m_requireCrosshair; j["m_attackRange"]=m_attackRange;
    j["m_minimumFallDistance"]=m_minimumFallDistance; j["m_attackIntervalMs"]=m_attackIntervalMs;
    j["m_targetAimHeight"]=m_targetAimHeight; j["m_crosshairTolerance"]=m_crosshairTolerance;
}
void ElytraMacingModule::onMenuRegistered() {
    using namespace pl::modmenu;
    ConfigSchemaBuilder schema; schema.defaultCategory("combat").category("combat","Combat","Independent elytra mace timing");
    auto slider=[&](const char* key,const char* title,const char* min,const char* max,const char* step,const char* unit) {
        ConfigNodeV2 n; n.id=key;n.key=key;n.title=title;n.category="combat";
        n.type=ConfigControlTypeV2::SliderFloat;n.minValue=min;n.maxValue=max;n.step=step;n.unit=unit;schema.node(std::move(n));
    };
    auto toggle=[&](const char* key,const char* title) {
        ConfigNodeV2 n;n.id=key;n.key=key;n.title=title;n.category="combat";n.type=ConfigControlTypeV2::Toggle;schema.node(std::move(n));
    };
    toggle("m_behaviorEnabled","Enable ElytraMacing behavior");
    toggle("m_requireGliding","Only while gliding with elytra");
    toggle("m_equipMace","Equip mace before attack");
    toggle("m_restorePreviousSlot","Restore previous hotbar slot");
    toggle("m_requireCrosshair","Require target in crosshair");
    slider("m_attackRange","Target range","2","12","0.1"," blocks");
    slider("m_minimumFallDistance","Minimum fall distance","0","15","0.1"," blocks");
    slider("m_attackIntervalMs","Attack interval","80","1000","10"," ms");
    slider("m_targetAimHeight","Target point height","0.1","0.95","0.01"," fraction");
    slider("m_crosshairTolerance","Crosshair tolerance","0.05","0.8","0.01"," blocks");
    ConfigNodeV2 kb;kb.id="keybind";kb.key="keybind";kb.title="Keybind";kb.category="combat";
    kb.type=ConfigControlTypeV2::Keybind;schema.node(std::move(kb));
    pl::modmenu::setConfigSchemaJson(moduleId,schema.toJson());
}
bool ElytraMacingModule::onMenuConfigChanged(std::string_view key,std::string_view value) {
    auto flag=[&](){return value=="true"||value=="1"||value=="on";};
    if(key=="m_behaviorEnabled"||key=="m_requireGliding"||key=="m_equipMace"||
       key=="m_restorePreviousSlot"||key=="m_requireCrosshair") {
        std::lock_guard<std::mutex> lock(m_stateMutex); const bool v=flag();
        if(key=="m_behaviorEnabled")m_behaviorEnabled=v;
        else if(key=="m_requireGliding")m_requireGliding=v;
        else if(key=="m_equipMace")m_equipMace=v;
        else if(key=="m_restorePreviousSlot")m_restorePreviousSlot=v;
        else if(key=="m_requireCrosshair")m_requireCrosshair=v;
        return true;
    }
    try {
        std::size_t n=0; const std::string text(value); const float v=std::stof(text,&n);
        while(n<text.size()&&std::isspace(static_cast<unsigned char>(text[n])))++n;
        if(n!=text.size()||!std::isfinite(v))return false;
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if(key=="m_attackRange")m_attackRange=std::clamp(v,2.0f,12.0f);
        else if(key=="m_minimumFallDistance")m_minimumFallDistance=std::clamp(v,0.0f,15.0f);
        else if(key=="m_attackIntervalMs")m_attackIntervalMs=std::clamp(v,80.0f,1000.0f);
        else if(key=="m_targetAimHeight")m_targetAimHeight=std::clamp(v,0.1f,0.95f);
        else if(key=="m_crosshairTolerance")m_crosshairTolerance=std::clamp(v,0.05f,0.8f);
        else return false;
        return true;
    } catch(...) { return false; }
}
