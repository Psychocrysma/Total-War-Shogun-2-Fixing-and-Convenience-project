// hook.cpp -- shared hook engine (see hook.h).
//
// From ShakuEight/Shogun2-fix (proxy/hook.cpp), Copyright 2026 ShakuEight, Apache License 2.0
// (third_party/LICENSE-APACHE-2.0). Unchanged apart from this notice.

#include "hook.h"
#include <cstring>

// Trampoline memory: executable pages shared by all fixes, handed out in 16-byte aligned pieces.
// Fixes install one after another on the thread of the game's first DirectInput8Create call,
// so the pool needs no lock.
static const size_t POOL_PAGE = 4096;
static uint8_t* g_pool = nullptr;
static size_t g_poolUsed = 0;

static uint8_t* PoolAlloc(size_t size) {
    size = (size + 15) & ~(size_t)15;
    if (size > POOL_PAGE) return nullptr;
    if (!g_pool || g_poolUsed + size > POOL_PAGE) {
        // a new page; the old one stays in use by the trampolines already in it
        uint8_t* page = (uint8_t*)VirtualAlloc(nullptr, POOL_PAGE, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!page) return nullptr;
        g_pool = page;
        g_poolUsed = 0;
    }
    uint8_t* r = g_pool + g_poolUsed;
    g_poolUsed += size;
    return r;
}

uint8_t* Hook_GameModule(bool requireSize, uint32_t* sizeOut) {
    HMODULE hMod = GetModuleHandleA("empire.retail.dll");
    if (!hMod) return nullptr;
    uint8_t* base = (uint8_t*)hMod;
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
    const IMAGE_NT_HEADERS32* nt = (const IMAGE_NT_HEADERS32*)(base + dos->e_lfanew);
    uint32_t size = nt->OptionalHeader.SizeOfImage;
    if (sizeOut) *sizeOut = size;
    if (requireSize && size != HOOK_ANALYSED_IMAGE_SIZE) return nullptr;
    return base;
}

uint32_t Hook_RebaseDelta(const uint8_t* base) {
    return (uint32_t)(uintptr_t)base - HOOK_ANALYSED_IMAGE_BASE;
}

void Hook_Expected(const HookDef& d, uint32_t delta, uint8_t* out) {
    memcpy(out, d.expected, d.len);
    if (d.relocOff >= 0) {
        uint32_t abs;
        memcpy(&abs, out + d.relocOff, 4);
        abs += delta;
        memcpy(out + d.relocOff, &abs, 4);
    }
}

bool Hook_SiteMatches(const uint8_t* base, const HookDef& d) {
    if (d.len < 5 || d.len > HOOK_MAX_STOLEN) return false;
    uint8_t expect[HOOK_MAX_STOLEN];
    Hook_Expected(d, Hook_RebaseDelta(base), expect);
    return memcmp(base + d.rva, expect, d.len) == 0;
}

bool Hook_Prepare(uint8_t* base, const HookDef* defs, int n, const bool* use, uint8_t** tramps,
                  HookSlot* slots) {
    for (int h = 0; h < n; h++) {
        if (use && !use[h]) continue;
        const HookDef& d = defs[h];
        uint8_t* target = base + d.rva;
        uint8_t* tramp = PoolAlloc((size_t)d.len + 5);
        if (!tramp) return false;
        memcpy(tramp, target, d.len);
        tramp[d.len] = 0xE9;
        int32_t back = (int32_t)((target + d.len) - (tramp + d.len + 5));
        memcpy(tramp + d.len + 1, &back, 4);
        FlushInstructionCache(GetCurrentProcess(), tramp, (SIZE_T)d.len + 5);
        tramps[h] = tramp;
        memcpy(slots[h].original, target, d.len);
        slots[h].target = target;
        slots[h].len = d.len;
        slots[h].active = false;
    }
    return true;
}

void Hook_Patch(const HookDetour* detours, HookSlot* slots, int n) {
    for (int h = 0; h < n; h++) {
        uint8_t* target = slots[h].target;
        if (!target || slots[h].active) continue;
        int len = slots[h].len;
        uint8_t patch[HOOK_MAX_STOLEN];
        patch[0] = 0xE9;
        int32_t rel = (int32_t)((uint8_t*)detours[h] - target - 5);
        memcpy(patch + 1, &rel, 4);
        memset(patch + 5, 0x90, len - 5);
        DWORD old;
        VirtualProtect(target, len, PAGE_EXECUTE_READWRITE, &old);
        memcpy(target, patch, len);
        VirtualProtect(target, len, old, &old);
        FlushInstructionCache(GetCurrentProcess(), target, len);
        slots[h].active = true;
    }
}

void Hook_Remove(HookSlot* slots, int n) {
    for (int h = 0; h < n; h++) {
        if (!slots[h].active) continue;
        DWORD old;
        VirtualProtect(slots[h].target, slots[h].len, PAGE_EXECUTE_READWRITE, &old);
        memcpy(slots[h].target, slots[h].original, slots[h].len);
        VirtualProtect(slots[h].target, slots[h].len, old, &old);
        FlushInstructionCache(GetCurrentProcess(), slots[h].target, slots[h].len);
        slots[h].active = false;
    }
}

bool Hook_WriteCode(uint8_t* at, const void* bytes, int n) {
    DWORD old;
    if (!VirtualProtect(at, (SIZE_T)n, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(at, bytes, (size_t)n);
    VirtualProtect(at, (SIZE_T)n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, (SIZE_T)n);
    return true;
}
