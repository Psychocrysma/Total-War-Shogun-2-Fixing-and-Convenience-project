// fix_guard.cpp -- guard mode ranged fix (missile units in guard mode neither chase nor shoot at shattered units).
//
// From ShakuEight/Shogun2-fix (fixes/guard-mode-ranged-fix/src/guardfix.cpp), Copyright 2026 ShakuEight,
// Apache License 2.0 (third_party/LICENSE-APACHE-2.0). Ported to this project: the fix itself is
// unchanged; the development-only parts (toggle key, tones, environment variables, the research probe
// with its three probe-only hooks, sampler thread and CSV files, harness exports) are removed, the
// detour stub comes from hook.h's GCC macro, and the log lines go to tws2fix.log.
//
// Fault: a unit in guard mode holds its position, and a melee unit in guard mode does not pursue. A
// missile unit (bows, matchlocks) in guard mode that was ordered to attack an enemy unit still walks
// after it when the enemy leaves its range (a routing enemy above all), and it keeps shooting at a
// shattered enemy.
//
// Engine decode (ShakuEight's notes):
// - Guard mode is byte +0xEB1 of the battle unit (the object a soldier points at with +0x214). The
//   battle command BCQ_MULTIPLE_SELECTION_ORDER_CHANGE_GUARD_MODE (executor FUN_102a8340, per selection
//   FUN_1045d0b0) sets it through FUN_10186130 (skipped while the unit routs); unit setup FUN_10150ac0
//   copies the default from +0x17A.
// - The melee no-pursuit: the melee attack task's PURSUE_STATE update (FUN_1019fa80) keeps chasing only
//   while FUN_101af030 says so, and that test answers no when the unit's +0xEB1 is set.
// - A missile unit's attack order runs the missile attack squad task (vtable 0x115C0F24, built by
//   FUN_10127460 from the deferred attack command FUN_103d0470). Its target battle unit is at task
//   +0x24, task +0x2D is a "never intercept" byte of the order. Its FIRE, FIRE_UNFORMED and
//   PERFORM_CAN_SHOOT_TEST updates ask FUN_101aefc0 ("+0x2D clear and the last can-shoot test failed")
//   and then go to INTERCEPT_STATE, whose enter pushes a direct intercept task (0x115C0A48) that walks
//   the group towards the target: the chase. Guard mode is never read on that path. Without INTERCEPT
//   the same updates fall through to FIRE when the test passed, else to CANNOT_SHOOT_STATE, which ends
//   the task. The same updates, and INTERCEPT's, first ask FUN_101b2460 whether the target is still a
//   valid one; true ends the task.
//
// The fix redirects seven calls of the missile attack task's updates (the whole CALL instruction is
// byte-checked, only its displacement is written):
// - Lever 1, no chase: the three calls of FUN_101aefc0. For a unit in guard mode, "go and intercept"
//   is answered no once the unit has had this target in range (its task was in FIRE or FIRE_UNFORMED
//   against it), so a target that leaves the range is let go: the task ends through CANNOT_SHOOT as
//   for an order with +0x2D set, and the unit stays where it stands. An order on a target that was
//   never in range still walks into range (guard mode does not refuse the order).
// - Lever 2, no shots at shattered units: the four calls of FUN_101b2460 (FIRE, FIRE_UNFORMED,
//   PERFORM_CAN_SHOOT_TEST, INTERCEPT). For a unit in guard mode the target counts as invalid when its
//   morale level (+0xCE8) is 7 (shattered), so the task ends; in INTERCEPT the target also counts as
//   invalid once it was in range (a chase already under way when guard mode is set).
// - Lever 2, fire at will: an entry hook on FUN_101752d0, fire at will's choice among the enemies in
//   range. For a missile unit in guard mode the shattered enemies are taken out of the list, so fire
//   at will takes another enemy in range or holds fire (without it, a dropped target was replaced by
//   the next shattered enemy at once, a new task every 0.2 s). A missile unit is one whose missile
//   attack task passed a redirected call in this battle.
// The INTERCEPT update is shared with the naval missile task (vtable 0x115C1418): the wrappers act only
// on the land missile task's vtable. The fix's tables (in-range pairs, missile units) are cleared when
// the battle tick goes back (a new battle).
// Units not in guard mode are never touched (every wrapper returns the game's own answer first).
// Self-check: a guard byte that is not 0 or 1 is counted; after 10 the fix switches itself off for the
// session.

