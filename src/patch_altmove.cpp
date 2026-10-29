// Alt group-move (Total War: WARHAMMER-style block move) for the battle map.
//
// Hold Alt and press-and-hold the left button on the ground: the selected units move there as a block,
// each keeping its offset from the others and its facing. While the button is held, holding Ctrl and
// moving the mouse rotates the block about its centre. Releasing the button issues the order.
//
// How it hooks in (engine names at base 0x10000000, May 2023 build):
//   The click and drag move orders go through two placement builders, FUN_10400870 (click) and
//   FUN_10401c50 (drag), for the ground preview and again in the command executor. For a plain
//   multi-selection both end in the selection formation's command builder FUN_1042da90, which lays the
//   line and then, per unit entry, turns the formation slot into a world point (FUN_1047d120), hands it
//   to the unit's placement (FUN_1044e3a0) and fills a 0x20-byte order record (FUN_1040bd90). This file
//   does not touch the formation code: it latches the selection's layout when the Alt-hold begins, and
//   while a FUN_1042da90 call is running it replaces each unit's slot point with centre + offset (and
//   the record's location and orientation to match). The preview and the executor run the same code, so
//   what is shown is what is ordered.
//
// Engine facts used: the click builder pushes unit + 0xC as the unit's placement handle, whose virtual
// functions give the unit's located position and heading (see HandleOk below); the formation set holds
// formations at +0x30 (count +0x2C), each with 0x1C-byte unit entries at +0x24 (count +0x20), the handle
// at entry+0; the params block passed to FUN_1042da90 holds the click
// location at +0, the "located" flag at +0x14 and the orientation u16 at +0x18.
// Directions come from the engine's own angle table at 0x11BD9560 (8 bytes per u16 angle: A, B), used
// exactly as FUN_1042da90 uses it (lateral = (B, -A)).
//
// Single player only: the executor runs the same detour, so a replay or an opponent without it desyncs.
#include "common.h"
#include "shaku/hook.h"
#include "shaku/gamemem.h"
#include <cmath>
#include <cstring>

