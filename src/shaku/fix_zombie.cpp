// fix_zombie.cpp -- zombie yari wall fix: a broken unit whose rout task was replaced runs again.
//
// From ShakuEight/Shogun2-fix (fixes/zombie-yari-wall-fix/src/zombiefix.cpp), Copyright 2026 ShakuEight,
// Apache License 2.0 (third_party/LICENSE-APACHE-2.0). Ported to this project: the fix itself is
// unchanged; the development-only parts (toggle key, tones, environment variables, the research probe
// with its thirteen probe-only hooks, sampler thread, unit registry and CSV files, harness exports)
// are removed, the naked detour comes from hook.h's GCC macro, and the log lines go to tws2fix.log.
//
// Fault: yari ashigaru in yari wall break and shatter, but now and then a unit does not run: it
// stands on the spot, out of the fight for the morale system, and its yari wall keeps killing
// whatever touches it.
//
// Engine decode (ShakuEight's notes):
// - A battle unit (the object a soldier points at with +0x214) holds the morale block at +0xCC4:
//   morale level +0xCE8 (5 wavering, 6 broken, 7 shattered), rout state +0xCEC (2 or 3 = routing
//   for the script condition "routing"), rally byte +0xD10; its kill count at +0xD88. Its soldier
//   groups are listed at +0xB90 (count +0xB8C); +0xB9C is the main group (most men).
// - A group (squad tasks: task+4; a soldier's +0x3B4) has its men at +0x1C (count +0x18) and its
//   behaviour stack at group+0x20+0x400 (8-byte entries, count +0x440, current +0x454).
// - The unit's interrupt machine lives at unit +0xE78 (its context's +0x10 = the unit). It enters
//   ROUT_INTERRUPT_STATE when the unit counts as routing; the enter (FUN_10170cc0) clears the
//   unit's order queue and its own task stack; every tick (FUN_101bd3b0) sets up each group whose
//   byte +0x2781 is 0 (FUN_101a9bf0: byte = 1, move cancelled, behaviour stack cleared, the rout
//   task, vtable 0x115E01F4, pushed). The exit clears the byte again.
//
// Cause: a group is set up once. When a unit breaks in the middle of a charge order, a melee
// attack task (0x115C0DFC) starts on the group in the same tick, and when it ends the stack falls
// back to the group's default re-form task (0x115C0644: REFORM, HALT, ADOPT_STANCES, IDLE, READY)
// or later to intercept tasks (0x115C0A48). The rout task is gone, the byte still says "set up",
// so nothing pushes it again: the unit stays routing for the morale system but stands, re-forms
// into its yari wall stance and keeps killing what touches it (replay zombie_yariwall: two units,
// 34 and 40 kills after they broke).
//
// The fix: an entry hook on the rout tick FUN_101bd3b0 (stolen bytes PUSH ESI; PUSH EDI; MOV EDI,ECX;
// MOV EAX,[EDI+0x10], 7 bytes, no branch into them; ECX = the interrupt context, its unit at +0x10)
// clears byte +0x2781 of every live group of the unit that is marked set up but has no rout task
// (vtable 0x115E01F4, or 0x115EDFA8 from the other constructor) on its behaviour stack, so the game's
// own tick sets the group up again in the same call (its own rout task, its own move). Nothing else
// is changed; only units in the rout state are touched, a unit that rallies leaves the rout state
// first, and groups without men are left alone.

#include "log.h"
#include "hook.h"
#include "gamemem.h"
#include "stack_offsets_zombie.h"
#include <windows.h>
#include <cstdint>
#include <cstring>

static uint32_t g_base = 0;

// ---------------------------------------------------------------------------
// Hook: the one fix hook, FUN_101bd3b0 entry (stolen bytes: whole instructions, no branch into
// the range)
// ---------------------------------------------------------------------------