#include "log.h"
#include "hook.h"
#include "gamemem.h"
#include "stack_offsets_guard.h"
#include <windows.h>
#include <cstdint>
#include <cstring>

static uint32_t g_base = 0;

// ---------------------------------------------------------------------------
// The entry hook (stolen bytes: docs/re/prologues_guardfix_faw.txt)
// ---------------------------------------------------------------------------

enum {
    H_FAW = 0,  // FUN_101752d0 entry: fire at will's choice among the enemies in range (cdecl: unit, list, ...)
    HOOK_COUNT
};

static const HookDef g_hookDefs[HOOK_COUNT] = {
    { "FUN_101752d0", "fire at will target choice (fix)", 0x1752D0, 7,
      { 0x83, 0xEC, 0x58, 0x8B, 0x4C, 0x24, 0x60 }, -1, false },           // SUB ESP,0x58; MOV ECX,[ESP+0x60]
};

static HookSlot g_slots[HOOK_COUNT] = {};
// Referenced by name from the detour stub (hook.h): a C symbol, prefixed like the other extern "C" ones.
extern "C" uint8_t* guard_Trampolines[HOOK_COUNT];
uint8_t* guard_Trampolines[HOOK_COUNT] = {};

// ---------------------------------------------------------------------------
// The fix sites: CALL rel32 instructions in the missile attack task's state updates
// (docs/re/prologues_guardfix.txt). The whole instruction is checked; the displacement is
// rewritten to the wrapper and back.
// ---------------------------------------------------------------------------

enum SiteKind { K_OOR = 0, K_TGT = 1 };
enum {
    S_OOR_FIRE = 0, S_OOR_UNFORMED, S_OOR_TEST,
    S_TGT_FIRE, S_TGT_UNFORMED, S_TGT_TEST, S_TGT_INTERCEPT,
    SITE_COUNT
};

struct CallSite {
    const char* name;
    const char* role;
    uint32_t rva;         // the CALL instruction
    uint32_t calleeRva;   // its target in the analysed binary
    SiteKind kind;
    bool firing;          // inside FIRE / FIRE_UNFORMED: the target is (or was just) in range
};

static const CallSite g_sites[SITE_COUNT] = {
    { "FUN_100fa100+0x2E", "FIRE update: out of range, intercept?", 0x0FA12E, 0x1AEFC0, K_OOR, true },
    { "FUN_100fa1d0+0x2E", "FIRE_UNFORMED update: out of range, intercept?", 0x0FA1FE, 0x1AEFC0, K_OOR, true },
    { "FUN_100fa300+0x2E", "PERFORM_CAN_SHOOT_TEST update: out of range, intercept?", 0x0FA32E, 0x1AEFC0, K_OOR, false },
    { "FUN_100fa100+0x1B", "FIRE update: target invalid?", 0x0FA11B, 0x1B2460, K_TGT, true },
    { "FUN_100fa1d0+0x1B", "FIRE_UNFORMED update: target invalid?", 0x0FA1EB, 0x1B2460, K_TGT, true },
    { "FUN_100fa300+0x1F", "PERFORM_CAN_SHOOT_TEST update: target invalid?", 0x0FA31F, 0x1B2460, K_TGT, false },
    { "0x100f8070+0x1B", "INTERCEPT update: target invalid?", 0x0F808B, 0x1B2460, K_TGT, false },
};

static HookDef SiteDef(int i) {
    HookDef d = {};
    d.func = g_sites[i].name;
    d.role = g_sites[i].role;
    d.rva = g_sites[i].rva;
    d.len = 5;
    int32_t rel = (int32_t)(g_sites[i].calleeRva - (g_sites[i].rva + 5));
    d.expected[0] = 0xE8;
    memcpy(d.expected + 1, &rel, 4);
    d.relocOff = -1;
    d.probeOnly = false;
    return d;
}

static bool g_sitePatched[SITE_COUNT] = {};

// ---------------------------------------------------------------------------
// Engine layout (see the header)
// ---------------------------------------------------------------------------

static const uint32_t TASK_OFF_OWNER = 0x04;              // task: group (squad task) / unit (unit task)
static const uint32_t MTASK_OFF_TARGET = 0x24;            // missile attack task: target unit (+0x2D: no-intercept byte)
static const uint32_t GRP_OFF_UNIT = 0x510;
static const uint32_t U_OFF_LEVEL = 0xCE8, U_OFF_GUARD = 0xEB1;
static const uint32_t LEVEL_SHATTERED = 7;
static const uint32_t RVA_MISSILE_TASK_VT = 0x15C0F24;    // missile attack squad task (FUN_10127460)

