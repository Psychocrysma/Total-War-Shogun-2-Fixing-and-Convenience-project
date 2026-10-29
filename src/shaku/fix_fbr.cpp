// fix_fbr.cpp -- fire-by-rank fix (matchlock fire by rank fires sooner, every rank fires, Hattori /
// Ikko Ikki gun units keep firing).
//
// From ShakuEight/Shogun2-fix (fixes/fire-by-rank-fix/src/fbrfix.cpp), Copyright 2026 ShakuEight,
// Apache License 2.0 (third_party/LICENSE-APACHE-2.0). Ported to this project: the fix itself is
// unchanged, with every rule at its default (the dev build's state with no environment variables
// set); the development-only parts (toggle key, MessageBeep cues, environment-variable switches,
// the research probe with its four extra hooks and CSV files, harness exports) are removed, the
// detour stubs come from hook.h's GCC macros, and the log lines go to tws2fix.log.
//
// Fault: Shogun 2's fire by rank is the group fire task (vtable 0x115C1048): the men are put in
// groups by formation rank (at most 3); the task waits until every man of the current group is
// "loaded and ready", orders the group to fire, waits until every man has finished his fire order,
// gives the group its (kneeling) reload orders, and moves to the next group.
//   - Hattori / Ikko Ikki gun units carry a sword officer (officer_katana: katana animations, no
//     gun). His kneeling reload order never completes, so after the first rotation the wait for
//     his group never ends: the unit stops firing and reloading for good.
//   - The wait before the first volley lasts about 10 s: it waits for the slowest ready pose.
//   - A man's reload time is set at his shot but only runs once he has his reload order, which
//     comes when his whole group is done, including men who never get a shot (2 to 9 s idle).
//   - Ranks beyond the third never fire; one man still reloading holds his whole group; the turn
//     goes strictly in order, so a loaded group waits while the group whose turn it is reloads.
//
// The fix (ShakuEight's notes): four entry hooks on functions only the group fire task uses.
//   FUN_10188290 (ready / reload order for one man, the task's vtable slot +0x84): none for a man
//     outside his unit's shooter class (soldier +0x48C differs from the unit's most common value:
//     the sword officer), and no second reload order for a man already reloading under his early
//     one or sitting out.
//   FUN_1017b980 (group "loaded and ready" test), answered by the fix:
//     - every rank fires: the group count (task +0x38) is raised to the formation's ranks (at most
//       64) before the first volley (group 0, count 3: the constructor FUN_101295d0's cap);
//     - front rank first: the front-most group with 75 percent of its shooters loaded takes the
//       turn (task +0x40 is written), whichever group's turn it was, and gets the ready order the
//       wait's entry (FUN_1019a9a0) gave the others; a group whose last turn had shots from fewer
//       than a quarter of its men (no line of fire) only fires on its own turn;
//     - that group fires when all its shooters are loaded and ready, or loaded and the wait has
//       lasted 15 ticks (1.5 s), or 10 ticks (1 s) after 75 percent of them are loaded and ready;
//     - men outside the shooter class are not waited for.
//   FUN_101883f0 (fire order for one man, vtable slot +0x80): none for a man still reloading; he
//     sits the volley out.
//   FUN_1017b9d0 (group "fire completed" test), answered by the fix: a shooter who has fired and
//     finished his fire order gets the game's own kneeling reload order at once (FUN_10188290 with
//     the exit FUN_101be8f0's after-fire flag) and counts as done; men sitting out do not count.
//     The turn also ends 50 ticks (5 s) after the group's last shot, so a man who aims but never
//     gets a shot does not hold the unit.
// "Loaded" is soldier +0x390 (reload time) at or below 0; "ready" is the game's own test
// FUN_103cbe40 (ECX = soldier), called directly after a byte check of its entry. The group table
// (task +0x48, dword group per soldier index +0x218) is filled by the game from the formation on
// the task's first tick and on every re-form (FUN_101540f0), so a new formation gets new ranks;
// each man keeps his own reload time.
// Self-check: 20 group tasks in a row without the decoded layout (and no answer given yet)
// switch the fix off for the session.

#include "log.h"
#include "hook.h"
#include "gamemem.h"
#include "stack_offsets_fbr.h"
#include <windows.h>
#include <cstdint>
#include <cstring>

static uint32_t g_base = 0;

// ---------------------------------------------------------------------------
// Hooks (stolen bytes recorded from the analysed binary; all relocatable, no branch into the range)
// ---------------------------------------------------------------------------

enum {
    H_GROUP_READY = 0, // FUN_1017b980 entry: group task "current group loaded and ready"
    H_GROUP_ORDER,     // FUN_10188290 entry: group task gives one man his ready / reload order
    H_GROUP_DONE,      // FUN_1017b9d0 entry: group task "current group's fire orders done" (early reload)
    H_GROUP_FIRE,      // FUN_101883f0 entry: group task gives one man his fire order (quorum)
    HOOK_COUNT
};

