// gamemem.cpp -- guarded access to game memory (see gamemem.h).
//
// From ShakuEight/Shogun2-fix (proxy/gamemem.cpp), Copyright 2026 ShakuEight, Apache License 2.0
// (third_party/LICENSE-APACHE-2.0). Changed for this project: the original guards every access with
// SEH (__try / __except), which mingw-w64 does not offer on 32-bit targets. Here the guarded copies go
// through ReadProcessMemory / WriteProcessMemory on the current process: the kernel does the copy and
// reports a fault as a failed call instead of raising it, which gives the same contract (a stale
// pointer costs a failed read, never a crash). The range checks are the original ones.
#include "gamemem.h"
#include <cstring>

static uint32_t g_userMax = 0x7FFEFFFF;

void Mem_Init() {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    g_userMax = (uint32_t)(uintptr_t)si.lpMaximumApplicationAddress;
}

uint32_t Mem_UserMax() { return g_userMax; }

bool PtrPlausible(uint32_t p) {
    return p >= 0x00010000 && p <= g_userMax && (p & 3) == 0;
}

bool PtrInUser(uint32_t p) {
    return p >= 0x00010000 && p <= g_userMax;
}

static bool GuardedRead(void* out, uint32_t addr, uint32_t n) {
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)addr, out, n, &got)) return false;
    return got == n;
}

static bool GuardedWrite(uint32_t addr, const void* in, uint32_t n) {
    SIZE_T put = 0;
    if (!WriteProcessMemory(GetCurrentProcess(), (LPVOID)(uintptr_t)addr, in, n, &put)) return false;
    return put == n;
}

bool MemcpyTry(void* out, uint32_t addr, uint32_t n) {
    if (addr < 0x00010000 || addr > g_userMax) return false;
    return GuardedRead(out, addr, n);
}

bool ReadDword(uint32_t addr, uint32_t* out) {
    if (GuardedRead(out, addr, 4)) return true;
    *out = 0;
    return false;
}

bool ReadByte(uint32_t addr, uint8_t* out) {
    if (GuardedRead(out, addr, 1)) return true;
    *out = 0;
    return false;
}

// A swallowed guard-page fault would break that page's purpose, so dumps of a whole range check
// every page first.
bool RangeReadable(uint32_t addr, uint32_t n) {
    if (addr < 0x10000) return false;
    uint32_t end = addr + n;
    if (end < addr) return false;
    uint32_t a = addr;
    while (a < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((const void*)(uintptr_t)a, &mbi, sizeof(mbi)) == 0) return false;
        if (mbi.State != MEM_COMMIT) return false;
        DWORD p = mbi.Protect;
        if (p & (PAGE_GUARD | PAGE_NOACCESS)) return false;
        if (!(p & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                   PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return false;
        uint32_t regionEnd = (uint32_t)(uintptr_t)mbi.BaseAddress + (uint32_t)mbi.RegionSize;
        if (regionEnd <= a) return false;
        a = regionEnd;
    }
    return true;
}

uint32_t CopyGuarded(uint32_t addr, uint8_t* out, uint32_t n) {
    if (!RangeReadable(addr, n)) return 0;
    return GuardedRead(out, addr, n) ? n : 0;
}

bool WriteByteTry(uint32_t addr, uint8_t v) {
    if (addr < 0x00010000 || addr > g_userMax) return false;
    return GuardedWrite(addr, &v, 1);
}

bool WriteFloatTry(uint32_t addr, float v) {
    if (addr < 0x00010000 || addr > g_userMax) return false;
    return GuardedWrite(addr, &v, 4);
}

bool WriteDwordTry(uint32_t addr, uint32_t v) {
    if (addr < 0x00010000 || addr > g_userMax) return false;
    return GuardedWrite(addr, &v, 4);
}

uint32_t GameTickAt(uint32_t base) {
    uint32_t shell, mode, owner, tobj, tick;
    if (!base) return 0;
    if (!MemcpyTry(&shell, base + 0x017C755C, 4)) return 0;
    if (!MemcpyTry(&mode, shell + 0x50, 4)) return 0;
    if (!MemcpyTry(&owner, mode + 0x00, 4)) return 0;
    if (!MemcpyTry(&tobj, owner + 0x1C, 4)) return 0;
    if (!MemcpyTry(&tick, tobj + 0x50, 4)) return 0;
    return tick;
}
