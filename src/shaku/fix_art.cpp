// fix_art.cpp -- artillery targeting fix: artillery aims at the middle of the target unit.
//
// From ShakuEight/Shogun2-fix (fixes/artillery-targeting-fix/src/artfix.cpp), Copyright 2026 ShakuEight,
// Apache License 2.0 (third_party/LICENSE-APACHE-2.0). Ported to this project: the fix itself is
// unchanged; the development-only parts (toggle key, beeps, environment variables, the research probe
// with its two extra hooks, write watchpoint and CSV files, harness exports) are removed, the naked
// detour comes from hook.h's GCC macro, and the log lines go to tws2fix.log.
//
// Fault: ordered to fire at an enemy unit, each artillery piece aims at one soldier of that unit,
// roughly the soldier in front of the gun, so a battery standing off to one side of its target aims
// every gun at the near end of the enemy line, where it hits the fewest men.
//
// Engine decode (ShakuEight's notes):
// - An artillery piece is an entity whose class sends fire orders through the engine fire action
//   (vtable slot +0x1B0 = FUN_1045a2e0; six engine classes, no infantry or cavalry class).
// - The fire action (vtable 0x115E7D94, constructor FUN_10409980, created by FUN_1045a2e0 from a fire
//   order) holds the target record at +0x20 (0x18 bytes: offset x/y/z, entity, object, transform) and
//   its gun at +0x60. On its first update ([action+0x7C] == 0) FUN_1047fd90 copies the record into the
//   gun (FUN_1045a8a0 or FUN_1044fb60, into gun+0x71C), and the gun turns, aims and fires at
//   record.entity + record.offset (the aim point comes from the record getter FUN_103a6850, read by the
//   launch check FUN_10420d00 once per shot).
// - Unit of a soldier: [soldier+0x3B4]; soldier array [unit+0x1C], count [unit+0x18]; entity
//   position at +0x48 (x, y, z; +0x50 = z).
//
// The fix hooks the entry of FUN_1047fd90 (stolen bytes PUSH EBX; PUSH ESI; MOV ESI,ECX; CMP byte
// [ESI+0x7C],0, 8 bytes, no branch into them). When [action+0x7C] == 0, the gun [action+0x60] is of
// an engine class (its vtable slot +0x1B0 is FUN_1045a2e0) and the record at action+0x20 targets an
// entity with no object and no transform, it takes that soldier's unit [entity+0x3B4], requires that
// the unit's soldier array holds him, computes the centroid (x / z) of all members and writes the
// member nearest to it into the record's entity field. Offset (aim height), object and transform
// stay; every later use (the copy, turning, launch) sees the changed record.
// Self-check: a soldier not found in his unit's array is left alone and counted; if the first 20
// artillery targets all fail that check, the fix switches off for the session. All reads and the one
// write are guarded; the hook runs on game worker threads and takes no locks.

#include "log.h"
#include "hook.h"
#include "gamemem.h"
#include "stack_offsets_art.h"
#include <windows.h>
#include <cstdint>
#include <cstring>

static uint32_t g_base = 0;

// ---------------------------------------------------------------------------
// Hook table: the one fix hook, FUN_1047fd90 entry (engine fire action hands its target to the gun)
// ---------------------------------------------------------------------------

enum { H_ORDERCOPY = 0, HOOK_COUNT };

static const HookDef g_hookDefs[HOOK_COUNT] = {
    { "FUN_1047fd90", "engine fire action update", 0x47FD90, 8,
      { 0x53, 0x56, 0x8B, 0xF1, 0x80, 0x7E, 0x7C, 0x00 }, -1, false },
      // PUSH EBX; PUSH ESI; MOV ESI,ECX; CMP byte [ESI+0x7C],0
};

static HookSlot g_slots[HOOK_COUNT] = {};
// Referenced only from the detour stub's assembly, so it must not be a static the optimiser can drop.
extern "C" uint8_t* art_Trampolines[HOOK_COUNT];
uint8_t* art_Trampolines[HOOK_COUNT] = {};

// ---------------------------------------------------------------------------
// Guarded reads (gamemem.h)
// ---------------------------------------------------------------------------