static const HookDef g_hookDefs[HOOK_COUNT] = {
    { "FUN_1017b980", "group loaded and ready test", 0x17B980, 5,
      { 0x53, 0x56, 0x57, 0x8B, 0xF9 }, -1, false },             // PUSH EBX; PUSH ESI; PUSH EDI; MOV EDI,ECX
    { "FUN_10188290", "group ready / reload order", 0x188290, 5,
      { 0x83, 0xEC, 0x50, 0x8B, 0xC1 }, -1, false },             // SUB ESP,0x50; MOV EAX,ECX
    { "FUN_1017b9d0", "group fire completed test", 0x17B9D0, 5,
      { 0x53, 0x56, 0x57, 0x8B, 0xF1 }, -1, false },             // PUSH EBX; PUSH ESI; PUSH EDI; MOV ESI,ECX
    // only referenced from the group task vtable, slot +0x80
    { "FUN_101883f0", "group fire order", 0x1883F0, 8,
      { 0x83, 0xEC, 0x50, 0x56, 0x8B, 0x74, 0x24, 0x58 }, -1, false },   // SUB ESP,0x50; PUSH ESI; MOV ESI,[ESP+0x58]
};

static HookSlot g_slots[HOOK_COUNT] = {};
extern "C" uint8_t* fbr_trampolines[HOOK_COUNT];
uint8_t* fbr_trampolines[HOOK_COUNT] = {};

// ---------------------------------------------------------------------------
// Engine layout (decoded statically, see the header)
// ---------------------------------------------------------------------------

static const uint32_t TASK_OFF_UNIT = 0x04;          // squad task: unit
// Group fire task (the one Shogun 2's fire by rank runs; constructor FUN_101295d0): groups capped at 3.
static const uint32_t RVA_GROUP_TASK_VT = 0x15C1048;
static const uint32_t GRP_OFF_COUNT = 0x38, GRP_OFF_CUR = 0x40;
static const uint32_t GRP_OFF_TABLE = 0x48;          // dword group per soldier index
static const uint32_t UNIT_OFF_COUNT = 0x18, UNIT_OFF_SOLDIERS = 0x1C;
static const uint32_t SOL_OFF_UNIT = 0x3B4, SOL_OFF_INDEX = 0x218, SOL_OFF_RELOAD = 0x390;
static const uint32_t MAX_MEN = 512;

static uint32_t Tick() { return GameTickAt(g_base); }

static bool Rd32(uint32_t a, uint32_t* v) { return MemcpyTry(v, a, 4); }

// A unit: plausible soldier array whose first soldier links back to it.
static bool IsUnit(uint32_t unit, uint32_t* count, uint32_t* arr) {
    uint32_t n, a, s0, back;
    if (!PtrPlausible(unit) || !Rd32(unit + UNIT_OFF_COUNT, &n) || !Rd32(unit + UNIT_OFF_SOLDIERS, &a)) return false;
    if (n == 0 || n > MAX_MEN || !PtrPlausible(a) || !Rd32(a, &s0) || !PtrPlausible(s0)) return false;
    if (!Rd32(s0 + SOL_OFF_UNIT, &back) || back != unit) return false;
    *count = n;
    *arr = a;
    return true;
}

static bool IsGroupTask(uint32_t task) {
    uint32_t vt;
    return PtrPlausible(task) && Rd32(task, &vt) && vt == g_base + RVA_GROUP_TASK_VT;
}

// ---------------------------------------------------------------------------
// The fix
// ---------------------------------------------------------------------------

static const uint32_t RVA_READY_TEST = 0x3CBE40;     // FUN_103cbe40: a soldier's order is done (ready)
static const uint32_t SOL_OFF_CLASS = 0x48C;         // 8 for every gun man seen, 0x10 for the sword officer
static const uint32_t GRACE_DEFAULT = 15;            // game ticks (0.1 s each)
typedef uint32_t (__fastcall *ReadyFn)(uint32_t soldier);