namespace {

constexpr uint32_t RVA_CMD_BUILD   = 0x0042DA90;   // FUN_1042da90: selection formation -> per-unit records
constexpr uint32_t RVA_RECORD      = 0x0040BD90;   // FUN_1040bd90: fills one unit order record
constexpr uint32_t RVA_SUBMIT      = 0x00419B40;   // FUN_10419b40: called by both builders after the command build
constexpr uint32_t RVA_CLICK_BLD   = 0x00400870;   // FUN_10400870: click builder (entry: resets the drag flag)
constexpr uint32_t RVA_DRAG_BLD    = 0x00401C50;   // FUN_10401c50: drag builder (entry: latches orientation and width)
constexpr uint32_t RVA_ANGLE_TABLE = 0x01BD9560;   // float A, float B per u16 angle
constexpr uint32_t RVA_SLOT_POINT  = 0x0047D120;   // FUN_1047d120: formation slot -> world point (x, z)
const uint8_t kSlotPointHead[] = { 0xF3, 0x0F, 0x10, 0x69, 0x10, 0x83, 0xEC, 0x08 };   // movss xmm5,[ecx+0x10]; sub esp,8

enum { H_CMD_BUILD = 0, H_RECORD, H_SUBMIT, H_CLICK, H_DRAG, H_PLACE, HOOK_COUNT };

const HookDef kHooks[HOOK_COUNT] = {
    { "FUN_1042da90", "selection command build", RVA_CMD_BUILD, 6, { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0 }, -1, false },
    { "FUN_1040bd90", "unit order record",       RVA_RECORD,    8, { 0x8B, 0x54, 0x24, 0x08, 0x8B, 0x44, 0x24, 0x04 }, -1, false },
    { "FUN_10419b40", "command submit",          RVA_SUBMIT,    8, { 0x56, 0x8B, 0x74, 0x24, 0x08, 0x57, 0x8B, 0xF9 }, -1, false },
    { "FUN_10400870", "click builder",           RVA_CLICK_BLD, 10, { 0x8B, 0x44, 0x24, 0x20, 0x81, 0xEC, 0xE4, 0x00, 0x00, 0x00 }, -1, false },
    { "FUN_10401c50", "drag builder",            RVA_DRAG_BLD,  10, { 0x8B, 0x44, 0x24, 0x24, 0x81, 0xEC, 0x0C, 0x01, 0x00, 0x00 }, -1, false },
    { "FUN_1044e3a0", "unit placement",          0x0044E3A0,     6, { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0 }, -1, false },
};

// pushad/pushfd context offsets (same stub as every fix in src/shaku)
constexpr int CTX_EAX = 0x20, CTX_ECX = 0x1C, CTX_ARG1 = 0x28, CTX_ARG2 = 0x2C, CTX_ARG3 = 0x30, CTX_ARG4 = 0x34;

uint8_t* g_base = nullptr;
HookSlot g_slots[HOOK_COUNT];


struct Entry { uint32_t handle; uint32_t ref; float offX, offZ; uint16_t heading; float width; };
constexpr int MAX_UNITS = 64;
Entry g_units[MAX_UNITS];
int g_count = 0;
bool g_latched = false;         // Alt-hold in progress (or its executor call pending)
bool g_rewrite = false;         // inside a FUN_1042da90 call that we are rewriting
bool g_isDrag = false;
bool g_sawDrag = false;
bool g_clickMoves = false, g_haveDragEnd = false; char g_source = '-';
bool g_frozen = false; float g_shiftX = 0, g_shiftZ = 0;   // Ctrl rotation: block held in place, then the kept shift
float g_clickX = 0, g_clickZ = 0, g_dragEndX = 0, g_dragEndZ = 0;
DWORD g_lastTrace = 0; int g_traces = 0;         // a drag build has run in this hold: from then on only drag builds move the centre
uint16_t g_dragOrient = 0;
float g_dragWidth = 0.0f;
int g_delta = 0;                // rotation, u16 angle units
uint16_t g_baseOrient = 0;      // the selection's facing when the hold began (the click builder's orientation)
int g_lastMouseX = 0;
bool g_ctrlWas = false;
bool g_lmbWas = false;
bool g_logged = false;
bool g_issued = false;          // the release has been logged for this hold
int g_postBuilds = 0;           // builds seen after the release (UI command + executor), for the log
float g_centreX = 0, g_centreZ = 0;
float g_startX = 0, g_startZ = 0;     // the press point (the first build of the hold)
double g_accX = 0, g_accZ = 0; int g_accN = 0;   // the engine's own slot points of the build in progress
float g_midX = 0, g_midZ = 0; bool g_midOk = false;   // their centre in the previous build
DWORD g_releaseTick = 0;
long g_cOrders = 0, g_cRewrites = 0;

inline uint32_t R32(const uint8_t* ctx, int off) { return *reinterpret_cast<const uint32_t*>(ctx + off); }
inline void W32(uint8_t* ctx, int off, uint32_t v) { *reinterpret_cast<uint32_t*>(ctx + off) = v; }
inline bool KeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

bool AngleVec(uint16_t o, float* a, float* b) {
    const float* t = reinterpret_cast<const float*>(g_base + RVA_ANGLE_TABLE) + o * 2;
    *a = t[0]; *b = t[1];
    return std::isfinite(*a) && std::isfinite(*b) && (*a != 0.0f || *b != 0.0f);
}
// Angle (u16) whose table vector points along (x, z), using the table's own basis at 0 and a quarter turn.
uint16_t AngleOf(float x, float z) {
    float b0, b1, q0, q1;
    if (!AngleVec(0, &b0, &b1) || !AngleVec(16384, &q0, &q1)) return 0;
    float c = x * b0 + z * b1, s = x * q0 + z * q1;
    double th = atan2((double)s, (double)c);
    if (th < 0) th += 6.283185307179586;
    return (uint16_t)((long)(th / 6.283185307179586 * 65536.0 + 0.5) & 0xFFFF);
}
// Rotate (x, z) by delta angle units: re-express as length along the table vector at (angle + delta).
void Rotate(float x, float z, int delta, float* ox, float* oz) {
    float len = sqrtf(x * x + z * z);
    if (len < 1e-4f || delta == 0) { *ox = x; *oz = z; return; }
    uint16_t o = (uint16_t)((AngleOf(x, z) + delta) & 0xFFFF);
    float a, b; AngleVec(o, &a, &b);
    *ox = a * len; *oz = b * len;
}

// The unit's placement handle (unit + 0xC, vtable 0x115BB2D0) answers for the unit itself through its
// virtual functions, exactly as the engine's formation code asks it:
//   slot +0x08 (FUN_1017af30): void (float out[2])   -> the unit's located position, x and z
//   slot +0x18 (FUN_1017af60): void (uint16_t* out)  -> the unit's heading, the engine's u16 angle
//   slot +0x28 (FUN_1017af80): returns handle - 0xC   (the unit)
// (both read [handle+0x434]: +0x10 the located point, +0x24 the width, +0x28 the heading.)
constexpr uint32_t RVA_HANDLE_VTABLE = 0x015BB2D0;
typedef void (__thiscall* HandlePosFn)(void* h, float* out2);
typedef void (__thiscall* HandleHeadFn)(void* h, uint16_t* out);
bool HandleOk(uint32_t h) {
    uint32_t vt = 0;
    return ReadDword(h, &vt) && vt == static_cast<uint32_t>(g_engine.base + RVA_HANDLE_VTABLE);
}
bool UnitCentre(uint32_t h, float* x, float* z) {
    if (!HandleOk(h)) return false;
    void** vt = *reinterpret_cast<void***>(h);
    float out[2] = { 0, 0 };
    reinterpret_cast<HandlePosFn>(vt[2])(reinterpret_cast<void*>(h), out);
    if (!std::isfinite(out[0]) || !std::isfinite(out[1])) return false;
    *x = out[0]; *z = out[1];
    return true;
}
// slot +0x00 (FUN_1017af90): float () -> the unit's current frontage width ([handle+0x434]+0x24)
typedef float (__thiscall* HandleWidthFn)(void* h);
float UnitWidth(uint32_t h) {
    void** vt = *reinterpret_cast<void***>(h);
    float w = reinterpret_cast<HandleWidthFn>(vt[0])(reinterpret_cast<void*>(h));
    return (std::isfinite(w) && w > 0.5f && w < 1000.0f) ? w : 0.0f;
}
// slot +0x1C (FUN_1017ad60): returns [handle+0x298], the unit object the placement is called with
typedef uint32_t (__thiscall* HandleRefFn)(void* h);
uint32_t UnitRef(uint32_t h) { void** vt = *reinterpret_cast<void***>(h); return reinterpret_cast<HandleRefFn>(vt[7])(reinterpret_cast<void*>(h)); }
uint16_t UnitHeading(uint32_t h) {
    void** vt = *reinterpret_cast<void***>(h);
    uint16_t o = 0;
    reinterpret_cast<HandleHeadFn>(vt[6])(reinterpret_cast<void*>(h), &o);
    return o;
}
void DumpHandle(uint32_t h) {
    char line[400]; int n = 0;
    n += snprintf(line + n, sizeof(line) - n, "altmove: handle %08X, dwords at -0xC:", h);
    for (int off = -0xC; off < 0x40 && n < (int)sizeof(line) - 12; off += 4) { uint32_t v = 0; ReadDword(h + off, &v); n += snprintf(line + n, sizeof(line) - n, " %08X", v); }
    DLog("%s", line);
}

// The formation set (ECX of FUN_1042da90) holds formations at +0x30 (count +0x2C); each formation holds
// its unit entries at +0x24 (count +0x20), 0x1C bytes each, the unit's placement handle at entry+0 - the
// loop in FUN_1042d560. A plain multi-selection is one formation with all the units.
int g_lastSeen = 0;   // unit entries seen on the last latch attempt (for the log)
bool Latch(uint32_t fset) {
    uint32_t nf = 0, farr = 0;
    g_lastSeen = 0;
    if (!ReadDword(fset + 0x2C, &nf) || !ReadDword(fset + 0x30, &farr)) return false;
    if (nf == 0 || nf > 64 || !PtrPlausible(farr)) return false;
    double cx = 0, cz = 0; int k = 0;
    for (uint32_t f = 0; f < nf; f++) {
        uint32_t fo = 0; if (!ReadDword(farr + 4 * f, &fo) || !PtrPlausible(fo)) return false;
        uint32_t n = 0, arr = 0;
        if (!ReadDword(fo + 0x20, &n) || !ReadDword(fo + 0x24, &arr)) return false;
        if (n > (uint32_t)MAX_UNITS || (n && !PtrPlausible(arr))) return false;
        for (uint32_t i = 0; i < n; i++) {
            if (k >= MAX_UNITS) return false;
            uint32_t h = 0; if (!ReadDword(arr + 0x1C * i, &h) || !PtrPlausible(h)) return false;
            g_lastSeen++;
            float x, z;
            if (!UnitCentre(h, &x, &z)) { static bool dumped = false; if (!dumped) { dumped = true; DumpHandle(h); } return false; }
            g_units[k].handle = h; g_units[k].offX = x; g_units[k].offZ = z; g_units[k].heading = UnitHeading(h); g_units[k].width = UnitWidth(h); g_units[k].ref = UnitRef(h);
            cx += x; cz += z; k++;
        }
    }
    if (k < 2) return false;
    cx /= k; cz /= k;
    for (int i = 0; i < k; i++) { g_units[i].offX -= (float)cx; g_units[i].offZ -= (float)cz; }
    g_count = k; g_delta = 0; g_ctrlWas = false;
    POINT p; GetCursorPos(&p); g_lastMouseX = p.x;
    return true;
}

void DumpMeasurement();

// --- hook handlers ---------------------------------------------------------------------------------
extern "C" void __stdcall altmove_OnClick(const uint8_t*) { g_isDrag = false; }
extern "C" void __stdcall altmove_OnDrag(const uint8_t* ctx) {
    g_isDrag = true;
    g_dragOrient = (uint16_t)R32(ctx, CTX_ARG3);
    memcpy(&g_dragWidth, ctx + CTX_ARG4, 4);
}

extern "C" void __stdcall altmove_OnCmdBuild(const uint8_t* ctx) {
    g_rewrite = false;
    if (!g_cfg.alt_group_move) return;
    bool lmb = KeyDown(VK_LBUTTON), alt = KeyDown(VK_MENU);
    uint32_t fset = R32(ctx, CTX_ECX), army = R32(ctx, CTX_ARG2), params = R32(ctx, CTX_ARG3);
    if (!PtrPlausible(fset) || !PtrPlausible(params)) return;
    bool newPress = lmb && !g_lmbWas;
    g_lmbWas = lmb;
    if (g_latched && newPress) g_latched = false;                 // a new press: the old hold is over
    if (g_latched && !lmb && GetTickCount() - g_releaseTick > 1500) g_latched = false;   // stale
    if (!g_latched) {
        (void)alt;
        if (!(lmb && PatchAltInput_SwapActive())) return;
        if (!Latch(fset)) {
            static DWORD lastFail = 0;
            if (GetTickCount() - lastFail > 2000) { lastFail = GetTickCount(); uint32_t n = 0; ReadDword(fset + 0x2C, &n); DLog("altmove: Alt+hold seen but no latch (%u formation(s), %d unit entries seen; needs 2..%d units with readable positions)", n, g_lastSeen, MAX_UNITS); }
            return;
        }
        g_latched = true; g_logged = false; g_issued = false; g_postBuilds = 0;
        MemcpyTry(&g_startX, params + 0, 4); MemcpyTry(&g_startZ, params + 8, 4);
        g_midOk = false; g_accN = 0; g_sawDrag = false;
        g_clickMoves = false; g_haveDragEnd = false; g_source = '-'; g_traces = 0; g_lastTrace = 0;
        g_frozen = false; g_shiftX = g_shiftZ = 0;
        g_baseOrient = 0; MemcpyTry(&g_baseOrient, params + 0x18, 2);
        { static int dumps = 0; if (dumps < 2) { dumps++; DumpMeasurement(); } }
        DLog("altmove: hold began, %d units latched (first unit: offset %.1f, %.1f from the centre, heading %u, width %.1f; engine orientation %u, drag=%d)", g_count, g_units[0].offX, g_units[0].offZ, g_units[0].heading, g_units[0].width, g_baseOrient, g_isDrag ? 1 : 0);
    }
    if (lmb) g_releaseTick = GetTickCount();
    // rotation while Ctrl is held during the hold
    POINT p; GetCursorPos(&p);
    if (lmb && KeyDown(VK_CONTROL)) {
        if (g_ctrlWas) g_delta += (p.x - g_lastMouseX) * g_cfg.alt_rotate_speed;
        g_ctrlWas = true;
    } else g_ctrlWas = false;
    g_lastMouseX = p.x;
    // The engine's own slot points of the previous build: their centre is the middle of the line it lays
    // (for a drag, the line from the press point to the cursor).
    // (Only drag builds are accumulated, so a click build in the same frame cannot pull it back.)
    if (g_accN) { g_midX = (float)(g_accX / g_accN); g_midZ = (float)(g_accZ / g_accN); g_midOk = true; }
    g_accX = g_accZ = 0; g_accN = 0;
    if (g_isDrag) g_sawDrag = true;
    // Block centre, frozen at the release so the command and the executor use what was shown:
    // a click = the click point; a drag = the cursor end = 2 * middle of the engine's line - press point.
    float sx = 0, sz = 0; MemcpyTry(&sx, params + 0, 4); MemcpyTry(&sz, params + 8, 4);
    // Where the cursor is. Two sources, chosen per hold:
    //  A. click builds that keep running during the hold with a moving location: that location is the
    //     engine's own ground point under the cursor;
    //  B. otherwise the drag build: FUN_10401c50 lays the line centred on its point, from point - L*w/2
    //     to point + L*w/2 with L = (B, -A) of the angle table at the drag orientation; the cursor is the
    //     end farther from the press point.
    if (lmb) {
        if (!g_isDrag) {
            if (fabsf(sx - g_startX) > 1.0f || fabsf(sz - g_startZ) > 1.0f) g_clickMoves = true;
            g_clickX = sx; g_clickZ = sz;
        } else {
            float a, b;
            if (AngleVec(g_dragOrient, &a, &b)) {
                // The cursor end is always point + L * w/2 (L = (B, -A) at the drag orientation): checked in the
                // 0.7.4 traces against every build whose line runs press -> cursor, and it holds for the builds
                // whose line is centred on the press point as well. (0.7.4/0.7.5 picked "the end farther from
                // the press point", which is ambiguous for the centred lines and could lock onto the mirror end.)
                g_dragEndX = sx + b * g_dragWidth * 0.5f; g_dragEndZ = sz - a * g_dragWidth * 0.5f;
                g_haveDragEnd = true;
            }
        }
        float cx, cz;
        if (g_clickMoves) { cx = g_clickX; cz = g_clickZ; g_source = 'A'; }
        else if (g_haveDragEnd) { cx = g_dragEndX; cz = g_dragEndZ; g_source = 'B'; }
        else { cx = g_startX; cz = g_startZ; g_source = '-'; }
        // Ctrl held: the block stays where it is and only turns (about its own middle, since the unit
        // offsets are relative to it). On release of Ctrl the block resumes following the cursor from
        // where it stands: the cursor-to-block shift is kept, so it does not jump.
        bool ctrl = KeyDown(VK_CONTROL);
        if (ctrl) { if (!g_frozen) { g_frozen = true; } }
        else {
            if (g_frozen) { g_frozen = false; g_shiftX = g_centreX - cx; g_shiftZ = g_centreZ - cz; }
            g_centreX = cx + g_shiftX; g_centreZ = cz + g_shiftZ;
        }
        DWORD now = GetTickCount();
        if (now - g_lastTrace >= 250 && g_traces < 40) {
            g_lastTrace = now; g_traces++;
            POINT cp; GetCursorPos(&cp);
            DLog("alttrace: src %c centre %.1f,%.1f | press %.1f,%.1f | click %.1f,%.1f%s | drag pt %.1f,%.1f o %u w %.1f end %.1f,%.1f | ctrl %d | screen %ld,%ld",
                g_source, g_centreX, g_centreZ, g_startX, g_startZ, g_clickX, g_clickZ, g_clickMoves ? "(moves)" : "",
                g_isDrag ? sx : 0.0f, g_isDrag ? sz : 0.0f, g_dragOrient, g_dragWidth, g_dragEndX, g_dragEndZ, KeyDown(VK_CONTROL) ? 1 : 0, cp.x, cp.y);
        }
    }
    // The slot-point replacement needs nothing else. (The executor's build, FUN_10400040, reaches this
    // function with a different second argument than the UI's FUN_10400870, so nothing below may bail.)
    g_rewrite = true;
    g_cOrders++;
    if (!g_logged) { g_logged = true; DLog("altmove: centre %.1f, %.1f (click point %.1f, %.1f; drag=%d orient=%u width=%.1f), delta=%d", g_centreX, g_centreZ, sx, sz, g_isDrag ? 1 : 0, g_dragOrient, g_dragWidth, g_delta); }
    if (!lmb && g_postBuilds < 4) { g_postBuilds++; DLog("altmove: post-release build %d: fset=%08X arg2=%08X params=%08X", g_postBuilds, fset, army, params); }
}

// --- measurement (0.7.2) ---------------------------------------------------------------------------
// For a normal right-button order, what the engine computes per unit (captured while the right button is
// physically held; the last values before the release are the order's): the slot point and its inputs,
// and the order record's location, orientation and width. Printed next to this file's own reading of the
// same units at the next Alt press, to fix the relation between the two.
struct Seen { uint32_t h; float out[2], origin[2], slot[5], box[4], recLoc[2], recW; uint32_t orient, recO; bool haveSlot, haveRec; };
Seen g_seen[MAX_UNITS]; int g_seenN = 0;
Seen* SeenFor(uint32_t h, bool create) {
    for (int i = 0; i < g_seenN; i++) if (g_seen[i].h == h) return &g_seen[i];
    if (!create || g_seenN >= MAX_UNITS) return nullptr;
    Seen* e = &g_seen[g_seenN++]; memset(e, 0, sizeof(*e)); e->h = h; return e;
}
void DumpMeasurement() {
    for (int i = 0; i < g_count; i++) {
        uint32_t h = g_units[i].handle;
        float x = 0, z = 0; UnitCentre(h, &x, &z);
        DLog("altmeasure: unit %d: read pos %.2f, %.2f heading %u width %.2f", i, x, z, g_units[i].heading, g_units[i].width);
        const Seen* e = SeenFor(h, false);
        if (!e) { DLog("altmeasure: unit %d: no engine order seen (give it a plain right-button order first)", i); continue; }
        if (e->haveSlot) DLog("altmeasure: unit %d: engine slot out %.2f, %.2f; origin %.2f, %.2f; orient %u; slot %.2f %.2f %.2f %.2f %.2f; box %.2f %.2f %.2f %.2f",
            i, e->out[0], e->out[1], e->origin[0], e->origin[1], e->orient, e->slot[0], e->slot[1], e->slot[2], e->slot[3], e->slot[4], e->box[0], e->box[1], e->box[2], e->box[3]);
        if (e->haveRec) DLog("altmeasure: unit %d: engine record loc %.2f, %.2f; orient %u; width %.2f", i, e->recLoc[0], e->recLoc[1], e->recO, e->recW);
    }
}

// The slot -> world point of a unit. FUN_1042d560 calls it per unit entry (thiscall: this = the
// formation; out[2], origin[2], orientation, entry+4), and everything downstream - the move itself
// (FUN_1044e3a0), the record, the ghost - starts from that point. During the block move the point is
// replaced by centre + R(delta) * offset for the unit of the entry (its handle sits at entry+0).
typedef void (__fastcall* SlotPointFn)(void* self, void* edx, float* out, const float* origin, uint32_t orient, float* slot);
SlotPointFn o_SlotPoint = nullptr;
long g_cSlots = 0;
void __fastcall my_SlotPoint(void* self, void* edx, float* out, const float* origin, uint32_t orient, float* slot) {
    o_SlotPoint(self, edx, out, origin, orient, slot);
    if (!g_rewrite && slot && KeyDown(VK_RBUTTON)) {
        if (Seen* e = SeenFor(reinterpret_cast<const uint32_t*>(slot)[-1], true)) {
            e->out[0] = out[0]; e->out[1] = out[1]; e->origin[0] = origin[0]; e->origin[1] = origin[1];
            e->orient = orient & 0xFFFF; memcpy(e->slot, slot, sizeof(e->slot));
            MemcpyTry(e->box, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self)) + 8, 16); e->haveSlot = true;
        }
    }
    if (!g_rewrite || !slot) return;
    if (g_isDrag) { g_accX += out[0]; g_accZ += out[1]; g_accN++; }
    uint32_t handle = reinterpret_cast<const uint32_t*>(slot)[-1];
    int i = 0; while (i < g_count && g_units[i].handle != handle) i++;
    if (i == g_count) return;
    float ox, oz; Rotate(g_units[i].offX, g_units[i].offZ, g_delta, &ox, &oz);
    out[0] = g_centreX + ox; out[1] = g_centreZ + oz;
    // The unit keeps its own frontage: the slot's width (entry+0x10, read after this call by the
    // placement FUN_1044e3a0 and by the order record) is set to the width the unit had at the press.
    // The unit keeps its own frontage (measured in 0.7.2: the handle's width equals the engine's slot
    // and record width for the same unit).
    if (g_units[i].width > 0.0f) slot[3] = g_units[i].width;
    g_cSlots++;
}