static bool Ptr(uint32_t p) { return PtrInUser(p); }
static bool Rd(uint32_t addr, void* out, uint32_t n) { return MemcpyTry(out, addr, n); }
static bool Rd32(uint32_t addr, uint32_t* out) { return MemcpyTry(out, addr, 4); }
static bool Wr32(uint32_t addr, uint32_t v) { return WriteDwordTry(addr, v); }

struct Vec3 { float x, y, z; };

static bool RdPos(uint32_t entity, Vec3* p) { return Rd(entity + 0x48, p, 12); }

static const uint32_t UNIT_OFF_COUNT = 0x18;
static const uint32_t UNIT_OFF_SOLDIERS = 0x1C;
static const uint32_t MAX_SOLDIERS = 1024;

// ---------------------------------------------------------------------------
// The fix. An artillery piece is an entity whose class sends fire orders through the engine fire
// action (vtable slot +0x1B0 = FUN_1045a2e0; six engine classes, no infantry or cavalry class).
// The fire action (vtable 0x115E7D94) holds the target record at +0x20 and its gun at +0x60; on its
// first update ([+0x7C] == 0) FUN_1047fd90 copies the record into the gun (FUN_1045a8a0 or
// FUN_1044fb60), and the gun turns, aims and fires at record.entity + record.offset. The game
// picks a soldier of the target unit roughly in front of each gun, so a battery standing off to
// one side of its target aims every gun at the near end of the line. Before that copy the fix
// replaces the soldier with the living soldier of the same unit nearest to the unit's centroid
// (soldier array [unit+0x1C], count [unit+0x18], unit = [soldier+0x3B4], positions +0x48 / +0x50);
// the record's offset (aim height) is kept.
// ---------------------------------------------------------------------------

static const uint32_t RVA_ENGINE_FIRE_ORDER = 0x45A2E0;   // FUN_1045a2e0
static const uint32_t SHOOTER_VT_SLOT_FIRE_ORDER = 0x1B0;
static const uint32_t ACTION_OFF_RECORD = 0x20;
static const uint32_t ACTION_OFF_GUN = 0x60;
static const uint32_t ACTION_OFF_COPIED = 0x7C;
static const uint32_t ENTITY_OFF_UNIT = 0x3B4;

static volatile LONG g_fixOn = 0;
static volatile LONG g_fixChanged = 0;     // record pointed at another soldier
static volatile LONG g_fixKept = 0;        // the game's soldier was already the centre one
static volatile LONG g_fixNotSoldier = 0;  // artillery record without a soldier target (ground, building)
static volatile LONG g_fixBadUnit = 0;     // soldier not found in [soldier+0x3B4]'s array
static volatile LONG g_fixDisabled = 0;    // self-check switched the fix off for the session

static bool IsArtillery(uint32_t shooter) {
    uint32_t vt, fn;
    return Ptr(shooter) && Rd32(shooter, &vt) && Rd32(vt + SHOOTER_VT_SLOT_FIRE_ORDER, &fn) &&
           fn == g_base + RVA_ENGINE_FIRE_ORDER;
}

// Soldier of `unit` nearest to the unit's centroid (x / z). Returns 0 when `member` is not in the
// unit's array or a read fails.
static uint32_t CentreSoldier(uint32_t unit, uint32_t member, uint32_t* countOut, float* cx, float* cz) {
    uint32_t n, arr;
    if (!Rd32(unit + UNIT_OFF_COUNT, &n) || !Rd32(unit + UNIT_OFF_SOLDIERS, &arr)) return 0;
    if (n == 0 || n > MAX_SOLDIERS || !Ptr(arr)) return 0;
    double sx = 0, sz = 0;
    int got = 0;
    bool holds = false;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t e;
        Vec3 p;
        if (!Rd32(arr + 4 * i, &e) || !RdPos(e, &p)) return 0;
        holds |= e == member;
        sx += p.x; sz += p.z; got++;
    }
    if (!holds || !got) return 0;
    *cx = (float)(sx / got);
    *cz = (float)(sz / got);
    uint32_t best = 0;
    float bestD = 3.4e38f;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t e;
        Vec3 p;
        if (!Rd32(arr + 4 * i, &e) || !RdPos(e, &p)) return 0;
        float d = (p.x - *cx) * (p.x - *cx) + (p.z - *cz) * (p.z - *cz);
        if (d < bestD) { bestD = d; best = e; }
    }
    *countOut = n;
    return best;
}

