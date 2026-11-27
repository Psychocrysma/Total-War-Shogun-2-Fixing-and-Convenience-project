// Battle rule fixes in the engine (each one switched in the [battle] section of tws2fix.ini).
//
// 1. ai_difficulty_accuracy (opt-in; default 1 = vanilla).
//    Battle difficulty is stored on each battle army at +0x280 ("army_handicap" in the battle script
//    interface, FUN_101c2c00) with the "human controlled" byte at +0x270. FUN_104324f0 (thiscall
//    (this, entity, char* human, int* handicap), ret 0xC) reads both for a land unit or a ship and,
//    for an AI army, lowers the handicap when that army already outnumbers its enemies (clamped 0..2).
//    For an AI army 1 = Hard and 2 = Very Hard (Legendary uses 2). The result goes into two stat
//    provider objects used for missile fire:
//      - FUN_1043d500 (vtable 0x115E736C slot 7, "Attacker Core Marksmanship" in the engine's own debug
//        text, FUN_10b92b60): AI land units x1.3 on Hard, x1.5 on Very Hard; AI ships x1.15 / x1.3.
//      - FUN_104646c0 (vtable 0x115E73BC slot 1, "Projectile Marksmanship Bonus"): AI +20 / +30.
//    Both feed the chance to hit (FUN_10b92b60), and core marksmanship also the kill chance
//    (FUN_10b91cb0, "Attacker Accuracy"). ai_difficulty_accuracy=0 points those four multipliers at
//    1.0 and the two bonuses at 0.0. Melee (+2 / +4 attack, FUN_1042a4b0), the human player's Easy
//    bonus and the AI's Easy penalty are not touched.
//    No separate reload bonus has been found yet: the handicap field (+0x280) has no other reader in
//    the engine code, so a reload bonus, if there is one, reaches the units some other way.
//
// 2. projectile_sound_priority (default 1 = fixed).
//    The firing sound of a projectile is chosen from banks/sound_bank_projectile_fire.xml by
//    FUN_10cc71b0 (thiscall (bank, projectile, audio_distance, game_mode, unit), ret 0x10; callers
//    FUN_10cc8b10 and FUN_10cc8bf0, the latter with no unit). Each bank entry (+0x10 sound id) has five
//    key filters: gun_type +0x14 (weight 4), shot_type +0x2c (1), audio_distance +0x44 (0.25),
//    game_mode +0x5c (0.25) and unit +0x74 (10); a filter is {vtable, ..., +0x10 count, +0x14 int list}.
//    A filter scores weight x10 when its list holds the value, x1 when the list is empty, and 0 when
//    the list is set but does not hold the value; the entry with the highest total wins. A listed but
//    different gun or shot type only loses points, so an entry made for a unit's cannon (unit match
//    = 100 points) beats the right matchlock / bow / torch entry (gun match = 40) when that unit fires
//    its other weapon, and the shot plays as a cannon.
//    The fix replaces the selection: entries whose gun_type or shot_type list excludes the projectile
//    rank below every entry that accepts it; within each group the game's own score decides, in the
//    game's order. When every entry accepts the projectile the result is the game's. The incoming,
//    impact and other banks are not touched. (The game's per-bank result cache is bypassed.)
//
// 3. fear_stacking (default 1 = fixed).
//    Fear from traits, ancillaries, skills and effect bundles reaches a unit through the unit-record
//    bonus "scares_men" (effect_bonus_value_unit_record_junctions; category 0x10, bonus 8). The battle
//    unit builder FUN_10151f10 sums it (FUN_10bcf460(unit, 8)) and, at 0x10152B23, only tests the sum
//    for zero: any non-zero total sets the unit's fear to the flat _kv_morale value
//    ume_concerned_unit_frightened negated (vanilla 4), and contributions of opposite sign cancel
//    (Boshin "Dreaded Force" -1 against a Bloodthirsty +1 leaves the general with no fear at all).
//    The fix stores the total itself as the fear value: three +1 traits give 3, a total of 0 or below
//    gives none. The unit-attribute fear of units like the warrior monks is handled elsewhere and is
//    not touched.
#include "common.h"
#include <cstring>

