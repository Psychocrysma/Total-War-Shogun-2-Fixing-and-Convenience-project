// fix_kneel.cpp -- kneel fire move fix (Fall of the Samurai).
//
// From ShakuEight/Shogun2-fix (fixes/kneel-fire-move-fix/src/kneelfix.cpp), Copyright 2026 ShakuEight,
// Apache License 2.0 (third_party/LICENSE-APACHE-2.0). Ported to this project: the fix itself is
// unchanged; the development-only parts (toggle key, tones, environment variables, research probe,
// harness exports) are removed, the naked helper is written in GCC assembly, and the log lines go to
// tws2fix.log.
//
// Fault: Fall of the Samurai line infantry with the Kneel Fire ability switched on refuse every move
// order; the player has to switch the ability off, move, and switch it on again.
//
// Engine decode (ShakuEight's notes):
// - Kneel fire is special ability 0x39 (bit 0x39 of the unit's active-ability set at +0xED4,
//   vfunc +4 = test bit). The ability handler treats it exactly like fire by rank: switching it on
//   sets the unit's firing drill +0xED0 to 6 (kneel_fire), switching it off restores the default.
// - FUN_1015c3f0 (ECX = unit) is the "can this unit take a move order" test. Its first step is
//   "kneel fire on -> no"; fire by rank has no such step. Only then come the real tests.
// - The move commands run their placement builders FUN_10400870 (click) and FUN_10401c50 (drag, also
//   through FUN_1048c7d0) both for the UI preview and in the command executors; the builders drop
//   every unit that fails FUN_1015c3f0. FUN_104367a0 asks it for the selection too.
//
// The fix redirects the eight calls of FUN_1015c3f0 in those four functions (the whole CALL
// instruction is byte-checked, only its displacement is written). When the game says "no" and the
// unit's kneel fire is on, the wrapper asks the game's own test again from just past its kneel step
// (KneelSkipEntry: the function's PUSH ESI; MOV ESI,ECX, then a jump to +0x1A), so a kneeling unit
// gets exactly the answer it would get with kneel fire off. Kneel fire stays on.
// Self-check: for a unit without kneel fire the two answers must agree (the first 500 such calls are
// compared); a mismatch switches the fix off for the session.

#include "log.h"
#include "hook.h"
#include "gamemem.h"
#include <windows.h>
#include <cstdint>
#include <cstring>

static uint32_t g_base = 0;

static const uint32_t RVA_CAN_MOVE = 0x15C3F0;         // FUN_1015c3f0
static const uint32_t RVA_CAN_MOVE_RESUME = 0x15C40A;  // just past its kneel step (+0x1A)
static const uint32_t ABILITY_KNEEL_FIRE = 0x39;
static const uint32_t U_OFF_ABILITIES = 0xED4;

enum {
    S_CLICK_ONE = 0, S_CLICK_SEL1, S_CLICK_SEL2, S_DRAG_SEL1, S_DRAG_SEL2, S_SEL_ANY1, S_SEL_ANY2, S_DRAG_ONE,
    SITE_COUNT
};

struct CallSite {
    const char* name;
    const char* role;
    uint32_t rva;   // the CALL instruction
};

static const CallSite g_sites[SITE_COUNT] = {
    { "FUN_10400870+0x7A", "click move: the one selected unit", 0x4008EA },
    { "FUN_10400870+0x123", "click move: selection filter", 0x400993 },
    { "FUN_10400870+0x663", "click move: selection filter (placements)", 0x400ED3 },
    { "FUN_10401c50+0xFF", "drag move: selection filter", 0x401D4F },
    { "FUN_10401c50+0x819", "drag move: selection filter (placements)", 0x402469 },
    { "FUN_104367a0+0x8B", "selection: any unit can move (first loop)", 0x43682B },
    { "FUN_104367a0+0xD2", "selection: any unit can move (second loop)", 0x436872 },
    { "FUN_1048c7d0+0x47", "drag move: the one selected unit", 0x48C817 },
};