static inline uint32_t R32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint32_t Rd(uint32_t addr) { uint32_t v = 0; ReadDword(addr, &v); return v; }
static inline uint32_t Rva(uint32_t a) { return a >= g_base && a < g_base + HOOK_ANALYSED_IMAGE_SIZE ? a - g_base : 0; }
static uint32_t Tick() { return GameTickAt(g_base); }

// ---------------------------------------------------------------------------
// Fix state
// ---------------------------------------------------------------------------

static volatile LONG g_fixOn = 0;
static volatile LONG g_selfOff = 0;       // switched off by the self-check
static volatile LONG g_cCalls[SITE_COUNT] = {};
static volatile LONG g_cGuardYes = 0;     // "intercept" asked for a guard unit
static volatile LONG g_fNoChase = 0;      // intercept answered no (lever 1)
static volatile LONG g_fFresh = 0;        // intercept left to the game: target never in range
static volatile LONG g_fShattered = 0;    // shattered target answered invalid (lever 2)
static volatile LONG g_fStopped = 0;      // INTERCEPT under way stopped (target was in range)
static volatile LONG g_fBadGuard = 0;     // guard byte neither 0 nor 1
static volatile LONG g_fMarks = 0;        // (unit, target) pairs seen in range
static volatile LONG g_cFaw = 0;          // fire at will choices (all units)
static volatile LONG g_fIgnored = 0;      // shattered enemies taken out of a guard missile unit's choice (lever 2)
static const LONG BAD_GUARD_LIMIT = 10;

static bool FixOn() { return InterlockedCompareExchange(&g_fixOn, 0, 0) != 0; }

// Pairs (unit, target) that the unit has had in range: its missile task was in FIRE or
// FIRE_UNFORMED against that target. Game thread only (the wrappers). A full table overwrites
// the oldest entry.
struct Mark { uint32_t unit, target, tick; };
static const int MARK_TABLE = 512;
static Mark g_marks[MARK_TABLE];
static int g_markNext = 0;

static bool Marked(uint32_t unit, uint32_t target) {
    for (int i = 0; i < MARK_TABLE; i++)
        if (g_marks[i].unit == unit && g_marks[i].target == target) return true;
    return false;
}

static void MarkInRange(uint32_t unit, uint32_t target) {
    uint32_t t = Tick();
    for (int i = 0; i < MARK_TABLE; i++)
        if (g_marks[i].unit == unit && g_marks[i].target == target) { g_marks[i].tick = t; return; }
    g_marks[g_markNext].unit = unit;
    g_marks[g_markNext].target = target;
    g_marks[g_markNext].tick = t;
    g_markNext = (g_markNext + 1) % MARK_TABLE;
    InterlockedIncrement(&g_fMarks);
}

// Units whose missile attack task passed one of the redirected calls: the missile units (fire at
// will's list is filtered for these only). Game thread only.
static const int MISSILE_TABLE = 256;
static uint32_t g_missile[MISSILE_TABLE];
static int g_missileN = 0;

static bool IsMissileUnit(uint32_t u) {
    for (int i = 0; i < g_missileN; i++) if (g_missile[i] == u) return true;
    return false;
}

static void NoteMissileUnit(uint32_t u) {
    if (IsMissileUnit(u)) return;
    if (g_missileN < MISSILE_TABLE) g_missile[g_missileN++] = u;
    else g_missile[(g_missileN++) % MISSILE_TABLE] = u;
    if (g_missileN >= 2 * MISSILE_TABLE) g_missileN = MISSILE_TABLE;
}

// A new battle (the battle tick went back): forget the in-range pairs and the missile units, whose
// addresses the next battle may reuse. Game thread only.
static uint32_t g_lastTick = 0;
static void BattleCheck() {
    uint32_t t = Tick();
    if (t < g_lastTick) {
        memset(g_marks, 0, sizeof(g_marks));
        g_markNext = 0;
        memset(g_missile, 0, sizeof(g_missile));
        g_missileN = 0;
    }
    g_lastTick = t;
}

// Guard mode of a unit: 1 / 0, or -1 when the byte is not a bool (counted; the self-check).
static void SelfCheckFailed();
static int GuardOf(uint32_t unit) {
    uint8_t g;
    if (!ReadByte(unit + U_OFF_GUARD, &g)) return -1;
    if (g > 1) {
        if (InterlockedIncrement(&g_fBadGuard) == BAD_GUARD_LIMIT) SelfCheckFailed();
        return -1;
    }
    return g;
}