// The record's location is already derived from the replaced slot point; only the orientation (the
// unit's own heading plus the rotation) is written here.
extern "C" void __stdcall altmove_OnRecord(uint8_t* ctx) {
    if (!g_rewrite) {
        if (KeyDown(VK_RBUTTON))
            if (Seen* e = SeenFor(R32(ctx, CTX_ARG1), true)) {
                uint32_t loc = R32(ctx, CTX_ARG2);
                if (MemcpyTry(&e->recLoc[0], loc, 4) && MemcpyTry(&e->recLoc[1], loc + 8, 4)) {
                    e->recO = R32(ctx, CTX_ARG3) & 0xFFFF; memcpy(&e->recW, ctx + CTX_ARG4, 4); e->haveRec = true;
                }
            }
        return;
    }
    uint32_t handle = R32(ctx, CTX_ARG1);
    int i = 0; while (i < g_count && g_units[i].handle != handle) i++;
    if (i == g_count) return;
    W32(ctx, CTX_ARG3, (uint16_t)((g_units[i].heading + g_delta) & 0xFFFF));
    g_cRewrites++;
}

// The unit's placement (ghost and move): FUN_1044e3a0(this, out, unitRef, location*, orientation u16,
// width, depth). The engine passes the whole formation's facing; during the block move each unit gets its
// own heading plus the rotation, so the ghosts turn with the block.
long g_cPlaced = 0;
extern "C" void __stdcall altmove_OnPlace(uint8_t* ctx) {
    if (!g_rewrite) return;
    uint32_t ref = R32(ctx, CTX_ARG2);
    int i = 0; while (i < g_count && g_units[i].ref != ref) i++;
    if (i == g_count) return;
    W32(ctx, CTX_ARG4, (uint16_t)((g_units[i].heading + g_delta) & 0xFFFF));
    g_cPlaced++;
}