static HookDef SiteDef(int i) {
    HookDef d = {};
    d.func = g_sites[i].name;
    d.role = g_sites[i].role;
    d.rva = g_sites[i].rva;
    d.len = 5;
    int32_t rel = (int32_t)(RVA_CAN_MOVE - (g_sites[i].rva + 5));
    d.expected[0] = 0xE8;
    memcpy(d.expected + 1, &rel, 4);
    d.relocOff = -1;
    d.probeOnly = false;
    return d;
}

// FUN_1015c3f0 itself is not patched, but KneelSkipEntry enters it at +0x1A: its first 26 bytes
// (PUSH ESI; MOV ESI,ECX; the kneel test; JNZ to the "no" exit) and the instruction at the entry
// point must be the analysed ones.
static const uint8_t kCanMoveHead[] = {
    0x56, 0x8B, 0xF1, 0x8B, 0x86, 0xD4, 0x0E, 0x00, 0x00, 0x8D, 0x8E, 0xD4, 0x0E, 0x00, 0x00, 0x6A,
    0x39, 0x8B, 0x40, 0x04, 0xFF, 0xD0, 0x84, 0xC0, 0x75, 0x45,
    0x8B, 0x46, 0x14, 0x8B, 0x48, 0x08,
};

static bool g_sitePatched[SITE_COUNT] = {};

static volatile LONG g_fixOn = 0;
static volatile LONG g_selfOff = 0;
static volatile LONG g_cCalls[SITE_COUNT] = {};
static volatile LONG g_cKneelNo = 0;      // the game said no to a unit with kneel fire on
static volatile LONG g_fChanged = 0;      // ... and the fix answered yes
static volatile LONG g_cChecked = 0;      // self-check comparisons (kneel fire off, game said no)
static volatile LONG g_fMismatch = 0;
static const LONG SELF_CHECK_CALLS = 500;

static bool FixOn() { return g_fixOn != 0; }
static uint32_t Rd(uint32_t a) { uint32_t v = 0; ReadDword(a, &v); return v; }

typedef uint32_t (__fastcall* CanMoveFn)(uint32_t unit);
typedef uint8_t (__fastcall* BitTestFn)(uint32_t set, uint32_t edx, uint32_t bit);   // __thiscall, 1 stack arg

extern "C" uint32_t g_kneelResume asm("tws2_kneel_resume");   // base + RVA_CAN_MOVE_RESUME
uint32_t g_kneelResume = 0;

// FUN_1015c3f0 from just past its kneel step: the function's own frame (PUSH ESI; MOV ESI,ECX), then
// its code from +0x1A on, which returns to our caller with AL = the answer (upper EAX bits undefined).
extern "C" uint32_t __fastcall KneelSkipEntry(uint32_t unit) asm("tws2_kneel_skip_entry");
__asm__(".text\n"
        ".intel_syntax noprefix\n"
        "tws2_kneel_skip_entry:\n"
        "  push esi\n"
        "  mov esi, ecx\n"
        "  jmp dword ptr [tws2_kneel_resume]\n"
        ".att_syntax prefix\n");

static bool KneelOn(uint32_t unit) {
    uint32_t set = unit + U_OFF_ABILITIES;
    uint32_t vt = Rd(set);
    if (!PtrPlausible(vt)) return false;
    uint32_t fn = Rd(vt + 4);
    if (!PtrPlausible(fn)) return false;
    return ((BitTestFn)(uintptr_t)fn)(set, 0, ABILITY_KNEEL_FIRE) != 0;
}

static void SelfCheckFailed(uint32_t unit) {
    InterlockedExchange(&g_selfOff, 1);
    InterlockedExchange(&g_fixOn, 0);
    Log("kneelfix: SELF-CHECK, unit 0x%08X without kneel fire: the game said no and the test past the kneel step "
        "said yes; fix switched off for this session", unit);
}

static uint32_t HandleSite(int site, uint32_t unit) {
    InterlockedIncrement(&g_cCalls[site]);
    uint32_t game = ((CanMoveFn)(uintptr_t)(g_base + RVA_CAN_MOVE))(unit) & 0xFF;
    if (game || g_selfOff || !PtrPlausible(unit)) return game;
    bool kneel = KneelOn(unit);
    uint32_t answer = game;
    if (kneel) {
        InterlockedIncrement(&g_cKneelNo);
        if (FixOn()) {
            answer = KneelSkipEntry(unit) & 0xFF;
            if (answer) InterlockedIncrement(&g_fChanged);
        }
    } else if (FixOn() && g_cChecked < SELF_CHECK_CALLS) {
        InterlockedIncrement(&g_cChecked);
        if (KneelSkipEntry(unit) & 0xFF) {
            InterlockedIncrement(&g_fMismatch);
            SelfCheckFailed(unit);
        }
    }
    return answer;
}

