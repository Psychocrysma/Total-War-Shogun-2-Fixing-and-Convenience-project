// fix_siege.cpp -- siege responsiveness fix (the defender's units answer move orders late in a large siege).
//
// From ShakuEight/Shogun2-fix (fixes/siege-responsiveness-fix/src/siegefix.cpp), Copyright 2026 ShakuEight,
// Apache License 2.0 (third_party/LICENSE-APACHE-2.0). Ported to this project: the fix itself is
// unchanged (both parts on, at their defaults); the development-only parts (toggle key, MessageBeep,
// environment variables, research probe and its five hooks' handlers, harness exports) are removed, the
// detour stubs come from hook.h's GCC macro, and the log lines go to tws2fix.log.
//
// Fault: in a large siege, once the attackers climb the walls, the defender's units answer move orders
// late (a unit on the wall takes long to leave it, a unit sent onto the wall long to get there). Attack
// orders stay responsive.
//
// Engine decode (ShakuEight's notes, docs/re/path_queue_x86.md):
// - An entity's move runs the movement state machine FUN_103df3f0. State 1 builds a path request and
//   calls the query FUN_10385fd0 (ECX = pathfinder, arg 1 = request; the state machine's call returns
//   to RVA 0x3DF50D). A request whose mode (request +0x30) is 0 is searched at once; any other mode puts
//   it in the pathfinder's request queue and the entity waits in state 2 until the queue has served it.
// - The pathfinder's per-tick update FUN_103aac30 serves the queue in order (one request by
//   FUN_1039a500), and stops for the tick once its A* node counter (pathfinder +0x470, which also counts
//   the immediate searches made since the last tick) reaches 500 (CMP [ESI+0x470],500 at
//   FUN_103aac30+0x274). At least one request is served per tick. The incremental searches of the tick
//   are capped at 200 (pathfinder +0x368, CMP [ESI+0x368],200 at FUN_103aac30+0x91).
// - With thousands of attackers on ladders and walls the queue grows, and a request waits in it for
//   many ticks. When an entity asks again (a new order, a replan), its old request is freed and leaves
//   the queue; the new one joins at the back. Measured in the user's wall assault replay: from the
//   climb on, 500 to 1100 requests waited, 140 to 180 joined per tick, 3 to 5 were served per tick, and
//   only 7 % of all queued requests were ever served (the defender's included). The soldiers' immediate
//   searches alone used about 2000 nodes per tick, so the queue usually got its one request per tick.
//
// The fix, two parts, both on:
//   A. The queue gets its own node budget: the counter +0x470 is zeroed where the queue is reached
//      (mid-function hook FUN_103aac30+0x1E8, MOV EDI,[ESI+0x178]), and the budget 500 becomes 2000 (a
//      4-byte patch of the compare's immediate at FUN_103aac30+0x274, byte-checked as a whole
//      instruction and restored at unload). The incremental cap at FUN_103aac30+0x91 is byte-checked too
//      and stays at the game's 200 (ShakuEight's SIEGEFIX_INCR default = unchanged, so no write).
//   B. The local player's requests skip the queue: in FUN_10385fd0 (entry hook), a request from the
//      movement state machine (return address RVA 0x3DF50D, EBX = entity, entity +0x28 = world,
//      [[world+8]+0xB8] must be the pathfinder) for a soldier of the local player's army has its mode
//      (request +0x30) set to 0, so it is searched at once, as the game does for every soldier's slot
//      order. The local player's army: world +0x281E4 holds (army index, alliance index) pairs, count
//      world +0x281E0; battle = [[world+8]+0xB4]; battle +0x88 alliance count, +0x8C array; alliance
//      +0x20 army count, +0x24 array; army +0x114 unit count, +0x118 array. The local armies' unit list
//      (at most 256 units) is read every 200 ms; a soldier's group (entity +0x3B4) holds its army unit
//      at +0x510.
// Hooks on the pathfinder update entry (FUN_103aac30), the queued search (FUN_1039a500), its end
// (FUN_103aac30+0x28B), the queue's end (FUN_103aac30+0x2D3) and the request builder (FUN_1035fb40)
// served only ShakuEight's research probe: their sites are still byte-checked, but not hooked.

#include "log.h"
#include "hook.h"
#include "gamemem.h"
#include "stack_offsets_siege.h"
#include <windows.h>
#include <cstdint>
#include <cstring>

static uint32_t g_base = 0;

// ---------------------------------------------------------------------------
// Hooks (stolen bytes: whole instructions, no branch into the range) and the two immediate patches
// ---------------------------------------------------------------------------

