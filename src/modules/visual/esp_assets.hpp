#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <cmath>
#include <pl/ModMenu.hpp>
namespace bedaim::espassets {
struct PaletteSprite { int width; int height; std::array<std::uint8_t,48> palette; std::string_view packed; bool runLengthEncoded = false; };
inline constexpr PaletteSprite LockInAim{48,42,{191,231,242,155,157,190,59,183,213,227,98,115,125,97,129,32,90,121,182,36,60,126,29,48,66,25,38,22,45,69,10,30,54,5,9,19,153,0,28,35,0,9,0,0,92,0,0,0},R"BEDAIM(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)BEDAIM"};
inline constexpr PaletteSprite OffscreenArrow{40,40,{189,179,255,161,138,255,125,93,255,102,76,255,114,71,255,112,72,255,111,72,255,109,73,255,104,74,255,123,67,255,127,65,255,130,61,255,118,69,255,116,70,255,115,69,255,48,27,255},R"BEDAIM(/wDVAAGyAbUgAAGwAbYBuAG2AbAiAAGgAbsDrwGsAaIhAAGlBa8BrgGmAfAfAAGXAp8BLwGvA58BmgGRHgABmAKfAR8BDwEvA58BnQGUHQAByAKfAR8CDwEfBJ8Blw4AAfANyAHMAs8BHwMPAR8BnwPPAdINAAEgEN8BHwUPAS8C3wHnDQABIBBPAR8FDwEvAk8B5w0AAfANZwFbAl8BHwMPAR8DXwFvAWIbAAF4An8BHwIPAS8BbwN/AXccAAF4An8CHwEvA38BfQGEHQABhwKPAS8BPwOPAYoBMR4AATUFPwGOATYgAAGgATsDPwE8ATIiAAEgATYBOAE2ASApAAExATX/AMMA)BEDAIM",true};
inline std::vector<std::uint8_t> decodeBase64(std::string_view in) {
 static constexpr std::string_view alpha="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
 std::vector<std::uint8_t> out; out.reserve(in.size()*3/4); unsigned acc=0; int bits=0;
 for(char ch:in){ if(ch=='='||ch=='\n'||ch=='\r'||ch==' '||ch=='\t') continue; auto p=alpha.find(ch); if(p==std::string_view::npos)continue; acc=(acc<<6)|static_cast<unsigned>(p); bits+=6; if(bits>=8){bits-=8;out.push_back(static_cast<std::uint8_t>((acc>>bits)&255));}}
 return out;
}
inline std::vector<unsigned char> decodeRgba(const PaletteSprite& s) {
 const auto encoded=decodeBase64(s.packed); const std::size_t count=static_cast<std::size_t>(s.width)*s.height;
 std::vector<std::uint8_t> packed;
 if(s.runLengthEncoded){
  packed.reserve(count);
  for(std::size_t i=0;i+1<encoded.size()&&packed.size()<count;i+=2){
   const std::size_t run=std::min<std::size_t>(encoded[i],count-packed.size());
   packed.insert(packed.end(),run,encoded[i+1]);
  }
 }else packed=encoded;
 std::vector<unsigned char> out(count*4,0); const auto n=std::min(count,packed.size());
 for(std::size_t i=0;i<n;++i){const auto v=packed[i];const unsigned a=v&15;if(!a)continue;const std::size_t p=static_cast<std::size_t>(v>>4)*3;out[i*4]=s.palette[p];out[i*4+1]=s.palette[p+1];out[i*4+2]=s.palette[p+2];out[i*4+3]=static_cast<unsigned char>((a*255+7)/15);}
 return out;
}
inline std::vector<unsigned char> rotateRgba(const std::vector<unsigned char>& src,int w,int h,float degrees,int& ow,int& oh){
 ow=oh=std::max(w,h);std::vector<unsigned char> out(static_cast<std::size_t>(ow)*oh*4,0);const float cx=(w-1)*.5f,cy=(h-1)*.5f,ox=(ow-1)*.5f,oy=(oh-1)*.5f;const float r=degrees*.01745329251994329577f,c=std::cos(r),s=std::sin(r);
 for(int y=0;y<oh;++y)for(int x=0;x<ow;++x){const float dx=x-ox,dy=y-oy;const int sx=static_cast<int>(std::lround(c*dx+s*dy+cx)),sy=static_cast<int>(std::lround(-s*dx+c*dy+cy));if(sx<0||sx>=w||sy<0||sy>=h)continue;const auto si=(static_cast<std::size_t>(sy)*w+sx)*4,di=(static_cast<std::size_t>(y)*ow+x)*4;for(int k=0;k<4;++k)out[di+k]=src[si+k];}
 return out;
}
inline void registerImages(){
 const auto lock=decodeRgba(LockInAim);const auto arrow=decodeRgba(OffscreenArrow);
 pl::modmenu::registerImage("bedaim.esp.lockin",lock,LockInAim.width,LockInAim.height);
 for(int i=0;i<16;++i){int w=0,h=0;const auto rotated=rotateRgba(arrow,OffscreenArrow.width,OffscreenArrow.height,static_cast<float>(i)*22.5f,w,h);const std::string id="bedaim.esp.arrow."+std::to_string(i);pl::modmenu::registerImage(id,rotated,w,h);}
}
}