static volatile LONG g_fixOn = 0;
static volatile LONG g_fixDisabled = 0;              // self-check switched it off
static const uint32_t g_grace = GRACE_DEFAULT;
static ReadyFn g_ready = nullptr;
static volatile LONG g_fReadyAll = 0;     // group test answered "ready": every shooter ready
static volatile LONG g_fReadyGrace = 0;   // ... loaded, some not ready yet, grace time over
static volatile LONG g_fNotLoaded = 0;    // ... "not ready": a shooter still reloading
static volatile LONG g_fWaiting = 0;      // ... "not ready": within the grace time
static volatile LONG g_fExcludedMen = 0;  // men left out of a group test (not of the shooter class)
static volatile LONG g_fOrderSkipped = 0; // ready / reload orders not given to such men
static volatile LONG g_fOrderGiven = 0;
static volatile LONG g_fBadTask = 0;      // group task fields implausible (self-check)
static const bool g_allRanks = true;      // every rank fires in turn (the game: 3 groups)
static const uint32_t MAX_GROUPS = 64;    // ranks beyond this never fire
static volatile LONG g_fAllRanks = 0;     // group tasks whose count was raised
static const bool g_frontFirst = true;    // the front-most loaded group fires next (the game: strict order)
static volatile LONG g_fSwitched = 0;     // group tests that moved the turn to a front-most loaded group
// Early reload (FUN_1017b9d0 hook)
static const uint32_t RVA_GROUP_ORDER = 0x188290;   // FUN_10188290, the task's slot +0x84
static const uint32_t GRP_VT_SLOT_ORDER = 0x84;
typedef void (__fastcall *GroupOrderFn)(uint32_t task, uint32_t edx, uint32_t soldier, uint32_t afterFire);
static const bool g_earlyReload = true;
static const uint32_t g_fireGrace = 50;   // end the turn this long (ticks) after the last shot (0 = off)
static volatile LONG g_fEarly = 0;        // early reload orders given
static volatile LONG g_fDone = 0;         // fire-completed test answered "done"
static volatile LONG g_fFiring = 0;       // ... answered "not done": a shooter still in his fire order
static volatile LONG g_fCut = 0;          // turns ended by the fire grace
static volatile LONG g_fNotFired = 0;     // shooters who had not fired when their turn was cut
static volatile LONG g_fExitSkipped = 0;  // second reload orders not given to men already reloading
// Quorum (FUN_1017b980 answer, FUN_101883f0 hook)
static const uint32_t g_quorum = 75;      // percent of shooters loaded and ready (0 = off)
static const uint32_t g_quorumWait = 10;  // ticks after the quorum is reached
static volatile LONG g_fQuorum = 0;       // group tests answered "fire" with men still reloading
static volatile LONG g_fQuorumWait = 0;   // ... "not yet": quorum reached, within the wait
static volatile LONG g_fSatOut = 0;       // fire orders not given to men still reloading

// The unit's shooter class: the most common soldier +0x48C value among its men.
static bool ShooterClass(uint32_t unit, uint32_t* cls) {
    uint32_t count, arr;
    if (!IsUnit(unit, &count, &arr)) return false;
    uint32_t vals[8] = {}, hits[8] = {};
    int nv = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t s, c;
        if (!Rd32(arr + 4 * i, &s) || !Rd32(s + SOL_OFF_CLASS, &c)) continue;
        int k = 0;
        while (k < nv && vals[k] != c) k++;
        if (k == nv) { if (nv == 8) continue; vals[nv++] = c; }
        hits[k]++;
    }
    if (!nv) return false;
    int best = 0;
    for (int k = 1; k < nv; k++) if (hits[k] > hits[best]) best = k;
    *cls = vals[best];
    return true;
}

// How long the task has been asking about its current group: consecutive calls (one per tick
// while the wait state runs) with the same group number.
// qfirst: the first tick of this wait with the quorum reached (QUORUM_NONE: not reached yet).
struct WaitRec { uint32_t task, group, first, last, qfirst; };
static const uint32_t QUORUM_NONE = 0xFFFFFFFF;
static WaitRec g_waits[64];
static volatile LONG g_waitLock = 0;

static uint32_t WaitTicks(uint32_t task, uint32_t group, uint32_t tick) {
    while (InterlockedCompareExchange(&g_waitLock, 1, 0) != 0) {}
    WaitRec* w = nullptr;
    WaitRec* oldest = &g_waits[0];
    for (int i = 0; i < 64; i++) {
        if (g_waits[i].task == task) { w = &g_waits[i]; break; }
        if (g_waits[i].last < oldest->last) oldest = &g_waits[i];
    }
    if (!w) { w = oldest; w->task = task; w->group = group + 1; }
    if (w->group != group || tick < w->last || tick - w->last > 3) {
        w->group = group; w->first = tick; w->qfirst = QUORUM_NONE;
    }
    w->last = tick;
    uint32_t waited = tick - w->first;
    InterlockedExchange(&g_waitLock, 0);
    return waited;
}

// Ticks since the quorum was first reached in the task's current wait (WaitTicks ran this tick);
// a call without the quorum starts it again.
static uint32_t QuorumTicks(uint32_t task, uint32_t tick, bool reached) {
    uint32_t q = 0;
    while (InterlockedCompareExchange(&g_waitLock, 1, 0) != 0) {}
    for (int i = 0; i < 64; i++) {
        WaitRec& w = g_waits[i];
        if (w.task != task) continue;
        if (!reached) w.qfirst = QUORUM_NONE;
        else {
            if (w.qfirst == QUORUM_NONE) w.qfirst = tick;
            q = tick - w.qfirst;
        }
        break;
    }
    InterlockedExchange(&g_waitLock, 0);
    return q;
}

