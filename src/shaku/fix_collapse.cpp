// fix_collapse.cpp -- wall collapse fix (siege defence: gate doors destroyed at Start Battle).
//
// From ShakuEight/Shogun2-fix (fixes/wall-collapse-fix/src/collapsefix.cpp), Copyright 2026 ShakuEight,
// Apache License 2.0 (third_party/LICENSE-APACHE-2.0). Ported to this project: the fix itself is
// unchanged; the development-only parts (toggle key, tones, environment variables, the research probe
// with its seven probe-only hooks, sampler thread and CSV writers, harness exports) are removed, the
// detour stub comes from the GCC HOOK_DETOUR_RET macro, and the log lines go to tws2fix.log.
//
// Fault: in a siege defence, wall pieces damaged in the campaign are shown destroyed during
// deployment and units can be deployed on them; now and then such a piece is destroyed again when the
// battle starts and the men standing on it die.
//
// Engine decode (ShakuEight's notes):
// - battle = [tick object +0xB4] (the phase machine, phase at +0x14): buildings at +0x118 (count
//   +0x114); a building's pieces at +0x1D0 (count +0x1CC).
// - A piece: template +0x18 (level count +0x6C, level models +0x70, each with a damage threshold at
//   +0x90), damage +0xEC (float), damage level +0x54, level model +0x64, destruct level +0x68,
//   variant +0x78, destroyed +0xF1, destroyed before the battle +0xF2, takes damage +0xF3.
// - FUN_10477f40 (ECX = piece, level, variant) applies a level model; FUN_10477ff0 sets the damage
//   and the level from it; FUN_1047b370 adds damage (projectiles); FUN_10479ca0 starts the collapse
//   effects of a destroyed piece; FUN_10426940 applies the campaign's fort damage at battle set-up.
//
// Cause (replay "broken wall bug"): the campaign can leave a castle's gates open to everyone (the
// flag at [tick object +0x5C] +0x9C; gate pieces are the pieces with byte +0x6C set, built open when
// the flag is set, and passable for every alliance). The battle start, run once on the first tick of
// phase 5 (FUN_101787b0, guarded by battle +0x18), then calls FUN_1043c870 for every building, which
// gives every gate piece one more damage than the threshold of its current level (FUN_1043c8b0 ->
// FUN_1047b370): an intact gate door collapses there, at Start Battle. Its gatehouse top is a
// deployable wall position; the men standing on it leave their unit the tick after (10 men in the
// replay). The deployment view showed the door intact, next to a gate tower already destroyed by the
// campaign damage, so nothing warned the player.
//
// The fix: an entry hook on FUN_1043c870 (10 stolen bytes) returns at once, so no gate is damaged at
// the battle start. The gates stay open and passable for every alliance for the whole battle (the
// game's own gate tick and passability test read the same flag), and every other way to damage a
// piece (artillery, rams, fire) is untouched. Before returning, the hook counts the intact gate doors
// the game would have destroyed and logs each building.

#include "log.h"
#include "hook.h"
#include "gamemem.h"
#include "stack_offsets_collapse.h"
#include <windows.h>
#include <cstdint>
#include <cstring>

static uint32_t g_base = 0;

// ---------------------------------------------------------------------------
// Hook (stolen bytes: whole instructions, no branch into the range)
// ---------------------------------------------------------------------------

enum {
    H_GATES = 0,   // FUN_1043c870: battle start, every gate piece of a building damaged (ECX = building); the fix
    HOOK_COUNT
};

static const HookDef g_hookDefs[HOOK_COUNT] = {
    { "FUN_1043c870", "battle start gate damage (the fix)", 0x43C870, 10,
      { 0x56, 0x57, 0x8B, 0xF9, 0x8B, 0xB7, 0xD0, 0x01, 0x00, 0x00 }, -1, false },   // PUSH ESI; PUSH EDI; MOV EDI,ECX; MOV ESI,[EDI+0x1D0]
};

static HookSlot g_slots[HOOK_COUNT] = {};
extern "C" uint8_t* collapse_Trampolines[HOOK_COUNT];
uint8_t* collapse_Trampolines[HOOK_COUNT] = {};

// ---------------------------------------------------------------------------
// Engine layout (see the header)
// ---------------------------------------------------------------------------

static const uint32_t BLD_OFF_NPIECE = 0x1CC, BLD_OFF_PIECE = 0x1D0;
static const uint32_t PC_OFF_GATE = 0x6C, PC_OFF_OWNER = 0x0C, PC_OFF_DESTROYED = 0xF1;
static const uint32_t OWNER_OFF_TOBJ = 0x08, TOBJ_OFF_SETTINGS = 0x5C, SET_OFF_GATES_OPEN = 0x9C;