// The order is built twice after the release: once by the UI (the command that goes into the queue) and
// once more when the queue executes it. The latch therefore stays until a new press or the 1.5 s stale
// timeout, so both builds get the block layout; only the first is logged.
extern "C" void __stdcall altmove_OnSubmit(const uint8_t*) {
    if (g_rewrite && !KeyDown(VK_LBUTTON)) {
        if (!g_issued) { g_issued = true; DLog("altmove: order issued (rotation %d; slot points replaced %ld, placements turned %ld, unit records rewritten %ld so far)", g_delta, g_cSlots, g_cPlaced, g_cRewrites); }
    }
    g_rewrite = false;
}

extern "C" uint8_t* altmove_Trampolines[HOOK_COUNT];
uint8_t* altmove_Trampolines[HOOK_COUNT] = {};
HOOK_DETOUR(altmove_D0, altmove_OnCmdBuild, altmove_Trampolines, 0)
HOOK_DETOUR(altmove_D1, altmove_OnRecord,   altmove_Trampolines, 1)
HOOK_DETOUR(altmove_D2, altmove_OnSubmit,   altmove_Trampolines, 2)
HOOK_DETOUR(altmove_D3, altmove_OnClick,    altmove_Trampolines, 3)
HOOK_DETOUR(altmove_D4, altmove_OnDrag,     altmove_Trampolines, 4)
HOOK_DETOUR(altmove_D5, altmove_OnPlace,    altmove_Trampolines, 5)
const HookDetour g_detours[HOOK_COUNT] = { altmove_D0, altmove_D1, altmove_D2, altmove_D3, altmove_D4, altmove_D5 };

} // namespace