namespace {

uint8_t* g_base = nullptr;
constexpr uintptr_t kPreferred = 0x10000000;   // the engine's preferred image base (operands are relocated)

// Instruction with an absolute data operand at +4 (movss/addss xmm, [abs]); the operand is checked
// against the address the engine itself would use (relocated to the actual base).
struct OperandSite { uint32_t rva; uint8_t op[4]; uint32_t dataVa; const char* what; };

bool SiteOk(const OperandSite& s) {
    const uint8_t* p = g_base + s.rva;
    uint32_t want = static_cast<uint32_t>(s.dataVa - kPreferred + reinterpret_cast<uintptr_t>(g_base));
    uint32_t have; memcpy(&have, p + 4, 4);
    return memcmp(p, s.op, 4) == 0 && have == want;
}
bool PointAt(const OperandSite& s, const float* value) {
    uint32_t a = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(value));
    return WriteCode(g_base + s.rva + 4, &a, 4);
}

// ---------------------------------------------------------------- 1. AI accuracy on Hard / Very Hard
const float kOne = 1.0f, kZero = 0.0f;
const OperandSite kAccMul[] = {
    { 0x0043D5D3, { 0xF3, 0x0F, 0x10, 0x0D }, 0x115E8B38, "AI ship Hard x1.15" },
    { 0x0043D5EC, { 0xF3, 0x0F, 0x10, 0x0D }, 0x115DA008, "AI land Hard x1.3" },
    { 0x0043D60E, { 0xF3, 0x0F, 0x10, 0x0D }, 0x115DA008, "AI ship Very Hard x1.3" },
    { 0x0043D627, { 0xF3, 0x0F, 0x10, 0x0D }, 0x115C26AC, "AI land Very Hard x1.5" },
};
const OperandSite kAccAdd[] = {
    { 0x0046477B, { 0xF3, 0x0F, 0x58, 0x05 }, 0x115C2700, "AI Hard +20" },
    { 0x00464796, { 0xF3, 0x0F, 0x58, 0x05 }, 0x115C2708, "AI Very Hard +30" },
};

// Diagnostics: log each distinct (human, handicap) pair the engine hands to the missile code.
constexpr uint32_t RVA_HANDICAP = 0x004324F0;
const uint8_t kHandicapHead[] = { 0x51, 0x55, 0x56, 0x8B, 0x74, 0x24, 0x10 };
typedef void (__fastcall *HandicapFn)(void*, void*, void*, char*, int*);
HandicapFn o_Handicap = nullptr;
volatile LONG g_seenMask = 0;
void __fastcall my_Handicap(void* self, void* edx, void* entity, char* human, int* handicap) {
    o_Handicap(self, edx, entity, human, handicap);
    int h = *handicap; bool hu = *human != 0;
    if (h < -3 || h > 3) return;
    LONG bit = 1L << ((h + 3) + (hu ? 7 : 0));
    if (!(InterlockedOr(&g_seenMask, bit) & bit))
        Log("battle_rules: missile fire from a %s army with battle handicap %d%s", hu ? "human" : "AI", h,
            (!hu && h > 0 && !g_cfg.ai_difficulty_accuracy) ? " - its accuracy bonus is removed" : "");
}

