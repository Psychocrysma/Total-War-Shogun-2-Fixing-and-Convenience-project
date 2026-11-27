// Campaign minimap area for enlarged maps (minimap_area in [patches]).
//
// Two getters on the campaign UI object (the global at 0x11BC8FE4) give the minimap its world rectangle:
//   FUN_11121990(this, float out[2])  centre (x, y)
//   FUN_11121a80(this, float out[2])  size (w, h)
// Every minimap user goes through them: the Lua CampaignUI.TheatreMapDimensions (map_overlay.lua, click to
// camera, 0x1114DD67/77) and four more call pairs (0x11066662, 0x110B5401, 0x111B64E1, 0x111ED65F), most
// likely the camera marker, unit and settlement icons and the region overlay.
// Unless the flag this+0x10DE is set or the campaign key is "jap_boshin" (Fall of the Samurai), both return a
// fixed area: centre (-22.5, -80), size 540 x 270, i.e. x -292.5..247.5, y -215..55, whatever the map says.
// Fall of the Samurai instead uses the rectangle of a theatre record: [[[this+0xA98]+0xB0]+0x6E8] points to
// a record whose first four floats are x0, y0, x1, y1. Theatre records have that layout: the campaign's
// theatre table ([[[this+0xA98]+8]+0x1494] -> table: count +0x80, records +0x84, 0xA0 bytes each, name
// (wide string) at +0x5C, rectangle at +0) is what CampaignUI.TheatreMapDimensionsBL (0x1114DE10) reads.
//
// This patch wraps both getters. The game's function runs first; then
//   auto  if the theatre record's rectangle is valid and differs from CA's Sengoku box (-305,-240 .. 270,70),
//         the minimap uses that rectangle (map mods that enlarge the map: Hokkaido add-on, 200% map).
//         CA's maps and the normal Expanded Japan map keep the game's fixed area, which their minimap
//         pictures are drawn for.
//   on    always the theatre record's rectangle (when valid)
//   x0,y0,x1,y1  this rectangle (for a map whose theatre box is not what its minimap picture covers)
//   off   the game's behaviour (the getters are not hooked)
// minimap_debug_log=1 logs, once per campaign, the theatre table (each theatre's name and box) and which
// record the minimap reads, so the first in-game run shows whether that record is the map's theatre box.
#include "common.h"
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cwchar>

namespace {

constexpr uint32_t RVA_CENTRE = 0x01121990, RVA_SIZE = 0x01121A80;
const uint8_t kHead[] = { 0x56, 0x8B, 0x74, 0x24, 0x08, 0x57 };   // push esi; mov esi,[esp+8]; push edi
const float kCaBox[4] = { -305.0f, -240.0f, 270.0f, 70.0f };

typedef float* (__fastcall *GetterFn)(void* self, void* edx, float* out);
GetterFn o_Centre = nullptr, o_Size = nullptr;

enum Mode { M_OFF, M_AUTO, M_ON, M_FIXED };
Mode g_mode = M_AUTO;
float g_fixed[4] = {};
const void* g_loggedRecord = nullptr;
float g_loggedRect[4] = {};

bool Readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || !VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    return reinterpret_cast<const uint8_t*>(p) + n <= reinterpret_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
}
template <class T> bool Get(const void* base, uint32_t off, T* out) {
    const uint8_t* p = static_cast<const uint8_t*>(base) + off;
    if (!base || !Readable(p, sizeof(T))) return false;
    memcpy(out, p, sizeof(T)); return true;
}
bool SaneRect(const float* r) {
    for (int i = 0; i < 4; i++) if (!std::isfinite(r[i]) || std::fabs(r[i]) > 20000.0f) return false;
    return r[2] - r[0] > 10.0f && r[3] - r[1] > 10.0f;
}
bool SameRect(const float* a, const float* b) {
    for (int i = 0; i < 4; i++) if (std::fabs(a[i] - b[i]) > 1.0f) return false;
    return true;
}

// The theatre record the Fall of the Samurai branch reads; null if any step is unreadable.
const float* TheatreRecord(void* self) {
    void* a = nullptr; void* b = nullptr; const float* rec = nullptr;
    if (!Get(self, 0xA98, &a) || !Get(a, 0xB0, &b) || !Get(b, 0x6E8, &rec)) return nullptr;
    if (!rec || !Readable(rec, 16)) return nullptr;
    return rec;
}

void LogTheatres(void* self, const float* used) {
    void* a = nullptr; void* b = nullptr; void* list = nullptr; uint8_t* table = nullptr;
    if (!Get(self, 0xA98, &a) || !Get(a, 8, &b) || !Get(b, 0x1494, &list) || !Get(list, 0, &table)) { Log("minimap_area: theatre table not readable"); return; }
    uint32_t count = 0; uint8_t* recs = nullptr;
    if (!Get(table, 0x80, &count) || !Get(table, 0x84, &recs) || count > 64 || !Readable(recs, count * 0xA0)) { Log("minimap_area: theatre table not readable"); return; }
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* r = recs + i * 0xA0;
        float box[4]; memcpy(box, r, 16);
        uint32_t len = 0; const wchar_t* text = nullptr; char name[64] = "?";
        if (Get(r, 0x5C, &len) && Get(r, 0x64, &text) && text && len < 60 && Readable(text, len * 2)) {
            for (uint32_t k = 0; k < len; k++) name[k] = text[k] < 128 ? static_cast<char>(text[k]) : '?';
            name[len] = 0;
        }
        Log("minimap_area:   theatre %u \"%s\" box %.1f,%.1f .. %.1f,%.1f%s", i, name, box[0], box[1], box[2], box[3],
            reinterpret_cast<const float*>(r) == used ? "  <- the record the minimap reads" : "");
    }
}