enum {
    H_UPDATE = 0,      // FUN_103aac30 entry: pathfinder per-tick update (ECX = pathfinder)
    H_INCR_END,        // FUN_103aac30+0x1E8: incremental searches done, queue not yet served (ESI = pathfinder)
    H_SEARCH,          // FUN_1039a500 entry: serve one queued request (ECX = pathfinder, arg 1 = request)
    H_SEARCHED,        // FUN_103aac30+0x28B: after it (AL = result, EDI = request, ESI = pathfinder)
    H_DRAIN_END,       // FUN_103aac30+0x2D3: queue served for this tick (EBX = served, EDI = first left)
    H_QUERY,           // FUN_10385fd0 entry: path query (ECX = pathfinder, arg 1 = request; the fix, part B)
    H_ENQUEUE,         // FUN_1035fb40 entry: queued request built (ECX = it, arg 1 = pathfinder, arg 4 = request)
    HOOK_COUNT
};

// probeOnly sites are byte-checked at install (they are part of the analysed build's signature) but
// not hooked: their handlers were the research probe.
static const HookDef g_hookDefs[HOOK_COUNT] = {
    { "FUN_103aac30", "pathfinder update", 0x3AAC30, 6,
      { 0x81, 0xEC, 0x9C, 0x00, 0x00, 0x00 }, -1, true },                  // SUB ESP,0x9C
    { "FUN_103aac30+0x1E8", "queue reached (the fix, part A)", 0x3AAE18, 6,
      { 0x8B, 0xBE, 0x78, 0x01, 0x00, 0x00 }, -1, false },                  // MOV EDI,[ESI+0x178]
    { "FUN_1039a500", "queued search", 0x39A500, 5,
      { 0x83, 0xEC, 0x40, 0x53, 0x55 }, -1, true },                        // SUB ESP,0x40; PUSH EBX; PUSH EBP
    { "FUN_103aac30+0x28B", "queued search done", 0x3AAEBB, 7,
      { 0x88, 0x47, 0x21, 0xC6, 0x47, 0x20, 0x01 }, -1, true },            // MOV [EDI+0x21],AL; MOV byte [EDI+0x20],1
    { "FUN_103aac30+0x2D3", "queue served", 0x3AAF03, 6,
      { 0x8B, 0x86, 0x8C, 0x01, 0x00, 0x00 }, -1, true },                  // MOV EAX,[ESI+0x18C]
    { "FUN_10385fd0", "path query", 0x385FD0, 5,
      { 0x83, 0xEC, 0x40, 0x53, 0x55 }, -1, false },                       // SUB ESP,0x40; PUSH EBX; PUSH EBP
    { "FUN_1035fb40", "request queued", 0x35FB40, 5,
      { 0x8B, 0x44, 0x24, 0x04, 0x56 }, -1, true },                        // MOV EAX,[ESP+4]; PUSH ESI
};

static HookSlot g_slots[HOOK_COUNT] = {};
extern "C" uint8_t* siege_Trampolines[HOOK_COUNT];
uint8_t* siege_Trampolines[HOOK_COUNT] = {};

// A data patch: the whole instruction is byte-checked, then only the immediate is written.
struct PatchDef {
    const char* func;
    const char* role;
    uint32_t    rva;           // instruction start
    int         len;
    uint8_t     expected[12];
    int         immOff;        // offset of the imm32 in the instruction
    uint32_t    original;      // the game's value
};

enum { P_BUDGET = 0, P_INCR, PATCH_COUNT };
static const PatchDef g_patchDefs[PATCH_COUNT] = {
    { "FUN_103aac30+0x274", "queued-request node budget", 0x3AAEA4, 12,
      { 0x81, 0xBE, 0x70, 0x04, 0x00, 0x00, 0xF4, 0x01, 0x00, 0x00, 0x73, 0x53 }, 6, 500 },   // CMP [ESI+0x470],500; JAE
    { "FUN_103aac30+0x91", "incremental search cap", 0x3AACC1, 12,
      { 0x81, 0xBE, 0x68, 0x03, 0x00, 0x00, 0xC8, 0x00, 0x00, 0x00, 0x0F, 0x83 }, 6, 200 },   // CMP [ESI+0x368],200; JAE
};

// ---------------------------------------------------------------------------
// Engine layout (docs/re/path_queue_x86.md, docs/re/request_radius_x86.md)
// ---------------------------------------------------------------------------

