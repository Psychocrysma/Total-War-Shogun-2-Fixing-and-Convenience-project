// Clan selection: a clan button starts the wrong clan when another clan's key contains its key.
//
// The campaign selection screen finds the clan of a clicked button with FUN_1101a930 (thiscall, one
// string argument by value, ret 0xC; callers 0x10fc4dde on the selection screen, 0x11026387 and
// 0x11026ab3). It takes the button's name after its first '_' ("button_nasu" -> "nasu") and walks the
// campaign's faction list (this+0xA8: count at +0x2C, 0xB4-byte records at +0x30, the key string first
// in each record, in alphabetical key order) with a "contains" test (FUN_100f0350, a substring search),
// returning the first record whose key CONTAINS the text. A clan whose key sorts after a longer key that
// contains it is never reached: with Expanded Japan's Hokkaido pack, "menasunkur" contains "nasu", so
// Nasu's button started a Menasunkur campaign. (CA's own and Expanded Japan's other clans only got away
// with it because each short key happens to sort before the longer keys containing it: aki / akita,
// sugi / uesugi, hatakeyama / hatakeyama_mutsu.)
//
// The button name comes in as the engine's narrow (UTF-8) string { uint32 length in bytes, uint32
// capacity, char* text }: the function copies it into a wide string with FUN_100ed2d0 (FUN_100dbc60
// counts the UTF-8 characters, FUN_100dbb50 decodes them) before splitting it. The faction keys are wide
// strings { uint32 length, uint32 capacity, wchar_t* text }.
//
// The fix wraps FUN_1101a930: the game function runs as before (it also frees its by-value argument),
// then, when a faction's key is exactly the button's text after its first '_', that faction is returned
// instead. Without an exact match the game's own answer stands, so buttons that relied on the substring
// search still work.
//
// Checked under Wine against the real engine with a test clan list (test/clan_select_check.cpp):
// unpatched, button_nasu returns menasunkur; patched, every button returns its own clan.
// Address and bytes are for empire.retail.dll of the May 2023 build; the entry bytes are checked first.
#include "common.h"
#include <cstring>

namespace {

constexpr uint32_t RVA_FIND_FACTION = 0x0101A930;   // FUN_1101a930
const uint8_t kHead[7] = { 0x83, 0xEC, 0x18, 0x8D, 0x44, 0x24, 0x1C };   // sub esp,0x18; lea eax,[esp+0x1c]

struct EngineString { uint32_t len; uint32_t cap; const wchar_t* text; };   // wide (the faction keys)

typedef void* (__fastcall* FindFactionFn)(void* self, void* edx, uint32_t len, uint32_t cap, const char* text);
FindFactionFn o_FindFaction = nullptr;
volatile LONG g_fixed = 0;

bool Readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || !VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return reinterpret_cast<const uint8_t*>(p) + n <= reinterpret_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
}

// key = the UTF-8 text after its first '_' as UTF-16, as the game function makes it; false if unusable
bool ButtonKey(const char* text, wchar_t* key, int keyCap, int* keyLen) {
    if (!text || !Readable(text, 1)) return false;
    int n = 0;
    while (n < 255 && Readable(text + n, 1) && text[n]) n++;
    if (n == 0 || n >= 255) return false;
    wchar_t wide[256];
    int w = MultiByteToWideChar(CP_UTF8, 0, text, n, wide, 255);
    if (w <= 0) return false;
    int start = 0;
    for (int i = 0; i < w; i++) if (wide[i] == L'_') { start = i + 1; break; }
    int k = w - start;
    if (k <= 0 || k > keyCap) return false;
    memcpy(key, wide + start, k * sizeof(wchar_t)); *keyLen = k;
    return true;
}

void* ExactMatch(void* self, const char* text) {
    wchar_t key[256]; int klen = 0;
    if (!ButtonKey(text, key, 255, &klen)) return nullptr;
    uint8_t* list = nullptr;
    if (!Readable(static_cast<uint8_t*>(self) + 0xA8, 4)) return nullptr;
    memcpy(&list, static_cast<uint8_t*>(self) + 0xA8, 4);
    if (!list || !Readable(list + 0x2C, 8)) return nullptr;
    uint32_t count = 0; uint8_t* recs = nullptr;
    memcpy(&count, list + 0x2C, 4); memcpy(&recs, list + 0x30, 4);
    if (!recs || count == 0 || count > 4096 || !Readable(recs, count * 0xB4)) return nullptr;
    for (uint32_t i = 0; i < count; i++) {
        const EngineString* s = reinterpret_cast<const EngineString*>(recs + i * 0xB4);
        if (s->len != static_cast<uint32_t>(klen) || !s->text || !Readable(s->text, klen * 2)) continue;
        if (memcmp(s->text, key, klen * 2) == 0) return recs + i * 0xB4;
    }
    return nullptr;
}

void* __fastcall my_FindFaction(void* self, void* edx, uint32_t len, uint32_t cap, const char* text) {
    void* exact = ExactMatch(self, text);           // read the argument before the game function frees it
    char name[96] = {};                             // copied now too, for the log line
    if (exact) for (int i = 0; i < 95 && text[i]; i++) name[i] = text[i];
    void* r = o_FindFaction(self, edx, len, cap, text);
    if (exact && exact != r) {
        if (InterlockedIncrement(&g_fixed) <= 20)
            Log("clan_select_exact: %s matched a longer key first; the clan whose key is exactly the button's is used", name);
        return exact;
    }
    return r;
}

} // namespace

void PatchClanSelect_Install() {
    if (!g_cfg.clan_select_exact) { Log("clan_select_exact: off"); return; }
    if (!g_engine.knownBuild && !g_cfg.allow_unknown_build) { Log("clan_select_exact: skipped, engine build not verified"); return; }
    uint8_t* at = reinterpret_cast<uint8_t*>(g_engine.base) + RVA_FIND_FACTION;
    if (memcmp(at, kHead, sizeof(kHead)) != 0) { Log("clan_select_exact: bytes at RVA 0x%08X differ; patch NOT applied", RVA_FIND_FACTION); return; }
    o_FindFaction = reinterpret_cast<FindFactionFn>(InstallDetour(at, reinterpret_cast<void*>(my_FindFaction), sizeof(kHead)));
    Log("clan_select_exact: %s - a clan button picks the clan whose key is exactly its own before any key that only contains it",
        o_FindFaction ? "ON" : "FAILED (detour)");
}
