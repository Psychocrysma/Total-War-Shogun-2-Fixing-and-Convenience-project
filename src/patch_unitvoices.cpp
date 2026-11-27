// European voices for listed units (fots_voice_units in the [voices] section of tws2fix.ini).
//
// The Sengoku campaign's Portuguese Tercos speak with Fall of the Samurai's British voice set because
// the engine names their unit key in three places. This patch gives the same treatment to every unit
// key in a list, so a mod's European units (Expanded Japan's Portugal add-on) can use that voice set
// without "Boshin" in their keys. The mod supplies the voice files and the unit's voice settings
// (units table: VO_actor_numbers 24,25, num_audio_VO_variations 2, audio_language UK).
//
// What the engine does (empire.retail.dll, May 2023 build, preferred base 0x10000000):
//   1. Unit record, FUN_10c0c9c0: FUN_10c1abb0(key) gives the voice nationality at record+0xD4
//      (0 British, 1 American, 2 French, 3 Japanese, by substrings like "royal_marines"); at
//      0x10C0CCE6 a record still at 3 whose key is Inf_Missile_Portuguese_Tercos(_MP) is set to 0.
//      Hook there: a listed key is set to 0 as well. Records load with the database, after this
//      DLL has installed its patches.
//   2. Battle land unit, FUN_10129db0: byte unit+0xFC8 = key starts with "Boshin" or is the Tercos
//      (0x1012A5E1..0x1012A61C; the key is the string at unit+0x20, in EDI). Hook at 0x1012A618,
//      just before the store: AL = 1 for a listed key.
//   3. Battle ship, FUN_1035ae20: byte ship+0x16D0 = key starts with "Boshin" (0x1035C3F1..0x1035C40C;
//      the key is the string at ship+0x218, in ESI). Hook at 0x1035C405: AL = 1 for a listed key.
//   Every other reader of the two bytes picks voice lines (FUN_102ded50, FUN_103050a0, FUN_103052d0,
//   FUN_10337cc0) except the one below.
//   4. Battle audio set-up, FUN_10291ae0: a land unit with +0xFC8 or a ship with +0x16D0 in the
//      player's alliance sets byte this+0x358, which makes FUN_10301120 prefix a battle sound event
//      name with "bosh_" (the Fall of the Samurai set). The engine skips the Tercos here by key
//      (0x10291C1B..0x10291C48) so a Sengoku battle keeps its own sounds. The listed keys are skipped
//      the same way: at the two flag tests (0x10291C12 land, 0x10291B99 ship) a listed unit is
//      swapped for a zeroed dummy, so the test reads 0. Neither register is used again on that path.
//
// Not touched: unit_stats_land's +0x279 flag (FUN_10c096d0, cleared for keys containing "Boshin"),
// which decides how the unit's models are drawn; the listed units keep their own keys, so it stays
// as it is for them.
//
// The British events exist for infantry and ships only; cavalry, generals and artillery in the list
// would probably fall silent, so leave them out.
#include "common.h"
#include "shaku/hook.h"
#include "shaku/gamemem.h"
#include <cstring>
#include <vector>
#include <string>