static void HandleOrderCopy(const uint8_t* ctx) {
    if (!g_fixOn) return;
    uint32_t action = *(const uint32_t*)(ctx + ART_OFF_ECX);
    uint8_t copied = 1;
    if (!Rd(action + ACTION_OFF_COPIED, &copied, 1) || copied) return;   // only right before the copy
    uint32_t gun;
    if (!Rd32(action + ACTION_OFF_GUN, &gun) || !IsArtillery(gun)) return;
    struct { float ox, oy, oz; uint32_t ent, obj, xform; } r;
    if (!Rd(action + ACTION_OFF_RECORD, &r, sizeof(r))) return;
    if (!Ptr(r.ent) || r.obj || r.xform) { InterlockedIncrement(&g_fixNotSoldier); return; }
    uint32_t unit, count = 0;
    float cx = 0, cz = 0;
    uint32_t centre = 0;
    if (Rd32(r.ent + ENTITY_OFF_UNIT, &unit) && Ptr(unit)) centre = CentreSoldier(unit, r.ent, &count, &cx, &cz);
    if (!centre) {
        // self-check: the soldier -> unit link must hold; switch off for the session if it never does
        LONG bad = InterlockedIncrement(&g_fixBadUnit);
        if (bad >= 20 && g_fixChanged + g_fixKept == 0 && !InterlockedExchange(&g_fixDisabled, 1)) {
            InterlockedExchange(&g_fixOn, 0);
            Log("artfix: DISABLED for this session, the first %ld artillery targets were not found in their "
                "unit's soldier array", bad);
        }
        return;
    }
    if (centre == r.ent) { InterlockedIncrement(&g_fixKept); }
    else if (Wr32(action + ACTION_OFF_RECORD + 0x0C, centre)) { InterlockedIncrement(&g_fixChanged); }
}

// ---------------------------------------------------------------------------
// Handler and detour. The detour sequence is exactly the one given to tools/re/stack_calc.py:
// pushad, pushfd, mark:ctx, push:ctxptr (stack_offsets_art.h)
// ---------------------------------------------------------------------------

extern "C" void __stdcall art_Handler0(const uint8_t* ctx) { HandleOrderCopy(ctx); }

HOOK_DETOUR(art_Detour0, art_Handler0, art_Trampolines, 0)

static const HookDetour g_detours[HOOK_COUNT] = { art_Detour0 };

// ---------------------------------------------------------------------------
// Install / remove
// ---------------------------------------------------------------------------

void ArtFix_Install() {
    Mem_Init();
    uint32_t size = 0;
    uint8_t* base = Hook_GameModule(false, &size);
    if (!base) { Log("artfix: REFUSED, empire.retail.dll is not loaded"); return; }
    g_base = (uint32_t)(uintptr_t)base;
    if (size != HOOK_ANALYSED_IMAGE_SIZE) {
        Log("artfix: REFUSED, SizeOfImage 0x%08X is not the analysed build", size);
        return;
    }
    for (int h = 0; h < HOOK_COUNT; h++) {
        if (!Hook_SiteMatches(base, g_hookDefs[h])) {
            Log("artfix: REFUSED, bytes at RVA 0x%06X (%s) differ from the analysed binary", g_hookDefs[h].rva,
                g_hookDefs[h].func);
            return;
        }
    }
    InterlockedExchange(&g_fixOn, 1);
    if (!Hook_Prepare(base, g_hookDefs, HOOK_COUNT, nullptr, art_Trampolines, g_slots)) {
        Log("artfix: REFUSED, no trampoline memory");
        return;
    }
    Hook_Patch(g_detours, g_slots, HOOK_COUNT);
    for (int h = 0; h < HOOK_COUNT; h++)
        if (g_slots[h].active)
            Log("artfix: hook %s (%s) installed at RVA 0x%06X", g_hookDefs[h].func, g_hookDefs[h].role,
                g_hookDefs[h].rva);
    Log("artfix: ON, artillery aims at the soldier nearest to the centre of the target unit");
}

void ArtFix_Remove() {
    if (g_base)
        Log("artfix: artillery targets moved to the centre soldier %ld, already centre %ld, not a soldier %ld, "
            "unit check failed %ld", g_fixChanged, g_fixKept, g_fixNotSoldier, g_fixBadUnit);
    Hook_Remove(g_slots, HOOK_COUNT);
}