void PatchAltMove_Install() {
    if (!g_cfg.alt_group_move) { Log("alt_group_move: off"); return; }
    if (!g_engine.knownBuild && !g_cfg.allow_unknown_build) { Log("alt_group_move: skipped, engine build not verified"); return; }
    Mem_Init();
    g_base = reinterpret_cast<uint8_t*>(g_engine.base);
    for (int h = 0; h < HOOK_COUNT; h++)
        if (!Hook_SiteMatches(g_base, kHooks[h])) { Log("alt_group_move: REFUSED, bytes at RVA 0x%06X (%s) differ", kHooks[h].rva, kHooks[h].func); return; }
    if (memcmp(g_base + RVA_SLOT_POINT, kSlotPointHead, sizeof(kSlotPointHead)) != 0) { Log("alt_group_move: REFUSED, slot-point function differs"); return; }
    o_SlotPoint = reinterpret_cast<SlotPointFn>(InstallDetour(g_base + RVA_SLOT_POINT, reinterpret_cast<void*>(my_SlotPoint), sizeof(kSlotPointHead)));
    if (!o_SlotPoint) { Log("alt_group_move: REFUSED, slot-point detour"); return; }
    if (!Hook_Prepare(g_base, kHooks, HOOK_COUNT, nullptr, altmove_Trampolines, g_slots)) { Log("alt_group_move: REFUSED, trampoline memory"); return; }
    Hook_Patch(g_detours, g_slots, HOOK_COUNT);
    Log("alt_group_move: ON, hold Alt + left button to move the selection as a block, Ctrl while holding to rotate (speed %d)", g_cfg.alt_rotate_speed);
}

void PatchAltMove_Remove() {
    if (!g_base) return;
    Hook_Remove(g_slots, HOOK_COUNT);
    Log("altmove: block orders %ld, slot points replaced %ld, unit records rewritten %ld", g_cOrders, g_cSlots, g_cRewrites);
}