enum {
    H_ROUT_TICK = 0,   // FUN_101bd3b0 entry: tick of the rout interrupt state (ECX = interrupt context); the fix
    HOOK_COUNT
};

static const HookDef g_hookDefs[HOOK_COUNT] = {
    { "FUN_101bd3b0", "rout interrupt state tick (the fix)", 0x1BD3B0, 7,
      { 0x56, 0x57, 0x8B, 0xF9, 0x8B, 0x47, 0x10 }, -1, false },           // PUSH ESI; PUSH EDI; MOV EDI,ECX; MOV EAX,[EDI+0x10]
};

static HookSlot g_slots[HOOK_COUNT] = {};
extern "C" uint8_t* zombie_Trampolines[HOOK_COUNT];   // extern "C": GCC mangles a file-scope static (_ZL...), the detour asm names it
uint8_t* zombie_Trampolines[HOOK_COUNT] = {};

// ---------------------------------------------------------------------------
// Engine layout (see the header)
// ---------------------------------------------------------------------------

static const uint32_t GRP_OFF_COUNT = 0x18, GRP_OFF_MEN = 0x1C;              // group: men
static const uint32_t GRP_OFF_OWNER = 0x20;                                  // group: behaviour stack owner
static const uint32_t OWN_OFF_STACK = 0x400, OWN_OFF_COUNT = 0x440;
static const uint32_t SOL_OFF_GROUP = 0x3B4;
static const uint32_t U_OFF_GA_N = 0xB8C, U_OFF_GA = 0xB90;
static const uint32_t MAX_MEN = 400;
static const uint32_t ICTX_OFF_UNIT = 0x10;                                  // interrupt context (unit +0xE78): its unit
static const uint32_t GRP_OFF_ROUTED = 0x2781;                               // group: rout set up (FUN_101a9bf0)
static const uint32_t RVA_ROUT_TASK_VT = 0x15E01F4, RVA_ROUT_TASK_VT2 = 0x15EDFA8;   // the rout task (FUN_103be670 / FUN_10529820)

static inline uint32_t R32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint32_t Rd(uint32_t addr) { uint32_t v = 0; ReadDword(addr, &v); return v; }
static inline uint8_t RdB(uint32_t addr) { uint8_t v = 0; ReadByte(addr, &v); return v; }

// A group: plausible men list whose first man links back to it.
static bool IsGroup(uint32_t g) {
    uint32_t n, a, s0;
    if (!PtrPlausible(g) || !ReadDword(g + GRP_OFF_COUNT, &n) || !ReadDword(g + GRP_OFF_MEN, &a)) return false;
    if (n == 0 || n > MAX_MEN || !PtrPlausible(a) || !ReadDword(a, &s0) || !PtrPlausible(s0)) return false;
    return Rd(s0 + SOL_OFF_GROUP) == g;
}

// ---------------------------------------------------------------------------
// Fix state
// ---------------------------------------------------------------------------

static volatile LONG g_fixOn = 0;
static volatile LONG g_fRearmed = 0, g_fMissing = 0, g_fTicks = 0;

// ---------------------------------------------------------------------------
// The fix: the rout interrupt state's tick (FUN_101bd3b0) sets up every group of the unit whose
// byte +0x2781 is still 0 (FUN_101a9bf0: byte = 1, the group's behaviour stack cleared, the rout
// task pushed) and never looks at the group again. A task pushed onto the group afterwards (an
// order of the same tick, the group's default re-form task, an intercept) replaces the rout task
// for good: the unit stays "routing" for the morale system but stands, re-forms, adopts its
// stances (yari wall: braced spears) and kills whatever runs into it. The fix clears the byte of
// a live group that is marked set up but has no rout task on its stack, so the game's own tick
// sets it up again in the same call.
// ---------------------------------------------------------------------------