static const uint32_t P_OFF_NODES = 0x470;             // A* nodes since the last served queue (budget 500)
static const uint32_t Q_OFF_MODE = 0x30;               // request: 0 = searched at once, else queued
static const uint32_t E_OFF_WORLD = 0x28, E_OFF_UNIT = 0x3B4;   // soldier entity: world, soldier group
static const uint32_t G_OFF_ARMY_UNIT = 0x510;          // soldier group: the army's unit it belongs to
static const uint32_t RVA_RET_REPLAN = 0x3DF50D;       // return address of the query call in FUN_103df3f0
// Local players (docs/re/path_queue_x86.md): world +0x281E4 -> (army index, alliance index) pairs,
// count world +0x281E0; battle = [[world+8]+0xB4]; battle +0x88 alliance count, +0x8C array; alliance
// +0x20 army count, +0x24 array; army +0x114 unit count, +0x118 array.
static const uint32_t W_OFF_LOCAL = 0x281E4, W_OFF_LOCAL_N = 0x281E0;
static const uint32_t B_OFF_NALL = 0x88, B_OFF_ALL = 0x8C, A_OFF_NARMY = 0x20, A_OFF_ARMY = 0x24;
static const uint32_t ARMY_OFF_NUNIT = 0x114, ARMY_OFF_UNIT = 0x118;
static const int LOCAL_MAX = 256;

static inline uint32_t R32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint32_t Rd(uint32_t addr) { uint32_t v = 0; ReadDword(addr, &v); return v; }

// ---------------------------------------------------------------------------
// Fix state
// ---------------------------------------------------------------------------

static volatile LONG g_fixOn = 0;
static const bool g_playerSync = true;   // part B
static const bool g_queueOwn = true;     // part A: the queue gets its own node budget (counter zeroed before it)
static volatile LONG g_fOwnResets = 0;
static const uint32_t g_budget = 2000, g_incr = 200;
static bool g_patched[PATCH_COUNT] = {};
static volatile LONG g_fSynced = 0, g_fNotPlayer = 0, g_fBadEntity = 0, g_fPatchFail = 0;

static uint8_t* PatchImm(int i) { return (uint8_t*)(uintptr_t)(g_base + g_patchDefs[i].rva + g_patchDefs[i].immOff); }

// The instruction holds the expected bytes with `imm` as its immediate.
static bool PatchIs(int i, uint32_t imm) {
    const PatchDef& d = g_patchDefs[i];
    uint8_t want[12], have[12];
    memcpy(want, d.expected, d.len);
    memcpy(want + d.immOff, &imm, 4);
    if (!MemcpyTry(have, g_base + d.rva, d.len)) return false;
    return memcmp(want, have, d.len) == 0;
}

// Writes `imm` and reads it back; only from a state the fix knows (the game's value or ours).
static bool PatchSet(int i, uint32_t imm) {
    const PatchDef& d = g_patchDefs[i];
    uint32_t ours = i == P_BUDGET ? g_budget : g_incr;
    if (!PatchIs(i, d.original) && !PatchIs(i, ours)) return false;
    if (!Hook_WriteCode(PatchImm(i), &imm, 4)) return false;
    return PatchIs(i, imm);
}

// Install and Remove only (one thread).
static void ApplyPatches(bool on) {
    for (int i = 0; i < PATCH_COUNT; i++) {
        uint32_t ours = i == P_BUDGET ? g_budget : g_incr;
        if (ours == g_patchDefs[i].original) continue;           // nothing to change
        uint32_t want = on ? ours : g_patchDefs[i].original;
        if (PatchSet(i, want)) {
            g_patched[i] = on;
        } else {
            InterlockedIncrement(&g_fPatchFail);
            Log("siegefix: ERROR, %s (%s) at RVA 0x%06X could not be set to %u", g_patchDefs[i].func,
                g_patchDefs[i].role, g_patchDefs[i].rva, want);
        }
    }
}

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------

// The local player's army units, read from the battle every 200 ms (any thread; under g_localCs).
static CRITICAL_SECTION g_localCs;
static uint32_t g_localUnits[LOCAL_MAX];
static int g_nLocal = 0;
static DWORD g_localAt = 0;
static uint32_t g_localWorld = 0;
static volatile LONG g_localArmies = 0, g_localReads = 0;