// Line of fire per group: how many of its shooters were given fire orders on its last turn and
// how many of them fired. A group that fired with fewer than a quarter of them (rear ranks with
// no line of fire, a target out of reach) stays loaded but may not take the turn out of order
// until it has had its own turn with shots. sig = the task's group table (a re-form or casualties
// change it): a new table clears the memory.
struct LosRec { uint32_t task, sig, last; uint16_t aimed[MAX_GROUPS], fired[MAX_GROUPS]; uint8_t turned[MAX_GROUPS]; };
static LosRec g_los[64];
static volatile LONG g_losLock = 0;

static LosRec* LosOf(uint32_t task, uint32_t tick) {   // with g_losLock held
    LosRec* oldest = &g_los[0];
    for (int i = 0; i < 64; i++) {
        if (g_los[i].task == task) { g_los[i].last = tick; return &g_los[i]; }
        if (g_los[i].last < oldest->last) oldest = &g_los[i];
    }
    memset(oldest, 0, sizeof(*oldest));
    oldest->task = task;
    oldest->last = tick;
    return oldest;
}

// Which groups may take the turn out of order (out[g] for g < groups).
static void LosCandidates(uint32_t task, uint32_t sig, uint32_t tick, uint32_t groups, bool* out) {
    while (InterlockedCompareExchange(&g_losLock, 1, 0) != 0) {}
    LosRec* r = LosOf(task, tick);
    if (r->sig != sig) {
        uint32_t keep = r->task;
        memset(r, 0, sizeof(*r));
        r->task = keep; r->sig = sig; r->last = tick;
    }
    for (uint32_t g = 0; g < groups; g++)
        out[g] = !r->turned[g] || (uint32_t)r->fired[g] * 4 >= r->aimed[g];
    InterlockedExchange(&g_losLock, 0);
}

static void LosNote(uint32_t task, uint32_t group, uint32_t aimed, uint32_t fired, uint32_t tick) {
    if (group >= MAX_GROUPS) return;
    while (InterlockedCompareExchange(&g_losLock, 1, 0) != 0) {}
    LosRec* r = LosOf(task, tick);
    r->turned[group] = 1;
    r->aimed[group] = (uint16_t)(aimed > 0xFFFF ? 0xFFFF : aimed);
    r->fired[group] = (uint16_t)(fired > 0xFFFF ? 0xFFFF : fired);
    InterlockedExchange(&g_losLock, 0);
}

// Self-check: a group task whose fields do not look like one. 20 in a row switch the fix off.
static void BadTask() {
    LONG n = InterlockedIncrement(&g_fBadTask);
    if (n >= 20 && g_fReadyAll + g_fReadyGrace + g_fNotLoaded + g_fWaiting + g_fQuorum + g_fQuorumWait == 0 &&
        !InterlockedExchange(&g_fixDisabled, 1)) {
        InterlockedExchange(&g_fixOn, 0);
        Log("fbrfix: DISABLED for this session, the first %ld group tasks did not have the decoded layout", n);
    }
}

