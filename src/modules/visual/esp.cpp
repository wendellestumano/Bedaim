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
EspModule::EspModule():Module("ESP","Draws player outlines colored by detected game mode."){showInMenu=true;}
void EspModule::onEnable() {
    auto* hitbox = static_cast<HitboxModule*>(ModuleRegistry::get().find("bedrocktools.Hitbox"));
    if (hitbox) {
        hitbox->ensureRenderHook();
    } else {
        bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Warning, "ESP",
            "renderlevel_hook=unavailable; Hitbox renderer module was not registered");
    }
}
void EspModule::onMenuRegistered(){
 using namespace pl::modmenu;ConfigSchemaBuilder schema;nlohmann::json config;saveConfig(config);
 schema.defaultCategory("players").category("players","Players","Player outline visibility and distance")
  .category("colors","Game-mode colors","Hexadecimal ARGB outline colors");
 auto toggle=[&](const char* key,const char* title,const char* desc,bool d){
  ConfigNodeV2 n;n.id=key;n.key=key;n.title=title;n.description=desc;n.category="players";n.type=ConfigControlTypeV2::Toggle;
  n.defaultValue=d?"true":"false";n.currentValue=cfgText(config,key,n.defaultValue.c_str());n.hasCurrentValue=true;schema.node(std::move(n));
 };
 toggle("showPlayers","Show players","Draw outlines around other players.",true);
 toggle("showInvisible","Show invisible players","Include invisible players in ESP.",false);
 ConfigNodeV2 r;r.id="range";r.key="range";r.title="ESP range";r.description="Maximum range for ESP outlines.";
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
 if(key=="showPlayers"||key=="showInvisible"){bool b=false;if(!parseBool(value,b))return false;
  if(key=="showPlayers")showPlayers=b;else showInvisible=b;return true;}
 if(key=="range"){float f=0;if(!parseFloat(value,f))return false;range=std::clamp(f,8.0f,80.0f);return true;}
 std::uint32_t c=0;if(!parseColor(value,c))return false;
 if(key=="survivalColor")survivalColor=c;else if(key=="creativeColor")creativeColor=c;
 else if(key=="spectatorColor")spectatorColor=c;else if(key=="adventureColor")adventureColor=c;
 else if(key=="unknownColor")unknownColor=c;else return false;return true;
}
void EspModule::loadConfig(const nlohmann::json& j){
 Module::loadConfig(j);showPlayers=j.value("showPlayers",showPlayers);showInvisible=j.value("showInvisible",showInvisible);
 range=std::clamp(j.value("range",range),8.0f,80.0f);
 loadColor(j,"survivalColor",survivalColor);loadColor(j,"creativeColor",creativeColor);
 loadColor(j,"spectatorColor",spectatorColor);loadColor(j,"adventureColor",adventureColor);loadColor(j,"unknownColor",unknownColor);
}
void EspModule::saveConfig(nlohmann::json& j){
 Module::saveConfig(j);j["showPlayers"]=showPlayers;j["showInvisible"]=showInvisible;j["range"]=std::clamp(range,8.0f,80.0f);
 char color[12];auto save=[&](const char* key,std::uint32_t v){std::snprintf(color,sizeof(color),"#%08X",v);j[key]=std::string(color);};
 save("survivalColor",survivalColor);save("creativeColor",creativeColor);save("spectatorColor",spectatorColor);
 save("adventureColor",adventureColor);save("unknownColor",unknownColor);
}