static int ReadLocalUnits(uint32_t world, uint32_t* out, int cap, LONG* armies) {
    int n = 0;
    *armies = 0;
    uint32_t pairs = Rd(world + W_OFF_LOCAL), np = Rd(world + W_OFF_LOCAL_N);
    uint32_t x = Rd(world + 8);
    uint32_t battle = PtrPlausible(x) ? Rd(x + 0xB4) : 0;
    if (!PtrPlausible(pairs) || np == 0 || np > 8 || !PtrPlausible(battle)) return 0;
    uint32_t nAll = Rd(battle + B_OFF_NALL), arrAll = Rd(battle + B_OFF_ALL);
    if (nAll == 0 || nAll > 16 || !PtrPlausible(arrAll)) return 0;
    for (uint32_t k = 0; k < np; k++) {
        uint32_t armyIdx = Rd(pairs + 8 * k), allIdx = Rd(pairs + 8 * k + 4);
        if (allIdx >= nAll) continue;
        uint32_t al = Rd(arrAll + 4 * allIdx);
        uint32_t nArmy = PtrPlausible(al) ? Rd(al + A_OFF_NARMY) : 0, arrArmy = PtrPlausible(al) ? Rd(al + A_OFF_ARMY) : 0;
        if (armyIdx >= nArmy || nArmy > 64 || !PtrPlausible(arrArmy)) continue;
        uint32_t army = Rd(arrArmy + 4 * armyIdx);
        uint32_t nU = PtrPlausible(army) ? Rd(army + ARMY_OFF_NUNIT) : 0, arrU = PtrPlausible(army) ? Rd(army + ARMY_OFF_UNIT) : 0;
        if (nU > (uint32_t)LOCAL_MAX || !PtrPlausible(arrU)) continue;
        (*armies)++;
        for (uint32_t i = 0; i < nU && n < cap; i++) {
            uint32_t u = Rd(arrU + 4 * i);
            if (PtrPlausible(u)) out[n++] = u;
        }
    }
    return n;
}

static bool IsLocalUnit(uint32_t world, uint32_t armyUnit) {
    if (!PtrPlausible(armyUnit)) return false;
    DWORD now = GetTickCount();
    EnterCriticalSection(&g_localCs);
    if (g_localAt == 0 || now - g_localAt >= 200 || world != g_localWorld) {
        LONG armies = 0;
        g_nLocal = ReadLocalUnits(world, g_localUnits, LOCAL_MAX, &armies);
        g_localAt = now ? now : 1;
        g_localWorld = world;
        InterlockedExchange(&g_localArmies, armies);
        InterlockedIncrement(&g_localReads);
    }
    bool r = false;
    for (int i = 0; i < g_nLocal && !r; i++) r = g_localUnits[i] == armyUnit;
    LeaveCriticalSection(&g_localCs);
    return r;
}

// Whose entity asks, from the movement state machine's query call (EBX = entity). side = the army
// unit of the soldier's group, player = it is the local player's.
struct Who { uint32_t unit, side; bool player; };
static Who Identify(const uint8_t* ctx, uint32_t ret, uint32_t P) {
    Who w = {};
    if (ret != g_base + RVA_RET_REPLAN) return w;
    uint32_t e = R32(ctx + SIEGE_OFF_EBX);
    if (!PtrPlausible(e)) { InterlockedIncrement(&g_fBadEntity); return w; }
    uint32_t world = Rd(e + E_OFF_WORLD);
    uint32_t p = PtrPlausible(world) ? Rd(world + 8) : 0;
    p = PtrPlausible(p) ? Rd(p + 0xB8) : 0;
    if (p != P) { InterlockedIncrement(&g_fBadEntity); return w; }   // not the entity we expect
    w.unit = Rd(e + E_OFF_UNIT);
    w.side = PtrPlausible(w.unit) ? Rd(w.unit + G_OFF_ARMY_UNIT) : 0;
    w.player = IsLocalUnit(world, w.side);
    return w;
}

// Part B (FUN_10385fd0 entry).
static uint32_t HandleQuery(const uint8_t* ctx) {
    uint32_t ret = R32(ctx + SIEGE_OFF_RET);
    uint32_t P = R32(ctx + SIEGE_OFF_ECX);
    uint32_t Q = R32(ctx + SIEGE_OFF_ARG1);
    uint32_t mode = 0;
    if (!ReadDword(Q + Q_OFF_MODE, &mode)) return HOOK_CONTINUE;
    Who w = Identify(ctx, ret, P);
    if (mode != 0 && g_fixOn && g_playerSync) {
        if (w.player) {
            bool synced = WriteDwordTry(Q + Q_OFF_MODE, 0);   // searched at once, as a soldier's slot order
            if (synced) InterlockedIncrement(&g_fSynced);
        } else {
            InterlockedIncrement(&g_fNotPlayer);
        }
    }
    return HOOK_CONTINUE;
}