static uint32_t __fastcall Site0(uint32_t u) { return HandleSite(0, u); }
static uint32_t __fastcall Site1(uint32_t u) { return HandleSite(1, u); }
static uint32_t __fastcall Site2(uint32_t u) { return HandleSite(2, u); }
static uint32_t __fastcall Site3(uint32_t u) { return HandleSite(3, u); }
static uint32_t __fastcall Site4(uint32_t u) { return HandleSite(4, u); }
static uint32_t __fastcall Site5(uint32_t u) { return HandleSite(5, u); }
static uint32_t __fastcall Site6(uint32_t u) { return HandleSite(6, u); }
static uint32_t __fastcall Site7(uint32_t u) { return HandleSite(7, u); }
static const CanMoveFn g_wrappers[SITE_COUNT] = { Site0, Site1, Site2, Site3, Site4, Site5, Site6, Site7 };

static bool WriteRel(int i, uint32_t to) {
    uint32_t at = g_base + g_sites[i].rva;
    int32_t rel = (int32_t)(to - (at + 5));
    return Hook_WriteCode((uint8_t*)(uintptr_t)(at + 1), &rel, 4);
}

static void UnpatchSites() {
    for (int i = 0; i < SITE_COUNT; i++) {
        if (!g_sitePatched[i]) continue;
        WriteRel(i, g_base + RVA_CAN_MOVE);
        g_sitePatched[i] = false;
    }
}

void KneelFix_Install() {
    Mem_Init();
    uint32_t size = 0;
    uint8_t* base = Hook_GameModule(false, &size);
    if (!base) { Log("kneelfix: REFUSED, empire.retail.dll is not loaded"); return; }
    if (size != HOOK_ANALYSED_IMAGE_SIZE) { Log("kneelfix: REFUSED, SizeOfImage 0x%08X is not the analysed build", size); return; }
    for (int i = 0; i < SITE_COUNT; i++) {
        HookDef d = SiteDef(i);
        if (!Hook_SiteMatches(base, d)) {
            Log("kneelfix: REFUSED, bytes at RVA 0x%06X (%s) differ from the analysed binary", d.rva, d.func);
            return;
        }
    }
    if (memcmp(base + RVA_CAN_MOVE, kCanMoveHead, sizeof(kCanMoveHead)) != 0) {
        Log("kneelfix: REFUSED, bytes at RVA 0x%06X (FUN_1015c3f0, the can-move test) differ from the analysed binary",
            RVA_CAN_MOVE);
        return;
    }
    g_base = (uint32_t)(uintptr_t)base;
    g_kneelResume = g_base + RVA_CAN_MOVE_RESUME;
    InterlockedExchange(&g_fixOn, 1);
    for (int i = 0; i < SITE_COUNT; i++) {
        if (!WriteRel(i, (uint32_t)(uintptr_t)g_wrappers[i])) {
            Log("kneelfix: REFUSED, cannot write the call at RVA 0x%06X; restoring", g_sites[i].rva);
            UnpatchSites();
            g_base = 0;
            return;
        }
        g_sitePatched[i] = true;
    }
    Log("kneelfix: ON, %d call sites redirected: units with kneel fire on take move orders (kneel fire stays on)", SITE_COUNT);
}

void KneelFix_Remove() {
    if (!g_base) return;
    UnpatchSites();
    long calls = 0;
    for (int i = 0; i < SITE_COUNT; i++) calls += g_cCalls[i];
    Log("kneelfix: can-move tests %ld; kneeling units refused by the game %ld, let move %ld; self-check %ld compared, "
        "%ld mismatches%s", calls, g_cKneelNo, g_fChanged, g_cChecked, g_fMismatch,
        g_selfOff ? " (fix switched off by the self-check)" : "");
}