static inline uint32_t R32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint32_t Rd(uint32_t addr) { uint32_t v = 0; ReadDword(addr, &v); return v; }
static inline uint8_t RdB(uint32_t addr) { uint8_t v = 0; ReadByte(addr, &v); return v; }

// ---------------------------------------------------------------------------
// Fix state
// ---------------------------------------------------------------------------

static volatile LONG g_fBuildings = 0, g_fSkipped = 0, g_fDoors = 0, g_fDoorsKept = 0;

// ---------------------------------------------------------------------------
// Handler
// ---------------------------------------------------------------------------

// FUN_1043c870 (ECX = building; no stack arguments): the battle start's gate damage. Counts the
// intact gate doors the game is about to destroy, then returns before it does.
static uint32_t HandleGates(const uint8_t* ctx) {
    uint32_t bld = R32(ctx + COLLAPSE_OFF_ECX);
    InterlockedIncrement(&g_fBuildings);
    uint32_t np = Rd(bld + BLD_OFF_NPIECE), pcs = Rd(bld + BLD_OFF_PIECE), doors = 0;
    if (np <= 4096 && PtrPlausible(pcs)) {
        for (uint32_t i = 0; i < np; i++) {
            uint32_t p = Rd(pcs + 4 * i);
            if (!PtrPlausible(p) || !RdB(p + PC_OFF_GATE) || RdB(p + PC_OFF_DESTROYED)) continue;
            uint32_t settings = Rd(Rd(Rd(p + PC_OFF_OWNER) + OWNER_OFF_TOBJ) + TOBJ_OFF_SETTINGS);
            if (!RdB(settings + SET_OFF_GATES_OPEN)) continue;
            doors++;
        }
    }
    if (doors) {
        InterlockedExchangeAdd(&g_fDoors, (LONG)doors);
        InterlockedExchangeAdd(&g_fDoorsKept, (LONG)doors);
        Log("collapsefix: battle start, gates open to all: building %08X, %u intact gate door(s) kept", bld, doors);
    }
    InterlockedIncrement(&g_fSkipped);
    return HOOK_RETURN(0);
}

extern "C" uint32_t __stdcall collapse_Handler0(const uint8_t* ctx) { return HandleGates(ctx); }

HOOK_DETOUR_RET(collapse_Detour0, collapse_Handler0, collapse_Trampolines, 0, 0)     // FUN_1043c870: __thiscall, no stack args

static const HookDetour g_detours[HOOK_COUNT] = { collapse_Detour0 };

// ---------------------------------------------------------------------------
// Install / remove
// ---------------------------------------------------------------------------

void CollapseFix_Install() {
    Mem_Init();
    uint32_t size = 0;
    uint8_t* base = Hook_GameModule(false, &size);
    if (!base) { Log("collapsefix: REFUSED, empire.retail.dll is not loaded"); return; }
    if (size != HOOK_ANALYSED_IMAGE_SIZE) {
        Log("collapsefix: REFUSED, SizeOfImage 0x%08X is not the analysed build", size);
        return;
    }
    for (int h = 0; h < HOOK_COUNT; h++) {
        if (!Hook_SiteMatches(base, g_hookDefs[h])) {
            Log("collapsefix: REFUSED, bytes at RVA 0x%06X (%s) differ from the analysed binary", g_hookDefs[h].rva,
                g_hookDefs[h].func);
            return;
        }
    }
    g_base = (uint32_t)(uintptr_t)base;
    bool use[HOOK_COUNT];
    for (int h = 0; h < HOOK_COUNT; h++) use[h] = true;
    if (!Hook_Prepare(base, g_hookDefs, HOOK_COUNT, use, collapse_Trampolines, g_slots)) {
        Log("collapsefix: REFUSED, no trampoline memory");
        g_base = 0;
        return;
    }
    Hook_Patch(g_detours, g_slots, HOOK_COUNT);
    Log("collapsefix: ON, FUN_1043c870 hooked at RVA 0x%06X: the battle start no longer destroys the gates of a castle "
        "whose gates are open to all", g_hookDefs[H_GATES].rva);
}

void CollapseFix_Remove() {
    Hook_Remove(g_slots, HOOK_COUNT);
    if (!g_base) return;
    Log("collapsefix: battle start gate damage calls %ld (skipped %ld); intact gate doors %ld, kept %ld", g_fBuildings,
        g_fSkipped, g_fDoors, g_fDoorsKept);
}