// ---------------------------------------------------------------------------
// The redirected calls (game thread)
// ---------------------------------------------------------------------------

typedef uint32_t (__fastcall* TestFn)(uint32_t task);

static uint32_t HandleSite(int site, uint32_t task) {
    InterlockedIncrement(&g_cCalls[site]);
    const CallSite& cs = g_sites[site];
    uint32_t game = ((TestFn)(uintptr_t)(g_base + cs.calleeRva))(task) & 0xFF;
    // the INTERCEPT update is shared with the naval missile task: only the land missile task
    if (Rva(Rd(task)) != RVA_MISSILE_TASK_VT) return game;
    uint32_t unit = Rd(Rd(task + TASK_OFF_OWNER) + GRP_OFF_UNIT);
    uint32_t target = Rd(task + MTASK_OFF_TARGET);
    if (!PtrPlausible(unit) || !PtrPlausible(target)) return game;
    int guard = GuardOf(unit);
    BattleCheck();
    NoteMissileUnit(unit);
    if (cs.firing && guard == 1) MarkInRange(unit, target);
    uint32_t answer = game;
    if (guard == 1 && FixOn()) {
        if (cs.kind == K_OOR && game) {
            InterlockedIncrement(&g_cGuardYes);
            bool marked = cs.firing || Marked(unit, target);
            if (marked) { answer = 0; InterlockedIncrement(&g_fNoChase); }
            else InterlockedIncrement(&g_fFresh);
        } else if (cs.kind == K_TGT && !game) {
            if (Rd(target + U_OFF_LEVEL) == LEVEL_SHATTERED) {
                answer = 1;
                InterlockedIncrement(&g_fShattered);
            } else if (site == S_TGT_INTERCEPT && Marked(unit, target)) {
                answer = 1;
                InterlockedIncrement(&g_fStopped);
            }
        }
    }
    return answer;
}

static uint32_t __fastcall Site0(uint32_t task) { return HandleSite(0, task); }
static uint32_t __fastcall Site1(uint32_t task) { return HandleSite(1, task); }
static uint32_t __fastcall Site2(uint32_t task) { return HandleSite(2, task); }
static uint32_t __fastcall Site3(uint32_t task) { return HandleSite(3, task); }
static uint32_t __fastcall Site4(uint32_t task) { return HandleSite(4, task); }
static uint32_t __fastcall Site5(uint32_t task) { return HandleSite(5, task); }
static uint32_t __fastcall Site6(uint32_t task) { return HandleSite(6, task); }
static TestFn const g_wrappers[SITE_COUNT] = { Site0, Site1, Site2, Site3, Site4, Site5, Site6 };

// ---------------------------------------------------------------------------
// Fire at will's choice (game thread): FUN_101752d0 (cdecl: the unit, a list of enemy units in
// missile range with its count at +0x2800, the current target(s), the range, ...) scores every
// enemy in the list (nearer is better, the current target gets a bonus, a routing enemy only a
// penalty) and returns the best. The unit interrupt machine's FIRE_AT_WILL_INTERRUPT_STATE starts
// a unit attack task against that choice (FUN_1016aa00 -> context +0x1C, enter FUN_10170990).
// For a missile unit in guard mode the shattered enemies (level 7) are taken out of the list
// before the game scores it, so fire at will takes another enemy in range or holds fire. The
// callers rebuild the list for every call (a stack buffer in FUN_1016aa00 / FUN_10178300, a
// global whose count FUN_10177e30 resets afterwards), so the list is changed in place.
// ---------------------------------------------------------------------------

static const uint32_t FAW_OFF_COUNT = 0x2800, FAW_MAX = 0xA00;

static void HandleFaw(const uint8_t* ctx) {
    InterlockedIncrement(&g_cFaw);
    if (!FixOn()) return;
    uint32_t unit = R32(ctx + GUARD_OFF_ARG1), list = R32(ctx + GUARD_OFF_ARG2), n;
    if (!PtrPlausible(unit) || !PtrPlausible(list) || !ReadDword(list + FAW_OFF_COUNT, &n) || n == 0 || n > FAW_MAX)
        return;
    BattleCheck();
    if (!IsMissileUnit(unit) || GuardOf(unit) != 1) return;
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t e = Rd(list + 4 * i);
        if (PtrPlausible(e) && Rd(e + U_OFF_LEVEL) == LEVEL_SHATTERED) { InterlockedIncrement(&g_fIgnored); continue; }
        if (k != i) WriteDwordTry(list + 4 * k, e);
        k++;
    }
    if (k != n) WriteDwordTry(list + FAW_OFF_COUNT, k);
}

