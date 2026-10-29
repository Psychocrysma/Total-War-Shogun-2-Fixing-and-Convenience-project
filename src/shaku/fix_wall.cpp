// fix_wall.cpp -- wall manning fix (siege defence: units go straight onto their wall section).
//
// From ShakuEight/Shogun2-fix (fixes/wall-manning-fix/src/wallfix.cpp), Copyright 2026 ShakuEight,
// Apache License 2.0 (third_party/LICENSE-APACHE-2.0). Ported to this project: the fix itself is
// unchanged; the development-only parts (toggle key, tones, environment variables including the
// WALLFIX_MAX_DIST limit, the research probe with its ten probe-only hooks, sampler thread and CSV
// files, harness exports) are removed, the detour stub comes from the GCC HOOK_DETOUR_RET macro,
// and the log lines go to tws2fix.log.
//
// Fault: in a siege defence, a unit ordered to man a wall section often marches to an unrelated
// point C on open ground first, stops, and only then walks onto the wall at B, instead of going
// from A to B at once.
//
// Engine decode (ShakuEight's notes): the order runs a squad task (vtable 0x115BF91C, constructor
// FUN_1010d810) with three steps. Its first state's enter (FUN_101b0b60) takes the bounding box of
// the wall positions the unit will hold and asks FUN_1016af90 whether the unit stands outside a
// circle around it (half the box diagonal plus the unit's arrival radius). If so it computes a
// staging point C with FUN_10175f10: for a building, on the line from the box centre towards a
// reference point of the building (inside the castle in the measured replay), half a diagonal plus
// half a formation depth out; else the first spot towards the unit where the whole formation
// rectangle fits. The second state marches the unit there in formation (an ordinary unit move,
// FUN_103db410 from FUN_1017e5a0) and waits until it stands in its rectangle; only the third state
// (FUN_101b2010) hands every man his wall position, and the men walk there one by one. A unit that
// does not come from C's side walks to C and back.
//
// The fix: one entry hook on FUN_1016af90 (stolen bytes 83 EC 18 57 8B F9; __thiscall, ECX = task,
// arg 1 = box {min x, min z, max x, max z}, RET 4), the staging test. For the call from the wall
// task (return address 0x101B0CAD, RVA 0x1B0CAD in FUN_101b0b60) the original runs through the
// trampoline; when the game says "far", the hook answers "close enough" (AL = 0) instead, so the
// task skips the staging march and the men go straight from where they stand to their wall
// positions, at the order's pace. The men's slots, their assignment and the stances are the
// game's. The other caller of the test (FUN_10197e00) is left alone (counted as "other").

#include "log.h"
#include "hook.h"
#include "gamemem.h"
#include "stack_offsets_wall.h"
#include <windows.h>
#include <cstdint>
#include <cstring>

static uint32_t g_base = 0;

// ---------------------------------------------------------------------------
// Hook (stolen bytes: whole instructions, no branch into the range)
// ---------------------------------------------------------------------------

enum {
    H_STAGE = 0,       // FUN_1016af90 entry: "unit far from its wall positions" (ECX = task, arg 1 = box); the fix
    HOOK_COUNT
};

static const HookDef g_hookDefs[HOOK_COUNT] = {
    { "FUN_1016af90", "staging test of the wall task (the fix)", 0x16AF90, 6,
      { 0x83, 0xEC, 0x18, 0x57, 0x8B, 0xF9 }, -1, false },                 // SUB ESP,0x18; PUSH EDI; MOV EDI,ECX
};

static HookSlot g_slots[HOOK_COUNT] = {};
extern "C" uint8_t* wall_Trampolines[HOOK_COUNT];   // read by the detour stub as _wall_Trampolines
uint8_t* wall_Trampolines[HOOK_COUNT] = {};

static const uint32_t RVA_RET_STAGING = 0x1B0CAD;   // return of the staging test call in FUN_101b0b60

static inline uint32_t R32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }

// ---------------------------------------------------------------------------
// Counters
// ---------------------------------------------------------------------------

static volatile LONG g_fSkipped = 0, g_fNear = 0, g_fOther = 0;

// ---------------------------------------------------------------------------
// The hook (game thread)
// ---------------------------------------------------------------------------

// FUN_1016af90 wrapped (ECX = task, arg 1 = box {min x, min z, max x, max z}; RET 4). The game's
// answer comes from the trampoline; for the wall task's call the fix answers 0 instead.
typedef uint32_t (__fastcall* StageFn)(uint32_t self, uint32_t edx, uint32_t box);

static uint32_t HandleStage(const uint8_t* ctx) {
    uint32_t ret = R32(ctx + WALL_OFF_RET);
    if (ret != g_base + RVA_RET_STAGING) {
        InterlockedIncrement(&g_fOther);
        return HOOK_CONTINUE;
    }
    uint32_t task = R32(ctx + WALL_OFF_ECX), boxp = R32(ctx + WALL_OFF_ARG1);
    uint32_t game = ((StageFn)wall_Trampolines[H_STAGE])(task, 0, boxp);
    uint32_t answer = game;
    if ((game & 0xFF) == 0) {
        InterlockedIncrement(&g_fNear);
    } else {
        answer = game & 0xFFFFFF00u;
        InterlockedIncrement(&g_fSkipped);
    }
    return HOOK_RETURN(answer);
}

extern "C" uint32_t __stdcall wall_StageHandler(const uint8_t* ctx) { return HandleStage(ctx); }

HOOK_DETOUR_RET(wall_StageDetour, wall_StageHandler, wall_Trampolines, 0, 4)   // FUN_1016af90: __thiscall, 1 stack arg

static const HookDetour g_detours[HOOK_COUNT] = { wall_StageDetour };

// ---------------------------------------------------------------------------
// Install / remove
// ---------------------------------------------------------------------------

void WallFix_Install() {
    Mem_Init();
    uint32_t size = 0;
    uint8_t* base = Hook_GameModule(false, &size);
    if (!base) { Log("wallfix: REFUSED, empire.retail.dll is not loaded"); return; }
    if (size != HOOK_ANALYSED_IMAGE_SIZE) { Log("wallfix: REFUSED, SizeOfImage 0x%08X is not the analysed build", size); return; }
    for (int h = 0; h < HOOK_COUNT; h++) {
        if (!Hook_SiteMatches(base, g_hookDefs[h])) {
            Log("wallfix: REFUSED, bytes at RVA 0x%06X (%s) differ from the analysed binary", g_hookDefs[h].rva,
                g_hookDefs[h].func);
            return;
        }
    }
    g_base = (uint32_t)(uintptr_t)base;
    if (!Hook_Prepare(base, g_hookDefs, HOOK_COUNT, nullptr, wall_Trampolines, g_slots)) {
        Log("wallfix: REFUSED, no trampoline memory");
        g_base = 0;
        return;
    }
    Hook_Patch(g_detours, g_slots, HOOK_COUNT);
    Log("wallfix: ON, %s (the staging test of the wall task) hooked at RVA 0x%06X: a unit ordered onto a wall skips "
        "the staging march and goes straight to its wall positions", g_hookDefs[H_STAGE].func, g_hookDefs[H_STAGE].rva);
}

void WallFix_Remove() {
    Hook_Remove(g_slots, HOOK_COUNT);
    if (!g_base) return;
    Log("wallfix: staging marches skipped %ld, not needed (unit close) %ld, other callers of the test %ld",
        g_fSkipped, g_fNear, g_fOther);
}