// FUN_1017b980 entry (ECX = group task): answer "current group loaded and ready" ourselves.
static uint32_t HandleGroupReady(const uint8_t* ctx) {
    if (!g_fixOn) return HOOK_CONTINUE;
    uint32_t task = *(const uint32_t*)(ctx + FBR_OFF_ECX);
    uint32_t unit = 0, group = 0, groups = 0, table = 0, count, arr, cls;
    if (!Rd32(task + TASK_OFF_UNIT, &unit) || !IsUnit(unit, &count, &arr) || !Rd32(task + GRP_OFF_CUR, &group) ||
        !Rd32(task + GRP_OFF_COUNT, &groups) || !Rd32(task + GRP_OFF_TABLE, &table) || !PtrPlausible(table) ||
        groups == 0 || groups > MAX_GROUPS || group >= groups || !ShooterClass(unit, &cls)) {
        BadTask();
        return HOOK_CONTINUE;
    }
    // Every rank fires: the task caps its group count at 3 (constructor FUN_101295d0) while the
    // formation numbers its ranks 0..n-1, so ranks 4+ never fire. Before the first volley (group
    // 0) raise the count to the number of ranks present (at most MAX_GROUPS).
    if (g_allRanks && group == 0 && groups == 3) {
        uint32_t top = 0;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t s, idx, g;
            if (Rd32(arr + 4 * i, &s) && Rd32(s + SOL_OFF_INDEX, &idx) && idx < MAX_MEN && Rd32(table + 4 * idx, &g) &&
                g < MAX_GROUPS && g > top)
                top = g;
        }
        if (top + 1 > groups && WriteDwordTry(task + GRP_OFF_COUNT, top + 1)) {
            InterlockedIncrement(&g_fAllRanks);
            groups = top + 1;
        }
    }
    // One pass over the unit: per group its shooters, those still reloading, those loaded but not
    // ready (pose), and the men outside the shooter class.
    uint32_t gMen[MAX_GROUPS] = {}, gNotLoaded[MAX_GROUPS] = {}, gNotReady[MAX_GROUPS] = {}, gExcluded[MAX_GROUPS] = {};
    uint32_t sig = groups;   // the group table as the game filled it
    for (uint32_t i = 0; i < count; i++) {
        uint32_t s, idx, g, c;
        float reload;
        if (!Rd32(arr + 4 * i, &s) || !Rd32(s + SOL_OFF_INDEX, &idx) || idx >= MAX_MEN || !Rd32(table + 4 * idx, &g))
            return HOOK_CONTINUE;
        sig = sig * 31 + g + 7 * idx;
        if (g >= groups) continue;
        if (!Rd32(s + SOL_OFF_CLASS, &c) || !MemcpyTry(&reload, s + SOL_OFF_RELOAD, 4)) return HOOK_CONTINUE;
        if (c != cls) { gExcluded[g]++; continue; }
        gMen[g]++;
        if (reload > 0.0f) gNotLoaded[g]++;
        else if ((g_ready(s) & 0xFF) == 0) gNotReady[g]++;
    }
    // Front rank first: the front-most group with g_quorum percent of its shooters loaded (all of
    // them with the quorum off) fires next, whichever group's turn it is, unless it fired with
    // fewer than a quarter of its men on its last turn (then only on its own turn). None: the
    // current group waits, as in the game.
    if (g_frontFirst) {
        bool mayJump[MAX_GROUPS];
        LosCandidates(task, sig, Tick(), groups, mayJump);
        for (uint32_t g = 0; g < groups; g++) {
            uint32_t loaded = gMen[g] - gNotLoaded[g];
            bool eligible = gMen[g] && (g_quorum ? loaded * 100 >= g_quorum * gMen[g] : gNotLoaded[g] == 0);
            if (!eligible || (g != group && !mayJump[g])) continue;
            if (g != group && IsGroupTask(task) && WriteDwordTry(task + GRP_OFF_CUR, g)) {
                InterlockedIncrement(&g_fSwitched);
                // The wait's entry (FUN_1019a9a0) gave its ready orders to the current and later
                // groups only; a front group taking the turn gets the same order now, so its men
                // stand from their kneeling reload pose before the fire order. Its wait starts
                // with the next test.
                GroupOrderFn order = (GroupOrderFn)(g_base + RVA_GROUP_ORDER);
                for (uint32_t i = 0; i < count; i++) {
                    uint32_t s, idx, gg, c;
                    if (!Rd32(arr + 4 * i, &s) || !Rd32(s + SOL_OFF_INDEX, &idx) || idx >= MAX_MEN ||
                        !Rd32(table + 4 * idx, &gg) || gg != g || !Rd32(s + SOL_OFF_CLASS, &c) || c != cls)
                        continue;
                    order(task, 0, s, 0);
                }
                return HOOK_RETURN(0);
            }
            break;
        }
    }
    uint32_t tick = Tick();
    uint32_t waited = WaitTicks(task, group, tick);
    uint32_t men = gMen[group], excluded = gExcluded[group], notLoaded = gNotLoaded[group], notReady = gNotReady[group];
    if (men == 0) return HOOK_CONTINUE;   // nothing to decide: the game's own test
    if (excluded) InterlockedExchangeAdd(&g_fExcludedMen, (LONG)excluded);
    uint32_t result;
    // Quorum: shooters loaded and ready (a loaded man counts as ready once the grace time is over)
    uint32_t readyToFire = men - notLoaded - (waited >= g_grace ? 0 : notReady);
    bool quorum = notLoaded && g_quorum && readyToFire * 100 >= g_quorum * men;
    uint32_t q = QuorumTicks(task, tick, quorum);
    if (quorum && q >= g_quorumWait) { result = 1; InterlockedIncrement(&g_fQuorum); }
    else if (quorum) { result = 0; InterlockedIncrement(&g_fQuorumWait); }
    else if (notLoaded) { result = 0; InterlockedIncrement(&g_fNotLoaded); }
    else if (!notReady) { result = 1; InterlockedIncrement(&g_fReadyAll); }
    else if (waited >= g_grace) { result = 1; InterlockedIncrement(&g_fReadyGrace); }
    else { result = 0; InterlockedIncrement(&g_fWaiting); }
    return HOOK_RETURN(result);
}

// A group's turn in WAIT_FOR_GROUP_FIRE_COMPLETED: consecutive calls (one per tick) for the same
// task and group. key = first tick and group, so a man's early order belongs to one turn only.
struct TurnRec { uint32_t task, group, first, last, lastShot, shots; };
static TurnRec g_turns[64];
static volatile LONG g_turnLock = 0;

