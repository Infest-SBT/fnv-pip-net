// Opt-in experimental native texture probe, FNV 1.4.0.525 x86 only.
// Do not use it with unsupported runtime builds.
#include "PipboySurface.h"
#include "nvse/GameTiles.h"
#include <windows.h>
#include <d3d9.h>
#include <cstdint>
#include <cstring>
namespace pipnet {
namespace {
constexpr uintptr_t kVisibility=0x011F308Fu, kMenuArray=0x011F350Cu;
constexpr uintptr_t kTileImageVt=0x0106F01Cu, kShaderVt=0x010B9D28u;
constexpr uintptr_t kSourceVt=0x0109B9ECu, kDx9DataVt=0x010ED37Cu;
constexpr uintptr_t kSetFloat=0x00A012D0u;
constexpr unsigned kMinMenu=0x3E9, kMapMenu=0x3FF, kEdge=256;
using TileSetFloat=void(__thiscall*)(Tile*, UInt32, float, bool);
bool Readable(const void* p, size_t sz=4) noexcept {
    if (!p || !sz) return false;
    MEMORY_BASIC_INFORMATION m{};
    if (VirtualQuery(p,&m,sizeof m)!=sizeof m || m.State!=MEM_COMMIT ||
        (m.Protect&(PAGE_NOACCESS|PAGE_GUARD))) return false;
    uintptr_t a=reinterpret_cast<uintptr_t>(p);
    uintptr_t b=reinterpret_cast<uintptr_t>(m.BaseAddress)+m.RegionSize;
    return a<=b && sz<=b-a;
}
void* PtrAt(const void* p, size_t off) noexcept {
    const auto* x=reinterpret_cast<const uint8_t*>(p)+off;
    return Readable(x,sizeof(void*)) ? *reinterpret_cast<void* const*>(x) : nullptr;
}
bool IsType(const void* p,uintptr_t vt) noexcept {
    return Readable(p,sizeof(void*)) && *reinterpret_cast<const uintptr_t*>(p)==vt;
}
Tile* MapRoot() noexcept {
    if (!Readable(reinterpret_cast<const void*>(kVisibility+kMapMenu),1) ||
        *reinterpret_cast<volatile const uint8_t*>(kVisibility+kMapMenu)==0) return nullptr;
    auto*** pp=reinterpret_cast<Tile***>(kMenuArray);
    if (!Readable(pp,sizeof(void*)) || !Readable(*pp,(kMapMenu-kMinMenu+1)*sizeof(void*))) return nullptr;
    return (*pp)[kMapMenu-kMinMenu];
}
Tile* Find(Tile* t,const char* name,int depth,int& budget) noexcept {
    if (!t || depth>30 || --budget<=0 || !Readable(t,sizeof(Tile))) return nullptr;
    if (t->name.m_data && Readable(t->name.m_data,1) &&
        std::strcmp(t->name.m_data,name)==0) return t;
    for(auto it=t->childList.Begin();!it.End()&&budget>0;++it) {
        if (*it && Readable(*it,sizeof(Tile::ChildNode))) {
            if(Tile* a=Find(it->child,name,depth+1,budget))return a;
        }
    }
    return nullptr;
}
Tile* FindInMap(const char* name) noexcept {
    int budget=8192;
    return Find(MapRoot(),name,0,budget);
}
bool Upload() noexcept {
    Tile* tile=FindInMap("PN_WebSurface");
    if(!IsType(tile,kTileImageVt)) return false;
    void* shader=PtrAt(tile,0x40);
    if(!IsType(shader,kShaderVt))return false;
    void* source=PtrAt(shader,0x60);
    if(!IsType(source,kSourceVt))return false;
    void* data=PtrAt(source,0x24);
    if(!IsType(data,kDx9DataVt))return false;
    void* base=PtrAt(data,0x64);
    if(!Readable(base,sizeof(void*)))return false;
    IDirect3DTexture9* texture=nullptr;
    if(FAILED(static_cast<IDirect3DBaseTexture9*>(base)->QueryInterface(
        IID_IDirect3DTexture9,reinterpret_cast<void**>(&texture)))||!texture)return false;
    D3DSURFACE_DESC desc{};
    if(FAILED(texture->GetLevelDesc(0,&desc))||desc.Width!=kEdge||
       desc.Height!=kEdge||desc.Format!=D3DFMT_A8R8G8B8) {
        texture->Release();return false;
    }
    D3DLOCKED_RECT lock{};
    if(FAILED(texture->LockRect(0,&lock,nullptr,0))) {
        texture->Release();return false;
    }
    static unsigned frame=0;
    unsigned scan=(++frame/3)%kEdge;
    for(unsigned y=0;y<kEdge;y++) {
        auto* row=static_cast<uint8_t*>(lock.pBits)+y*lock.Pitch;
        for(unsigned x=0;x<kEdge;x++) {
            bool grid=(x%32==0)||(y%32==0);
            bool active=(y>=scan&&y<scan+4);
            unsigned at=x*4;
            row[at+0]=active?90:(grid?35:12);
            row[at+1]=active?255:(grid?160:34);
            row[at+2]=active?125:(grid?70:16);
            row[at+3]=255;
        }
    }
    texture->UnlockRect(0);
    texture->Release();
    return true;
}
} // namespace
void SetSurfaceVisible(bool visible) noexcept {
    static Tile* last=nullptr;
    static bool lastVisible=false;
    Tile* panel=FindInMap("PN_WebPanel");
    if(!panel){last=nullptr;lastVisible=false;return;}
    if(panel==last&&visible==lastVisible)return;
    reinterpret_cast<TileSetFloat>(kSetFloat)(panel,Tile::kTileValue_visible,
                                              visible?1.0f:0.0f,true);
    last=panel;lastVisible=visible;
}
void DrawSurfaceProbeFrame() noexcept { if(MapRoot()) (void)Upload(); }
} // namespace pipnet