static bool HasRoutTask(uint32_t g) {
    uint32_t owner = g + GRP_OFF_OWNER, n = Rd(owner + OWN_OFF_COUNT);
    if (n > 8) return true;                       // not readable as a stack: leave the group alone
    for (uint32_t i = 0; i < n; i++) {
        uint32_t vt = Rd(Rd(owner + OWN_OFF_STACK + 8 * i));
        if (vt == g_base + RVA_ROUT_TASK_VT || vt == g_base + RVA_ROUT_TASK_VT2) return true;
    }
    return false;
}

static void HandleRoutTick(const uint8_t* ctx) {
    InterlockedIncrement(&g_fTicks);
    uint32_t u = Rd(R32(ctx + ZOMBIE_OFF_ECX) + ICTX_OFF_UNIT);
    if (!PtrPlausible(u)) return;
    uint32_t n = Rd(u + U_OFF_GA_N), arr = Rd(u + U_OFF_GA);
    if (n == 0 || n > 16 || !PtrPlausible(arr)) return;
    bool on = InterlockedCompareExchange(&g_fixOn, 0, 0) != 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t g = Rd(arr + 4 * i);
        if (!IsGroup(g) || RdB(g + GRP_OFF_ROUTED) != 1 || HasRoutTask(g)) continue;
        InterlockedIncrement(&g_fMissing);
        if (on && WriteByteTry(g + GRP_OFF_ROUTED, 0)) InterlockedIncrement(&g_fRearmed);
    }
}

// ---------------------------------------------------------------------------
// Handler and detour. The detour sequence is exactly the one given to tools/re/stack_calc.py:
// pushad, pushfd, mark:ctx, push:ctxptr (stack_offsets_zombie.h)
// ---------------------------------------------------------------------------

extern "C" void __stdcall zombie_Handler0(const uint8_t* ctx) { HandleRoutTick(ctx); }

HOOK_DETOUR(zombie_Detour0, zombie_Handler0, zombie_Trampolines, 0)

static const HookDetour g_detours[HOOK_COUNT] = { zombie_Detour0 };

// ---------------------------------------------------------------------------
// Install / remove
// ---------------------------------------------------------------------------

void ZombieFix_Install() {
    Mem_Init();
    uint32_t size = 0;
    uint8_t* base = Hook_GameModule(false, &size);
    if (!base) { Log("zombiefix: REFUSED, empire.retail.dll is not loaded"); return; }
    if (size != HOOK_ANALYSED_IMAGE_SIZE) { Log("zombiefix: REFUSED, SizeOfImage 0x%08X is not the analysed build", size); return; }
    for (int h = 0; h < HOOK_COUNT; h++) {
        if (!Hook_SiteMatches(base, g_hookDefs[h])) {
            Log("zombiefix: REFUSED, bytes at RVA 0x%06X (%s) differ from the analysed binary", g_hookDefs[h].rva,
                g_hookDefs[h].func);
            return;
        }
    }
    g_base = (uint32_t)(uintptr_t)base;
    if (!Hook_Prepare(base, g_hookDefs, HOOK_COUNT, nullptr, zombie_Trampolines, g_slots)) {
        Log("zombiefix: REFUSED, no trampoline memory");
        g_base = 0;
        return;
    }
    InterlockedExchange(&g_fixOn, 1);
    Hook_Patch(g_detours, g_slots, HOOK_COUNT);
    for (int h = 0; h < HOOK_COUNT; h++)
        if (g_slots[h].active)
            Log("zombiefix: hook %s (%s) installed at RVA 0x%06X", g_hookDefs[h].func, g_hookDefs[h].role, g_hookDefs[h].rva);
    Log("zombiefix: ON: a routing unit's group whose rout task was replaced is set up to rout again");
}

void ZombieFix_Remove() {
    Hook_Remove(g_slots, HOOK_COUNT);
    if (!g_base) return;
    Log("zombiefix: rout ticks %ld; groups found without their rout task %ld, set up to rout again %ld", g_fTicks,
        g_fMissing, g_fRearmed);
}