void InstallAccuracy() {
    if (g_cfg.ai_difficulty_accuracy) { Log("ai_difficulty_accuracy: on (vanilla AI accuracy bonus on Hard / Very Hard)"); }
    else {
        for (const auto& s : kAccMul) if (!SiteOk(s)) { Log("ai_difficulty_accuracy: bytes at RVA 0x%06X (%s) differ; NOT applied", s.rva, s.what); return; }
        for (const auto& s : kAccAdd) if (!SiteOk(s)) { Log("ai_difficulty_accuracy: bytes at RVA 0x%06X (%s) differ; NOT applied", s.rva, s.what); return; }
        int ok = 0;
        for (const auto& s : kAccMul) ok += PointAt(s, &kOne);
        for (const auto& s : kAccAdd) ok += PointAt(s, &kZero);
        Log("ai_difficulty_accuracy: off - AI missile accuracy bonus on Hard / Very Hard / Legendary removed (%d of 6 sites)", ok);
    }
    if (g_cfg.battle_rules_debug_log) {
        if (memcmp(g_base + RVA_HANDICAP, kHandicapHead, sizeof(kHandicapHead)) != 0) { Log("battle_rules: handicap function differs; no diagnostics"); return; }
        o_Handicap = reinterpret_cast<HandicapFn>(InstallDetour(g_base + RVA_HANDICAP, reinterpret_cast<void*>(my_Handicap), sizeof(kHandicapHead)));
        Log("battle_rules: diagnostics %s", o_Handicap ? "on" : "FAILED");
    }
}

// ---------------------------------------------------------------- 2. projectile firing sound
constexpr uint32_t RVA_FIRE_QUERY = 0x00CC71B0;
const uint8_t kFireQueryHead[] = { 0x83, 0xEC, 0x3C, 0x8B, 0x44, 0x24, 0x40 };   // sub esp,0x3c; mov eax,[esp+0x40]
constexpr uint32_t VA_INVALID_ID = 0x117ADCFC;   // the engine's "no sound" id
typedef int (__fastcall *FireQueryFn)(void* bank, void* edx, uint8_t* proj, int dist, int mode, int unit);
typedef float (__fastcall *WeightFn)(void* filter, void* edx);
FireQueryFn o_FireQuery = nullptr;
const int32_t* g_invalidId = nullptr;

struct KeyFilter { void** vt; uint32_t pad[3]; uint32_t count; const int32_t* list; };
enum Match { M_ANY, M_YES, M_NO };
inline Match FilterMatch(const uint8_t* entry, int off, int32_t v) {
    const KeyFilter* f = reinterpret_cast<const KeyFilter*>(entry + off);
    if (f->count == 0) return M_ANY;
    for (uint32_t i = 0; i < f->count; i++) if (f->list[i] == v) return M_YES;
    return M_NO;
}
inline float FilterScore(const uint8_t* entry, int off, int32_t v) {
    Match m = FilterMatch(entry, off, v);
    if (m == M_NO) return 0.0f;
    void* f = const_cast<uint8_t*>(entry + off);
    float w = reinterpret_cast<WeightFn>((*reinterpret_cast<void***>(f))[3])(f, nullptr);
    return m == M_YES ? w * 10.0f : w;
}

volatile LONG g_fireLogged = 0;
int __fastcall my_FireQuery(void* bank, void*, uint8_t* proj, int dist, int mode, int unit) {
    const uint8_t* b = static_cast<const uint8_t*>(bank);
    uint32_t n = *reinterpret_cast<const uint32_t*>(b + 8);
    const uint8_t* const* entries = *reinterpret_cast<const uint8_t* const* const*>(b + 0xC);
    int32_t gun = *reinterpret_cast<const int32_t*>(proj + 0x14);
    int32_t shot = *reinterpret_cast<const int32_t*>(*reinterpret_cast<uint8_t* const*>(proj + 0x24) + 0x10);
    int32_t none = *g_invalidId;
    int bestAll = none, bestOk = none; float scoreAll = -1.0f, scoreOk = -1.0f;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t* e = entries[i];
        float s = FilterScore(e, 0x14, gun) + FilterScore(e, 0x2C, shot) + FilterScore(e, 0x44, dist) + FilterScore(e, 0x5C, mode);
        if (unit) s += FilterScore(e, 0x74, unit);
        int id = *reinterpret_cast<const int32_t*>(e + 0x10);
        if (s > scoreAll) { scoreAll = s; bestAll = id; }
        if (FilterMatch(e, 0x14, gun) != M_NO && FilterMatch(e, 0x2C, shot) != M_NO && s > scoreOk) { scoreOk = s; bestOk = id; }
    }
    int pick = bestOk != none ? bestOk : bestAll;
    if (g_cfg.battle_rules_debug_log && pick != bestAll && InterlockedIncrement(&g_fireLogged) <= 40)
        Log("projectile_sound: gun_type %d shot_type %d unit %08X - sound %d (score %.2f) instead of the game's %d (score %.2f)",
            gun, shot, static_cast<uint32_t>(unit), pick, scoreOk, bestAll, scoreAll);
    return pick;
}