// ---------------------------------------------------------------------------
// Self-check, detours
// ---------------------------------------------------------------------------

static void SelfCheckFailed() {
    InterlockedExchange(&g_selfOff, 1);
    InterlockedExchange(&g_fixOn, 0);
    Log("guardfix: SELF-CHECK, %ld guard bytes that are not 0 or 1: fix switched off for this session", g_fBadGuard);
}

extern "C" void __stdcall guard_FawHandler(const uint8_t* ctx) { HandleFaw(ctx); }
HOOK_DETOUR(guard_FawDetour, guard_FawHandler, guard_Trampolines, 0)
static const HookDetour g_detours[HOOK_COUNT] = { guard_FawDetour };

// ---------------------------------------------------------------------------
// Install / remove
// ---------------------------------------------------------------------------

static bool WriteRel(int i, uint32_t to) {
    uint32_t at = g_base + g_sites[i].rva;
    int32_t rel = (int32_t)(to - (at + 5));
    return Hook_WriteCode((uint8_t*)(uintptr_t)(at + 1), &rel, 4);
}

static void UnpatchSites() {
    for (int i = 0; i < SITE_COUNT; i++) {
        if (!g_sitePatched[i]) continue;
        WriteRel(i, g_base + g_sites[i].calleeRva);
        g_sitePatched[i] = false;
    }
}

void GuardFix_Install() {
    Mem_Init();
    uint32_t size = 0;
    uint8_t* base = Hook_GameModule(false, &size);
    if (!base) { Log("guardfix: REFUSED, empire.retail.dll is not loaded"); return; }
    if (size != HOOK_ANALYSED_IMAGE_SIZE) { Log("guardfix: REFUSED, SizeOfImage 0x%08X is not the analysed build", size); return; }
    for (int i = 0; i < SITE_COUNT; i++) {
        HookDef d = SiteDef(i);
        if (!Hook_SiteMatches(base, d)) {
            Log("guardfix: REFUSED, bytes at RVA 0x%06X (%s) differ from the analysed binary", d.rva, d.func);
            return;
        }
    }
    for (int h = 0; h < HOOK_COUNT; h++) {
        if (!Hook_SiteMatches(base, g_hookDefs[h])) {
            Log("guardfix: REFUSED, bytes at RVA 0x%06X (%s) differ from the analysed binary",
                g_hookDefs[h].rva, g_hookDefs[h].func);
            return;
        }
    }
    g_base = (uint32_t)(uintptr_t)base;
    if (!Hook_Prepare(base, g_hookDefs, HOOK_COUNT, nullptr, guard_Trampolines, g_slots)) {
        Log("guardfix: REFUSED, no trampoline memory");
        g_base = 0;
        return;
    }
    InterlockedExchange(&g_fixOn, 1);
    for (int i = 0; i < SITE_COUNT; i++) {
        if (!WriteRel(i, (uint32_t)(uintptr_t)g_wrappers[i])) {
            Log("guardfix: REFUSED, cannot write the call at RVA 0x%06X; restoring", g_sites[i].rva);
            UnpatchSites();
            g_base = 0;
            return;
        }
        g_sitePatched[i] = true;
    }
    Hook_Patch(g_detours, g_slots, HOOK_COUNT);
    Log("guardfix: ON, %d call sites redirected and the fire at will hook installed: a missile unit in guard mode lets "
        "a target go once it leaves its range and drops shattered targets", SITE_COUNT);
}

void GuardFix_Remove() {
    Hook_Remove(g_slots, HOOK_COUNT);
    if (!g_base) return;
    UnpatchSites();
    long calls = 0;
    for (int i = 0; i < SITE_COUNT; i++) calls += g_cCalls[i];
    Log("guardfix: missile task tests %ld; guard units asking to intercept %ld: refused %ld, left to the game (target "
        "never in range) %ld; shattered targets dropped %ld; intercepts stopped %ld; fire at will choices %ld, shattered "
        "enemies left out of guard missile units' fire at will %ld; guard bytes not 0 / 1 %ld%s",
        calls, g_cGuardYes, g_fNoChase, g_fFresh, g_fShattered, g_fStopped, g_cFaw, g_fIgnored, g_fBadGuard,
        g_selfOff ? " (fix switched off by the self-check)" : "");
}