static void TurnOf(uint32_t task, uint32_t group, uint32_t tick, uint32_t fired, uint32_t* key, uint32_t* sinceShot) {
    while (InterlockedCompareExchange(&g_turnLock, 1, 0) != 0) {}
    TurnRec* t = nullptr;
    TurnRec* oldest = &g_turns[0];
    for (int i = 0; i < 64; i++) {
        if (g_turns[i].task == task) { t = &g_turns[i]; break; }
        if (g_turns[i].last < oldest->last) oldest = &g_turns[i];
    }
    if (!t) { t = oldest; t->task = task; t->group = group + 1; }
    if (t->group != group || tick < t->last || tick - t->last > 3) {
        t->group = group; t->first = t->lastShot = tick; t->shots = 0;
    }
    t->last = tick;
    if (fired > t->shots) { t->shots = fired; t->lastShot = tick; }
    *key = (t->first << 8) | (group & 0xFF);
    *sinceShot = t->shots ? tick - t->lastShot : 0;
    InterlockedExchange(&g_turnLock, 0);
}

// The task's current turn key, or 0 (no turn seen for it in the last ticks).
static uint32_t CurrentTurn(uint32_t task, uint32_t tick) {
    uint32_t key = 0;
    while (InterlockedCompareExchange(&g_turnLock, 1, 0) != 0) {}
    for (int i = 0; i < 64; i++)
        if (g_turns[i].task == task && tick >= g_turns[i].last && tick - g_turns[i].last <= 3) {
            key = (g_turns[i].first << 8) | (g_turns[i].group & 0xFF);
            break;
        }
    InterlockedExchange(&g_turnLock, 0);
    return key;
}

// Soldiers who had their early reload order: soldier -> (task, turn key); and soldiers who sat
// a volley out (no fire order while reloading): (satTask, satTick).
struct EarlyRec { uint32_t soldier, task, key, satTask, satTick; };
static const int EARLY_TABLE = 4096;   // power of two
static EarlyRec g_early[EARLY_TABLE];
static volatile LONG g_earlyLock = 0;

static EarlyRec* EarlyOf(uint32_t s, bool add) {
    uint32_t h = ((s >> 2) * 2654435761u) & (EARLY_TABLE - 1);
    EarlyRec* r = nullptr;
    while (InterlockedCompareExchange(&g_earlyLock, 1, 0) != 0) {}
    for (int i = 0; i < 64; i++) {
        EarlyRec& e = g_early[(h + i) & (EARLY_TABLE - 1)];
        if (e.soldier == s) { r = &e; break; }
        if (e.soldier == 0) { if (add) { e.soldier = s; r = &e; } break; }
    }
    if (!r && add) r = &g_early[h];   // table crowded: reuse the home slot
    if (r && add && r->soldier != s) { r->soldier = s; r->task = 0; r->key = 0; r->satTask = 0; r->satTick = 0; }
    InterlockedExchange(&g_earlyLock, 0);
    return r;
}

static bool HadEarly(uint32_t s, uint32_t task, uint32_t key) {
    EarlyRec* e = EarlyOf(s, false);
    return e && e->task == task && e->key == key;
}

// The man sat out the volley whose fire-completed turn started at turnFirst (the fire orders come
// on the tick before the turn's first test).
static bool SatOut(uint32_t s, uint32_t task, uint32_t turnFirst) {
    EarlyRec* e = EarlyOf(s, false);
    return e && e->satTask == task && e->satTick + 5 >= turnFirst && e->satTick <= turnFirst + 1;
}

// FUN_101883f0 entry (ECX = group task, arg1 = soldier, arg2 = target record): the group's fire
// order for one man. A man still reloading (the group fired on the quorum) gets none: he goes on
// reloading and sits this volley out.
static uint32_t HandleGroupFire(const uint8_t* ctx) {
    if (!g_fixOn || !g_quorum) return HOOK_CONTINUE;
    uint32_t task = *(const uint32_t*)(ctx + FBR_OFF_ECX);
    uint32_t s = *(const uint32_t*)(ctx + FBR_OFF_ARG1);
    float reload = 0.0f;
    if (!IsGroupTask(task) || !MemcpyTry(&reload, s + SOL_OFF_RELOAD, 4) || reload <= 0.0f) return HOOK_CONTINUE;
    EarlyRec* e = EarlyOf(s, true);
    if (e) { e->satTask = task; e->satTick = Tick(); }
    InterlockedIncrement(&g_fSatOut);
    return HOOK_RETURN(0);   // the function's own RET 8
}