namespace {

enum { H_NATION = 0, H_LAND, H_SHIP, H_AUDIO_LAND, H_AUDIO_SHIP, HOOK_COUNT };
const HookDef kHooks[HOOK_COUNT] = {
    { "FUN_10c0c9c0+0x326", "unit record: voice nationality", 0x00C0CCE6, 7,  { 0x83, 0xBE, 0xD4, 0x00, 0x00, 0x00, 0x03 }, -1, false },
    { "FUN_10129db0+0x868", "land unit: FotS voice flag",     0x0012A618, 10, { 0x8D, 0x4C, 0x24, 0x28, 0x88, 0x85, 0xC8, 0x0F, 0x00, 0x00 }, -1, false },
    { "FUN_1035ae20+0x15e5", "ship: FotS voice flag",         0x0035C405, 7,  { 0x8D, 0x8C, 0x24, 0x04, 0x01, 0x00, 0x00 }, -1, false },
    { "FUN_10291ae0+0x132", "battle audio: land flag test",   0x00291C12, 7,  { 0x80, 0xB9, 0xC8, 0x0F, 0x00, 0x00, 0x00 }, -1, false },
    { "FUN_10291ae0+0xb9",  "battle audio: ship flag test",   0x00291B99, 7,  { 0x80, 0xB8, 0xD0, 0x16, 0x00, 0x00, 0x00 }, -1, false },
};
constexpr int CTX_EDI = 0x04, CTX_ESI = 0x08, CTX_ECX = 0x1C, CTX_EAX = 0x20;

struct EngineString { uint32_t len; uint32_t cap; const wchar_t* text; };   // the engine's wide string

uint8_t* g_base = nullptr;
HookSlot g_slots[HOOK_COUNT];
std::vector<std::wstring> g_keys;                 // lower case
uint8_t g_zero[0x1800];                           // dummy unit / ship whose flag bytes read 0
volatile LONG g_logged = 0;

inline uint32_t R32(const uint8_t* ctx, int off) { uint32_t v; memcpy(&v, ctx + off, 4); return v; }
inline void W32(const uint8_t* ctx, int off, uint32_t v) { memcpy(const_cast<uint8_t*>(ctx) + off, &v, 4); }

bool Listed(const void* str) {
    if (!str) return false;
    const EngineString* s = static_cast<const EngineString*>(str);
    if (s->len == 0 || s->len > 200 || !s->text) return false;
    for (const std::wstring& k : g_keys) {
        if (k.size() != s->len) continue;
        uint32_t i = 0;
        for (; i < s->len; i++) {
            wchar_t c = s->text[i]; if (c >= L'A' && c <= L'Z') c += 32;
            if (c != k[i]) break;
        }
        if (i == s->len) return true;
    }
    return false;
}
void Note(const char* what, const void* str) {
    if (!g_cfg.fots_voice_debug_log || InterlockedIncrement(&g_logged) > 300) return;
    const EngineString* s = static_cast<const EngineString*>(str);
    char name[96] = {}; for (uint32_t i = 0; i < s->len && i < 95; i++) name[i] = s->text[i] < 128 ? static_cast<char>(s->text[i]) : '?';
    Log("unit_voices: %s %s", what, name);
}

extern "C" void __stdcall unitvoices_OnNation(const uint8_t* ctx) {      // ESI = unit record, key string at +0
    uint8_t* rec = reinterpret_cast<uint8_t*>(R32(ctx, CTX_ESI));
    if (!Listed(rec)) return;
    uint32_t was; memcpy(&was, rec + 0xD4, 4);
    uint32_t zero = 0; memcpy(rec + 0xD4, &zero, 4);
    if (g_cfg.fots_voice_debug_log) { char w[48]; snprintf(w, sizeof(w), "record: voice nationality %u -> 0 (British) for", was); Note(w, rec); }
}
extern "C" void __stdcall unitvoices_OnLand(const uint8_t* ctx) {        // EDI = unit key, AL = flag about to be stored
    const void* key = reinterpret_cast<const void*>(R32(ctx, CTX_EDI));
    if (!Listed(key)) return;
    W32(ctx, CTX_EAX, (R32(ctx, CTX_EAX) & 0xFFFFFF00u) | 1);
    Note("battle land unit uses the European voice set:", key);
}
extern "C" void __stdcall unitvoices_OnShip(const uint8_t* ctx) {        // ESI = ship's unit key, AL = flag about to be stored
    const void* key = reinterpret_cast<const void*>(R32(ctx, CTX_ESI));
    if (!Listed(key)) return;
    W32(ctx, CTX_EAX, (R32(ctx, CTX_EAX) & 0xFFFFFF00u) | 1);
    Note("battle ship uses the European voice set:", key);
}
extern "C" void __stdcall unitvoices_OnAudioLand(const uint8_t* ctx) {   // ECX = land unit (key at +0x20)
    const uint8_t* unit = reinterpret_cast<const uint8_t*>(R32(ctx, CTX_ECX));
    if (unit && unit[0xFC8] && Listed(unit + 0x20)) W32(ctx, CTX_ECX, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_zero)));
}
extern "C" void __stdcall unitvoices_OnAudioShip(const uint8_t* ctx) {   // EAX = ship (key at +0x218)
    const uint8_t* ship = reinterpret_cast<const uint8_t*>(R32(ctx, CTX_EAX));
    if (ship && ship[0x16D0] && Listed(ship + 0x218)) W32(ctx, CTX_EAX, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_zero)));
}

extern "C" uint8_t* unitvoices_Trampolines[HOOK_COUNT];
uint8_t* unitvoices_Trampolines[HOOK_COUNT] = {};
HOOK_DETOUR(unitvoices_D0, unitvoices_OnNation,    unitvoices_Trampolines, 0)
HOOK_DETOUR(unitvoices_D1, unitvoices_OnLand,      unitvoices_Trampolines, 1)
HOOK_DETOUR(unitvoices_D2, unitvoices_OnShip,      unitvoices_Trampolines, 2)
HOOK_DETOUR(unitvoices_D3, unitvoices_OnAudioLand, unitvoices_Trampolines, 3)
HOOK_DETOUR(unitvoices_D4, unitvoices_OnAudioShip, unitvoices_Trampolines, 4)
const HookDetour g_detours[HOOK_COUNT] = { unitvoices_D0, unitvoices_D1, unitvoices_D2, unitvoices_D3, unitvoices_D4 };

void ParseKeys(const std::wstring& list) {
    std::wstring cur;
    auto flush = [&] { if (!cur.empty()) { g_keys.push_back(cur); cur.clear(); } };
    for (wchar_t c : list) {
        if (c == L',' || c == L';' || c == L' ' || c == L'\t' || c == L'\r' || c == L'\n') { flush(); continue; }
        if (c >= L'A' && c <= L'Z') c += 32;
        cur.push_back(c);
    }
    flush();
}

} // namespace

void PatchUnitVoices_Install() {
    ParseKeys(g_cfg.fots_voice_units);
    if (g_keys.empty()) { Log("unit_voices: off (fots_voice_units is empty)"); return; }
    if (!g_engine.knownBuild) { Log("unit_voices: skipped, engine build not verified"); return; }
    Mem_Init();
    g_base = reinterpret_cast<uint8_t*>(g_engine.base);
    for (int h = 0; h < HOOK_COUNT; h++)
        if (!Hook_SiteMatches(g_base, kHooks[h])) { Log("unit_voices: bytes at RVA 0x%06X (%s) differ; NOT installed", kHooks[h].rva, kHooks[h].role); return; }
    if (!Hook_Prepare(g_base, kHooks, HOOK_COUNT, nullptr, unitvoices_Trampolines, g_slots)) { Log("unit_voices: trampoline memory; NOT installed"); return; }
    Hook_Patch(g_detours, g_slots, HOOK_COUNT);
    Log("unit_voices: ON - %d unit keys use the European (Fall of the Samurai British) voice set, like the Portuguese Tercos", static_cast<int>(g_keys.size()));
}

void PatchUnitVoices_Remove() {
    if (!g_base) return;
    Hook_Remove(g_slots, HOOK_COUNT);
}