void InstallFireSound() {
    if (!g_cfg.projectile_sound_priority) { Log("projectile_sound_priority: off (vanilla firing sound selection)"); return; }
    if (memcmp(g_base + RVA_FIRE_QUERY, kFireQueryHead, sizeof(kFireQueryHead)) != 0) { Log("projectile_sound_priority: function bytes differ; NOT applied"); return; }
    g_invalidId = reinterpret_cast<const int32_t*>(g_base + (VA_INVALID_ID - kPreferred));
    o_FireQuery = reinterpret_cast<FireQueryFn>(InstallDetour(g_base + RVA_FIRE_QUERY, reinterpret_cast<void*>(my_FireQuery), sizeof(kFireQueryHead)));
    Log("projectile_sound_priority: %s - firing sounds are chosen by weapon and shot type before unit", o_FireQuery ? "on" : "FAILED");
}

// ---------------------------------------------------------------- 3. fear from effects
constexpr uint32_t RVA_FEAR = 0x00152B27;
const uint8_t kFearOld[] = {   // mov ecx,esi; call kv getter; lea ecx,[eax+0x192c]; call value; mov eax,[eax]; neg eax
    0x8B, 0xCE, 0xE8, 0x32, 0xEB, 0xA7, 0x00, 0x8D, 0x88, 0x2C, 0x19, 0x00, 0x00, 0xE8, 0x67, 0xB8, 0x06, 0x00,
    0x8B, 0x00, 0xF7, 0xD8, 0x89, 0x84, 0x24, 0x9C, 0x00, 0x00, 0x00 };   // then mov [esp+0x9c],eax (kept)
const uint8_t kFearNew[22] = {   // eax = scares_men total: test eax,eax; jg +2; xor eax,eax; nops
    0x85, 0xC0, 0x7F, 0x02, 0x31, 0xC0, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
    0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };

void InstallFear() {
    if (!g_cfg.fear_stacking) { Log("fear_stacking: off (vanilla: any fear from effects is a flat 4, opposite signs cancel)"); return; }
    if (memcmp(g_base + RVA_FEAR, kFearOld, sizeof(kFearOld)) != 0) { Log("fear_stacking: bytes differ; NOT applied"); return; }
    bool ok = WriteCode(g_base + RVA_FEAR, kFearNew, sizeof(kFearNew));
    Log("fear_stacking: %s - a unit's fear from traits, items, skills and bundles is their total", ok ? "on" : "WRITE FAILED");
}

// ---------------------------------------------------------------- diagnostics: battle AI army states
// (battle_rules_debug_log=1) The battle AI runs each army group through a state object; vtables at
// 0x115EB08C..0x115EB2CC, slot 0 builds the state's plans (FUN_104d15d0 for 0x115EB290 builds an attack
// plan, FUN_101ca1c0, when its list at +0x1C (count +0x28) is filled, and the naval defence plan
// FUN_10546690 - AI_TACTIC_DEFEND_DEPLOYMENT_NAVAL - when its list at +0x08 (count +0x14) is filled).
// To find out which state the defending fleet's ships get in a naval battle, slot 0 of each state
// vtable is wrapped and every call is logged (first 200).
constexpr uint32_t kStateVt[] = { 0x115EB08C, 0x115EB0BC, 0x115EB0EC, 0x115EB11C, 0x115EB14C, 0x115EB17C,
                                  0x115EB1B8, 0x115EB1EC, 0x115EB220, 0x115EB254, 0x115EB290, 0x115EB2CC };