// FUN_1017b9d0 entry (ECX = group task): "has every man of the current group finished his fire
// order". The game's answer holds the men who fired until the last man is done, and their reload
// time only runs once the exit FUN_101be8f0 gives them their reload order. Here each shooter who
// has fired (reload time above 0: the wait before the fire order needed everyone loaded) and is
// done with his fire order gets that order at once; he then counts as done although his new
// order is open. Shooters who have not fired count as done when their fire order is.
static uint32_t HandleGroupDone(const uint8_t* ctx) {
    if (!g_fixOn || (!g_earlyReload && !g_fireGrace)) return HOOK_CONTINUE;
    uint32_t task = *(const uint32_t*)(ctx + FBR_OFF_ECX);
    uint32_t unit = 0, group = 0, groups = 0, table = 0, count, arr, cls, vt = 0, slot = 0;
    if (!Rd32(task, &vt) || vt != g_base + RVA_GROUP_TASK_VT || !Rd32(vt + GRP_VT_SLOT_ORDER, &slot) ||
        slot != g_base + RVA_GROUP_ORDER || !Rd32(task + TASK_OFF_UNIT, &unit) || !IsUnit(unit, &count, &arr) ||
        !Rd32(task + GRP_OFF_CUR, &group) || !Rd32(task + GRP_OFF_COUNT, &groups) ||
        !Rd32(task + GRP_OFF_TABLE, &table) || !PtrPlausible(table) || groups == 0 || groups > MAX_GROUPS ||
        group >= groups || !ShooterClass(unit, &cls)) {
        BadTask();
        return HOOK_CONTINUE;
    }
    uint32_t tick = Tick();
    uint32_t men = 0, excluded = 0, fired = 0, firing = 0, notFired = 0, satOut = 0;
    uint32_t todo[MAX_MEN];
    uint32_t nTodo = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t s, idx, g, c;
        float reload;
        if (!Rd32(arr + 4 * i, &s) || !Rd32(s + SOL_OFF_INDEX, &idx) || idx >= MAX_MEN || !Rd32(table + 4 * idx, &g))
            return HOOK_CONTINUE;
        if (g != group) continue;
        if (!Rd32(s + SOL_OFF_CLASS, &c) || !MemcpyTry(&reload, s + SOL_OFF_RELOAD, 4)) return HOOK_CONTINUE;
        if (c != cls) { excluded++; continue; }
        men++;
        if (reload > 0.0f) {
            fired++;
            if (nTodo < MAX_MEN) todo[nTodo++] = s;   // decided below, once the turn key is known
        } else if ((g_ready(s) & 0xFF) == 0) {
            firing++;
            notFired++;
        }
    }
    if (men == 0) return HOOK_CONTINUE;
    uint32_t key, sinceShot;
    TurnOf(task, group, tick, fired, &key, &sinceShot);
    GroupOrderFn order = (GroupOrderFn)(g_base + RVA_GROUP_ORDER);
    for (uint32_t k = 0; k < nTodo; k++) {
        uint32_t s = todo[k];
        if (HadEarly(s, task, key)) continue;
        if (SatOut(s, task, key >> 8)) { fired--; satOut++; continue; }   // reloading, had no fire order: not his volley
        if ((g_ready(s) & 0xFF) == 0) { firing++; continue; }   // fired, fire order not done yet
        if (!g_earlyReload) continue;                          // done with his fire order: as the game counts it
        order(task, 0, s, 1);                                  // the exit's own call: kneeling reload order
        EarlyRec* e = EarlyOf(s, true);
        if (e) { e->task = task; e->key = key; }
        InterlockedIncrement(&g_fEarly);
    }
    uint32_t result;
    if (!firing) { result = 1; InterlockedIncrement(&g_fDone); }
    else if (g_fireGrace && fired && sinceShot >= g_fireGrace) {
        result = 1;
        InterlockedIncrement(&g_fCut);
        InterlockedExchangeAdd(&g_fNotFired, (LONG)notFired);
    } else { result = 0; InterlockedIncrement(&g_fFiring); }
    if (result) LosNote(task, group, men - satOut, fired, tick);
    if (excluded) InterlockedExchangeAdd(&g_fExcludedMen, (LONG)excluded);
    return HOOK_RETURN(result);
}

// FUN_10188290 entry (ECX = group task, arg1 = soldier, arg2 = after-fire flag): no ready /
// reload order for a man outside the unit's shooter class, and no second reload order for a man
// still reloading who had his early one this turn or sat the volley out (the exit FUN_101be8f0
// orders the whole group).
static uint32_t HandleGroupOrder(const uint8_t* ctx) {
    if (!g_fixOn) return HOOK_CONTINUE;
    uint32_t task = *(const uint32_t*)(ctx + FBR_OFF_ECX);
    uint32_t s = *(const uint32_t*)(ctx + FBR_OFF_ARG1);
    uint32_t unit = 0, cls, c;
    if (!Rd32(task + TASK_OFF_UNIT, &unit) || !Rd32(s + SOL_OFF_CLASS, &c) || !ShooterClass(unit, &cls))
        return HOOK_CONTINUE;
    if (c == cls) {
        float reload = 0.0f;
        uint32_t key = g_earlyReload ? CurrentTurn(task, Tick()) : 0;
        if (key && MemcpyTry(&reload, s + SOL_OFF_RELOAD, 4) && reload > 0.0f &&
            (HadEarly(s, task, key) || SatOut(s, task, key >> 8))) {
            InterlockedIncrement(&g_fExitSkipped);
            return HOOK_RETURN(0);
        }
        InterlockedIncrement(&g_fOrderGiven);
        return HOOK_CONTINUE;
    }
    InterlockedIncrement(&g_fOrderSkipped);
    return HOOK_RETURN(0);   // the function's own RET 8
}

