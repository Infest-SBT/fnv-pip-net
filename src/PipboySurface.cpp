// Pip-Net experimental engine-owned texture surface; FalloutNV 1.4.0.525 x86.
// Performance guard: no UI tree traversal per frame, animation is opt-in.
#include "PipboySurface.h"
#include "nvse/GameTiles.h"
#include <windows.h>
#include <d3d9.h>
#include <cstdint>
#include <cstring>

namespace pipnet {
namespace {
constexpr uintptr_t kVisibility = 0x011F308Fu;
constexpr uintptr_t kMenuArray = 0x011F350Cu;
constexpr uintptr_t kTileImageVt = 0x0106F01Cu;
constexpr uintptr_t kShaderVt = 0x010B9D28u;
constexpr uintptr_t kSourceVt = 0x0109B9ECu;
constexpr uintptr_t kDx9DataVt = 0x010ED37Cu;
constexpr uintptr_t kSetFloat = 0x00A012D0u;
constexpr unsigned kMinMenu = 0x3E9u, kMapMenu = 0x3FFu;
constexpr unsigned kEdge = 256u;
constexpr ULONGLONG kRetryDelayMs = 1000u;
using TileSetFloat = void(__thiscall*)(Tile*, UInt32, float, bool);

struct SurfaceState {
    Tile* root = nullptr;
    Tile* panel = nullptr;
    Tile* surface = nullptr;
    bool visibilityKnown = false;
    bool lastVisible = false;
    ULONGLONG retryLookupAfter = 0;
    ULONGLONG lastUpload = 0;
};
SurfaceState g_state{};
bool g_pendingHide = false;

bool Readable(const void* p, size_t size = 4) noexcept {
    if (!p || !size) return false;
    MEMORY_BASIC_INFORMATION m{};
    if (VirtualQuery(p, &m, sizeof m) != sizeof m || m.State != MEM_COMMIT ||
        (m.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    uintptr_t start = reinterpret_cast<uintptr_t>(p);
    uintptr_t end = reinterpret_cast<uintptr_t>(m.BaseAddress) + m.RegionSize;
    return start <= end && size <= end - start;
}
void* PtrAt(const void* p, size_t off) noexcept {
    const auto* address = reinterpret_cast<const uint8_t*>(p) + off;
    return Readable(address, sizeof(void*)) ?
        *reinterpret_cast<void* const*>(address) : nullptr;
}
bool IsType(const void* p, uintptr_t vtable) noexcept {
    return Readable(p, sizeof(uintptr_t)) &&
           *reinterpret_cast<const uintptr_t*>(p) == vtable;
}
Tile* MapRoot() noexcept {
    if (!Readable(reinterpret_cast<const void*>(kVisibility + kMapMenu), 1) ||
        *reinterpret_cast<volatile const uint8_t*>(kVisibility + kMapMenu) == 0)
        return nullptr;
    auto*** array = reinterpret_cast<Tile***>(kMenuArray);
    if (!Readable(array, sizeof(void*)) ||
        !Readable(*array, (kMapMenu - kMinMenu + 1) * sizeof(void*)))
        return nullptr;
    return (*array)[kMapMenu - kMinMenu];
}
// Deep search is allowed only on a state transition or a throttled retry.
// Gamebryo's complete UI tree must never be traversed every game frame.
Tile* Find(Tile* tile, const char* name, int depth, int& remaining) noexcept {
    if (!tile || depth > 30 || --remaining <= 0 ||
        !Readable(tile, sizeof(Tile))) return nullptr;
    if (tile->name.m_data && Readable(tile->name.m_data, 1) &&
        std::strcmp(tile->name.m_data, name) == 0) return tile;
    for (auto it = tile->childList.Begin(); !it.End() && remaining > 0; ++it) {
        if (*it && Readable(*it, sizeof(Tile::ChildNode))) {
            if (Tile* found = Find(it->child, name, depth + 1, remaining))
                return found;
        }
    }
    return nullptr;
}
void ObserveRoot(Tile* root) noexcept {
    if (g_state.root == root) return;
    if (g_state.visibilityKnown && g_state.lastVisible)
        g_pendingHide = true;
    g_state = SurfaceState{};
    g_state.root = root;
}
// Cached pointers are never used across MapMenu root changes or while hidden.
// They are checked before dereference, with a bounded retry on failure.
Tile* Resolve(const char* name, Tile*& cache) noexcept {
    if (cache && Readable(cache, sizeof(Tile)) &&
        cache->name.m_data && Readable(cache->name.m_data, 1) &&
        std::strcmp(cache->name.m_data, name) == 0)
        return cache;
    cache = nullptr;
    const ULONGLONG now = GetTickCount64();
    if (!g_state.root || now < g_state.retryLookupAfter) return nullptr;
    int remaining = 8192;
    cache = Find(g_state.root, name, 0, remaining);
    if (!cache) g_state.retryLookupAfter = now + kRetryDelayMs;
    return cache;
}
bool UploadAnimatedProbe() noexcept {
    Tile* tile = Resolve("PN_WebSurface", g_state.surface);
    if (!IsType(tile, kTileImageVt)) return false;
    void* shader = PtrAt(tile, 0x40);
    if (!IsType(shader, kShaderVt)) return false;
    void* source = PtrAt(shader, 0x60);
    if (!IsType(source, kSourceVt)) return false;
    void* data = PtrAt(source, 0x24);
    if (!IsType(data, kDx9DataVt)) return false;
    void* base = PtrAt(data, 0x64);
    if (!Readable(base, sizeof(void*))) return false;
    IDirect3DTexture9* texture = nullptr;
    if (FAILED(static_cast<IDirect3DBaseTexture9*>(base)->QueryInterface(
        IID_IDirect3DTexture9, reinterpret_cast<void**>(&texture))) || !texture)
        return false;
    D3DSURFACE_DESC desc{};
    if (FAILED(texture->GetLevelDesc(0, &desc)) ||
        desc.Width != kEdge || desc.Height != kEdge ||
        desc.Format != D3DFMT_A8R8G8B8) {
        texture->Release();
        return false;
    }
    D3DLOCKED_RECT locked{};
    if (FAILED(texture->LockRect(0, &locked, nullptr, 0))) {
        texture->Release();
        return false;
    }
    if (!locked.pBits || locked.Pitch < static_cast<int>(kEdge * 4u)) {
        texture->UnlockRect(0);
        texture->Release();
        return false;
    }
    static unsigned frame = 0u;
    const unsigned scan = (++frame * 3u) % kEdge;
    for (unsigned y = 0; y < kEdge; ++y) {
        auto* row = static_cast<uint8_t*>(locked.pBits) + y * locked.Pitch;
        for (unsigned x = 0; x < kEdge; ++x) {
            const bool grid = (x % 32u == 0u) || (y % 32u == 0u);
            const bool active = y >= scan && y < scan + 4u;
            const unsigned at = x * 4u;
            row[at + 0] = active ? 90u : (grid ? 35u : 12u);
            row[at + 1] = active ? 255u : (grid ? 160u : 34u);
            row[at + 2] = active ? 125u : (grid ? 70u : 16u);
            row[at + 3] = 255u;
        }
    }
    texture->UnlockRect(0);
    texture->Release();
    return true;
}
} // namespace

void SetSurfaceVisible(bool visible) noexcept {
    Tile* root = MapRoot();
    ObserveRoot(root);
    if (!root) return;
    // XML initializes the image to hidden. When it has never been opened,
    // avoid even the first expensive tree search on the DATA page.
    if (!visible && !g_pendingHide &&
        (!g_state.visibilityKnown || !g_state.lastVisible))
        return;
    if (!g_pendingHide && g_state.visibilityKnown &&
        g_state.lastVisible == visible) return;
    Tile* panel = Resolve("PN_WebPanel", g_state.panel);
    if (!panel) return;
    reinterpret_cast<TileSetFloat>(kSetFloat)(
        panel, Tile::kTileValue_visible, visible ? 1.0f : 0.0f, true);
    g_state.visibilityKnown = true;
    g_state.lastVisible = visible;
    g_pendingHide = false;
}
void DrawSurfaceProbeFrame(int maxFps) noexcept {
    if (!g_state.visibilityKnown || !g_state.lastVisible) return;
    Tile* root = MapRoot();
    if (!root || root != g_state.root) return;
    if (maxFps < 1) maxFps = 1;
    if (maxFps > 10) maxFps = 10;
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG interval = 1000u / static_cast<unsigned>(maxFps);
    if (now - g_state.lastUpload < interval) return;
    g_state.lastUpload = now;
    (void)UploadAnimatedProbe();
}
} // namespace pipnet