constexpr int kStateCount = (int)(sizeof(kStateVt) / sizeof(kStateVt[0]));
typedef void (__fastcall *StateFn)(void*);
StateFn g_stateOrig[kStateCount] = {};
volatile LONG g_stateLogged = 0;

bool WriteData(void* address, const void* bytes, size_t len) {
    DWORD old = 0;
    if (!VirtualProtect(address, len, PAGE_READWRITE, &old)) return false;
    memcpy(address, bytes, len);
    DWORD tmp = 0; VirtualProtect(address, len, old, &tmp);
    return true;
}
uint32_t VtRva(const void* obj) {
    if (!obj) return 0;
    uintptr_t vt = *static_cast<const uintptr_t*>(obj);
    return (vt > reinterpret_cast<uintptr_t>(g_base)) ? static_cast<uint32_t>(vt - reinterpret_cast<uintptr_t>(g_base)) : 0;
}
template <int I> void __fastcall StateEnter(void* self) {
    if (InterlockedIncrement(&g_stateLogged) <= 200) {
        const uint8_t* s = static_cast<const uint8_t*>(self);
        uint32_t n1 = *reinterpret_cast<const uint32_t*>(s + 0x14), n2 = *reinterpret_cast<const uint32_t*>(s + 0x28);
        void* const* l1 = *reinterpret_cast<void* const* const*>(s + 0x18);
        void* const* l2 = *reinterpret_cast<void* const* const*>(s + 0x2C);
        bool listed = I >= 5;   // the states derived from 0x115EB17C carry the two unit lists
        Log("battle_ai: state vt 0x%08X slot0 self=%p owner=%08X group=%08X lists %u (first vt %08X) / %u (first vt %08X) +0x64=%08X",
            kStateVt[I] - 0x10000000, self, *reinterpret_cast<const uint32_t*>(s + 4), *reinterpret_cast<const uint32_t*>(s + 0x30),
            listed ? n1 : 0, (listed && n1 && l1) ? VtRva(l1[0]) : 0, listed ? n2 : 0, (listed && n2 && l2) ? VtRva(l2[0]) : 0,
            listed ? *reinterpret_cast<const uint32_t*>(s + 0x64) : 0);
    }
    g_stateOrig[I](self);
}
template <int... Is> struct Seq {};
template <int N, int... Is> struct MakeSeq : MakeSeq<N - 1, N - 1, Is...> {};
template <int... Is> struct MakeSeq<0, Is...> { typedef Seq<Is...> type; };
template <int... Is> void FillEnter(StateFn* out, Seq<Is...>) { StateFn t[] = { &StateEnter<Is>... }; for (int i = 0; i < (int)sizeof...(Is); i++) out[i] = t[i]; }

void InstallStateDiag() {
    StateFn enter[kStateCount]; FillEnter(enter, MakeSeq<kStateCount>::type());
    int ok = 0;
    for (int i = 0; i < kStateCount; i++) {
        uintptr_t* slot = reinterpret_cast<uintptr_t*>(g_base + (kStateVt[i] - kPreferred));
        uintptr_t fn = *slot;
        if (fn < reinterpret_cast<uintptr_t>(g_base) || fn >= reinterpret_cast<uintptr_t>(g_base) + g_engine.sizeOfImage) continue;
        g_stateOrig[i] = reinterpret_cast<StateFn>(fn);
        uintptr_t mine = reinterpret_cast<uintptr_t>(enter[i]);
        if (WriteData(slot, &mine, sizeof(mine))) ok++;
    }
    Log("battle_ai: diagnostics on (%d of %d AI army-state vtables wrapped)", ok, kStateCount);
}

} // namespace

void PatchBattleRules_Install() {
    if (!g_engine.knownBuild && !g_cfg.allow_unknown_build) { Log("battle_rules: skipped, engine build not verified"); return; }
    g_base = reinterpret_cast<uint8_t*>(g_engine.base);
    InstallAccuracy();
    InstallFireSound();
    InstallFear();
    if (g_cfg.battle_rules_debug_log) InstallStateDiag();
}