// ---------------------------------------------------------------------------
// Detours
// ---------------------------------------------------------------------------

extern "C" uint32_t __stdcall fbr_Handler0(const uint8_t* ctx) { return HandleGroupReady(ctx); }
extern "C" uint32_t __stdcall fbr_Handler1(const uint8_t* ctx) { return HandleGroupOrder(ctx); }
extern "C" uint32_t __stdcall fbr_Handler2(const uint8_t* ctx) { return HandleGroupDone(ctx); }
extern "C" uint32_t __stdcall fbr_Handler3(const uint8_t* ctx) { return HandleGroupFire(ctx); }

HOOK_DETOUR_RET(fbr_Detour0, fbr_Handler0, fbr_trampolines, 0, 0)   // FUN_1017b980: __thiscall, no stack args
HOOK_DETOUR_RET(fbr_Detour1, fbr_Handler1, fbr_trampolines, 1, 8)   // FUN_10188290: __thiscall, 2 stack args
HOOK_DETOUR_RET(fbr_Detour2, fbr_Handler2, fbr_trampolines, 2, 0)   // FUN_1017b9d0: __thiscall, no stack args
HOOK_DETOUR_RET(fbr_Detour3, fbr_Handler3, fbr_trampolines, 3, 8)   // FUN_101883f0: __thiscall, 2 stack args

static const HookDetour g_detours[HOOK_COUNT] = { fbr_Detour0, fbr_Detour1, fbr_Detour2, fbr_Detour3 };

// ---------------------------------------------------------------------------
// Install / remove
// ---------------------------------------------------------------------------

void FbrFix_Install() {
    Mem_Init();
    uint32_t size = 0;
    uint8_t* base = Hook_GameModule(false, &size);
    if (!base) { Log("fbrfix: REFUSED, empire.retail.dll is not loaded"); return; }
    if (size != HOOK_ANALYSED_IMAGE_SIZE) { Log("fbrfix: REFUSED, SizeOfImage 0x%08X is not the analysed build", size); return; }
    g_base = (uint32_t)(uintptr_t)base;
    for (int h = 0; h < HOOK_COUNT; h++) {
        if (!Hook_SiteMatches(base, g_hookDefs[h])) {
            Log("fbrfix: REFUSED, bytes at RVA 0x%06X (%s) differ from the analysed binary", g_hookDefs[h].rva,
                g_hookDefs[h].func);
            return;
        }
    }
    // the game's ready test, called by the group test: its entry must be the analysed one too
    static const uint8_t readyEntry[] = { 0x80, 0xB9, 0x84, 0x03, 0x00, 0x00, 0x01 };   // CMP byte [ECX+0x384],1
    if (memcmp(base + RVA_READY_TEST, readyEntry, sizeof(readyEntry)) != 0) {
        Log("fbrfix: REFUSED, FUN_103cbe40 differs from the analysed binary");
        return;
    }
    g_ready = (ReadyFn)(base + RVA_READY_TEST);
    InterlockedExchange(&g_fixOn, 1);
    if (!Hook_Prepare(base, g_hookDefs, HOOK_COUNT, nullptr, fbr_trampolines, g_slots)) {
        Log("fbrfix: REFUSED, no trampoline memory");
        return;
    }
    Hook_Patch(g_detours, g_slots, HOOK_COUNT);
    Log("fbrfix: ON, %d hooks installed: every rank fires, front rank first, grace %u ticks (%.1f game s), quorum %u%% "
        "after %u ticks (men still reloading sit the volley out), early reload, fire grace %u ticks after the group's "
        "last shot; men outside the shooter class get no ready / reload order and are not waited for",
        HOOK_COUNT, g_grace, g_grace / 10.0, g_quorum, g_quorumWait, g_fireGrace);
}

void FbrFix_Remove() {
    Hook_Remove(g_slots, HOOK_COUNT);
    if (g_base)
        Log("fbrfix: group tests: ready %ld, grace %ld, reloading %ld, waiting %ld; men left out %ld; ready orders "
            "skipped %ld, given %ld; bad task %ld; group counts raised (all ranks) %ld", g_fReadyAll, g_fReadyGrace,
            g_fNotLoaded, g_fWaiting, g_fExcludedMen, g_fOrderSkipped, g_fOrderGiven, g_fBadTask, g_fAllRanks);
    if (g_base)
        Log("fbrfix: fire-completed tests: done %ld, firing %ld, cut by the fire grace %ld (%ld men not fired); "
            "early reload orders %ld, second reload orders skipped %ld", g_fDone, g_fFiring, g_fCut, g_fNotFired,
            g_fEarly, g_fExitSkipped);
    if (g_base)
        Log("fbrfix: quorum: fired %ld, waited %ld ticks; fire orders not given to men reloading %ld; turns moved "
            "to a front-most loaded group %ld", g_fQuorum, g_fQuorumWait, g_fSatOut, g_fSwitched);
}