// The rectangle the minimap should use, or false to keep the game's answer.
bool Wanted(void* self, float* rect) {
    if (g_mode == M_FIXED) { memcpy(rect, g_fixed, 16); return true; }
    const float* rec = TheatreRecord(self);
    float r[4] = {};
    bool ok = rec != nullptr;
    if (ok) memcpy(r, rec, 16);
    ok = ok && SaneRect(r);
    bool use = ok && (g_mode == M_ON || !SameRect(r, kCaBox));
    if (rec != g_loggedRecord || (ok && !SameRect(r, g_loggedRect))) {
        g_loggedRecord = rec; if (ok) memcpy(g_loggedRect, r, 16);
        if (!rec) Log("minimap_area: no theatre record; the game's fixed area is kept");
        else if (!ok) Log("minimap_area: theatre record %p holds %g,%g .. %g,%g, not a usable box; the game's fixed area is kept", rec, r[0], r[1], r[2], r[3]);
        else if (use) Log("minimap_area: theatre box %.1f,%.1f .. %.1f,%.1f (fixed area -292.5,-215 .. 247.5,55 replaced)", r[0], r[1], r[2], r[3]);
        else Log("minimap_area: theatre box %.1f,%.1f .. %.1f,%.1f equals CA's, left as is", r[0], r[1], r[2], r[3]);
        if (g_cfg.minimap_debug_log) LogTheatres(self, rec);
    }
    if (use) memcpy(rect, r, 16);
    return use;
}

float* __fastcall my_Centre(void* self, void* edx, float* out) {
    float* r = o_Centre(self, edx, out);
    float box[4];
    if (out && Wanted(self, box)) { out[0] = box[0] + (box[2] - box[0]) * 0.5f; out[1] = box[1] + (box[3] - box[1]) * 0.5f; }
    return r;
}
float* __fastcall my_Size(void* self, void* edx, float* out) {
    float* r = o_Size(self, edx, out);
    float box[4];
    if (out && Wanted(self, box)) { out[0] = box[2] - box[0]; out[1] = box[3] - box[1]; }
    return r;
}

bool ParseMode() {
    const std::wstring& v = g_cfg.minimap_area;
    if (v.empty() || v == L"auto") { g_mode = M_AUTO; return true; }
    if (v == L"off" || v == L"0") { g_mode = M_OFF; return true; }
    if (v == L"on" || v == L"1" || v == L"theatre") { g_mode = M_ON; return true; }
    float f[4];
    if (swscanf(v.c_str(), L"%f , %f , %f , %f", &f[0], &f[1], &f[2], &f[3]) == 4 && SaneRect(f)) { memcpy(g_fixed, f, 16); g_mode = M_FIXED; return true; }
    return false;
}

} // namespace

void PatchMinimap_Install() {
    if (!ParseMode()) { Log("minimap_area: value not understood (auto, on, off or x0,y0,x1,y1); off"); return; }
    if (g_mode == M_OFF) { Log("minimap_area: off (the game's fixed Sengoku minimap area)"); return; }
    if (!g_engine.knownBuild) { Log("minimap_area: skipped, engine build not verified"); return; }
    uint8_t* base = reinterpret_cast<uint8_t*>(g_engine.base);
    if (memcmp(base + RVA_CENTRE, kHead, sizeof(kHead)) || memcmp(base + RVA_SIZE, kHead, sizeof(kHead))) { Log("minimap_area: getter bytes differ; NOT applied"); return; }
    o_Centre = reinterpret_cast<GetterFn>(InstallDetour(base + RVA_CENTRE, reinterpret_cast<void*>(my_Centre), sizeof(kHead)));
    o_Size   = reinterpret_cast<GetterFn>(InstallDetour(base + RVA_SIZE,   reinterpret_cast<void*>(my_Size),   sizeof(kHead)));
    if (g_mode == M_FIXED)
        Log("minimap_area: %s - the minimap uses %.1f,%.1f .. %.1f,%.1f from tws2fix.ini", (o_Centre && o_Size) ? "on" : "FAILED", g_fixed[0], g_fixed[1], g_fixed[2], g_fixed[3]);
    else
        Log("minimap_area: %s (%s) - the minimap follows the map's theatre box%s", (o_Centre && o_Size) ? "on" : "FAILED",
            g_mode == M_AUTO ? "auto" : "on", g_mode == M_AUTO ? " when it differs from CA's" : "");
}