// Part A (FUN_103aac30+0x1E8): the immediate searches of the tick no longer use up the queue's budget.
static void HandleIncrEnd(const uint8_t* ctx) {
    uint32_t P = R32(ctx + SIEGE_OFF_ESI);
    uint32_t nodes = Rd(P + P_OFF_NODES);
    bool reset = g_fixOn && g_queueOwn && nodes != 0 && WriteDwordTry(P + P_OFF_NODES, 0);
    if (reset) InterlockedIncrement(&g_fOwnResets);
}

// ---------------------------------------------------------------------------
// Detours
// ---------------------------------------------------------------------------

extern "C" void __stdcall siege_Handler1(const uint8_t* ctx);
extern "C" void __stdcall siege_Handler5(const uint8_t* ctx);
extern "C" void __stdcall siege_Handler1(const uint8_t* ctx) { HandleIncrEnd(ctx); }
extern "C" void __stdcall siege_Handler5(const uint8_t* ctx) { HandleQuery(ctx); }

HOOK_DETOUR(siege_Detour1, siege_Handler1, siege_Trampolines, 1)
HOOK_DETOUR(siege_Detour5, siege_Handler5, siege_Trampolines, 5)

// The probe-only sites are never prepared (Hook_Patch skips an unprepared slot).
static const HookDetour g_detours[HOOK_COUNT] = { nullptr, siege_Detour1, nullptr, nullptr, nullptr, siege_Detour5, nullptr };

// ---------------------------------------------------------------------------
// Install / remove
// ---------------------------------------------------------------------------

void SiegeFix_Install() {
    Mem_Init();
    uint32_t size = 0;
    uint8_t* base = Hook_GameModule(false, &size);
    if (!base) { Log("siegefix: REFUSED, empire.retail.dll is not loaded"); return; }
    if (size != HOOK_ANALYSED_IMAGE_SIZE) { Log("siegefix: REFUSED, SizeOfImage 0x%08X is not the analysed build", size); return; }
    for (int h = 0; h < HOOK_COUNT; h++) {
        if (!Hook_SiteMatches(base, g_hookDefs[h])) {
            Log("siegefix: REFUSED, bytes at RVA 0x%06X (%s) differ from the analysed binary", g_hookDefs[h].rva,
                g_hookDefs[h].func);
            return;
        }
    }
    for (int i = 0; i < PATCH_COUNT; i++) {
        const PatchDef& d = g_patchDefs[i];
        if (memcmp(base + d.rva, d.expected, d.len) != 0) {
            Log("siegefix: REFUSED, bytes at RVA 0x%06X (%s) differ from the analysed binary", d.rva, d.func);
            return;
        }
    }
    g_base = (uint32_t)(uintptr_t)base;
    InitializeCriticalSection(&g_localCs);
    bool use[HOOK_COUNT];
    for (int h = 0; h < HOOK_COUNT; h++) use[h] = !g_hookDefs[h].probeOnly;
    use[H_INCR_END] = g_queueOwn;
    if (!Hook_Prepare(base, g_hookDefs, HOOK_COUNT, use, siege_Trampolines, g_slots)) {
        Log("siegefix: REFUSED, no trampoline memory");
        g_base = 0;
        return;
    }
    ApplyPatches(true);
    InterlockedExchange(&g_fixOn, 1);
    Hook_Patch(g_detours, g_slots, HOOK_COUNT);
    Log("siegefix: ON; queued-request node budget %u per tick (game 500)%s, incremental cap %u (game 200); "
        "local player's requests %s", g_budget, g_queueOwn ? ", the queue's own" : ", shared with the immediate searches",
        g_incr, g_playerSync ? "searched at once" : "queued as in the game");
}

void SiegeFix_Remove() {
    Hook_Remove(g_slots, HOOK_COUNT);
    if (!g_base) return;
    ApplyPatches(false);
    for (int i = 0; i < PATCH_COUNT; i++)
        if (!PatchIs(i, g_patchDefs[i].original))
            Log("siegefix: WARNING, %s at RVA 0x%06X not restored (bytes changed by someone else)",
                g_patchDefs[i].func, g_patchDefs[i].rva);
    Log("siegefix: local player's requests searched at once %ld, other queued requests %ld, entities not "
        "recognised %ld, queue budget resets %ld, local army reads %ld (armies %ld), patch failures %ld", g_fSynced,
        g_fNotPlayer, g_fBadEntity, g_fOwnResets, g_localReads, g_localArmies, g_fPatchFail);
}
