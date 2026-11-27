// Runs the engine's own code around the five unit-voice hook sites (src/patch_unitvoices.cpp) on made-up
// units, vanilla and with the DLL. Each fragment is entered with chosen registers on a private stack and
// left through a jump written (in this test process only) just after the fragment.
// Build: i686-w64-mingw32-g++ -O1 -static -o unit_voices_check.exe unit_voices_check.cpp
// Run next to empire.retail.dll: "wine unit_voices_check.exe" (vanilla) or, with dinput8.dll + tws2fix.ini
// beside it, WINEDLLOVERRIDES="dinput8=n,b" wine unit_voices_check.exe fix
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <initializer_list>
struct EStr { uint32_t len, cap; wchar_t* text; };
typedef void (__fastcall *StrCtor)(EStr*, void*, const char*);
// The engine is mapped without its loader steps (several of its DLLs are not here): resolve the imports
// that exist, point the rest at a stub that reports itself, and give it its implicit TLS block.
static const char* g_missName[4096]; static int g_missCount = 0;
extern "C" void __cdecl MissingReport(int i) { printf("called missing import %s\n", g_missName[i]); fflush(stdout); ExitProcess(9); }
static void* MakeStub(const char* name) {
    static uint8_t* pool = nullptr; static int used = 0;
    if (!pool) pool = (uint8_t*)VirtualAlloc(nullptr, 65536, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    int i = g_missCount++; g_missName[i] = name;
    uint8_t* p = pool + used; used += 16;
    if (!strncmp(name, "steam_api.dll!", 14)) { p[0] = 0x31; p[1] = 0xC0; p[2] = 0xC3; return p; }   // cdecl: return 0
    p[0] = 0x68; *(int*)(p + 1) = i;                         // push i
    p[5] = 0xE8; *(int*)(p + 6) = (int)((uint8_t*)MissingReport - (p + 10));   // call MissingReport
    return p;
}
bool ResolveImports(uint8_t* b) {
    auto* nt = (IMAGE_NT_HEADERS*)(b + ((IMAGE_DOS_HEADER*)b)->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    int missing = 0;
    for (auto* d = (IMAGE_IMPORT_DESCRIPTOR*)(b + dir.VirtualAddress); d->Name; d++) {
        HMODULE m = LoadLibraryA((char*)b + d->Name);
        if (!m) missing++;
        auto* th = (IMAGE_THUNK_DATA32*)(b + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        auto* iat = (IMAGE_THUNK_DATA32*)(b + d->FirstThunk);
        for (; th->u1.AddressOfData; th++, iat++) {
            FARPROC f = nullptr;
            if (m) f = (th->u1.Ordinal & IMAGE_ORDINAL_FLAG32) ? GetProcAddress(m, (LPCSTR)(uintptr_t)(th->u1.Ordinal & 0xFFFF))
                                                            : GetProcAddress(m, ((IMAGE_IMPORT_BY_NAME*)(b + th->u1.AddressOfData))->Name);
            DWORD old; VirtualProtect(iat, 4, PAGE_READWRITE, &old);
            if (!f) { static char names[4096][64]; static int nn = 0; char* nm = names[nn++];
                if (th->u1.Ordinal & IMAGE_ORDINAL_FLAG32) snprintf(nm, 64, "%s#%u", (char*)b + d->Name, (unsigned)(th->u1.Ordinal & 0xFFFF));
                else snprintf(nm, 64, "%s!%s", (char*)b + d->Name, ((IMAGE_IMPORT_BY_NAME*)(b + th->u1.AddressOfData))->Name);
                f = (FARPROC)MakeStub(nm); }
            iat->u1.Function = (DWORD)(uintptr_t)f;
            VirtualProtect(iat, 4, old, &old);
        }
    }
    printf("imports resolved (%d DLLs missing, stubbed)\n", missing); fflush(stdout);
    return true;
}
bool SetupTls(uint8_t* b) {
    auto* nt = (IMAGE_NT_HEADERS*)(b + ((IMAGE_DOS_HEADER*)b)->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (!dir.VirtualAddress) { printf("no TLS\n"); return true; }
    auto* t = (IMAGE_TLS_DIRECTORY32*)(b + dir.VirtualAddress);
    uint32_t size = t->EndAddressOfRawData - t->StartAddressOfRawData + t->SizeOfZeroFill;
    uint8_t* blk = (uint8_t*)calloc(1, size + 16);
    memcpy(blk, (void*)(uintptr_t)t->StartAddressOfRawData, t->EndAddressOfRawData - t->StartAddressOfRawData);
    void** teb = (void**)NtCurrentTeb();
    void** oldArr = (void**)teb[0x2C / 4];
    void** arr = (void**)calloc(64, sizeof(void*));
    int idx = 0;
    if (oldArr) { while (idx < 32 && oldArr[idx]) { arr[idx] = oldArr[idx]; idx++; } }
    arr[idx] = blk;
    teb[0x2C / 4] = arr;
    DWORD old; uint32_t* pidx = (uint32_t*)(uintptr_t)t->AddressOfIndex;
    VirtualProtect(pidx, 4, PAGE_READWRITE, &old); *pidx = idx; VirtualProtect(pidx, 4, old, &old);
    printf("TLS block %u bytes at index %d\n", size, idx);
    return true;
}

// ---- fragment runner -------------------------------------------------------------------------
struct Regs { uint32_t eax, ecx, edx, ebx, ebp, esi, edi; };
extern "C" { uint32_t g_savedEsp; uint32_t g_entry; uint32_t g_newEsp; Regs g_regs; void RunFragment(); void FragmentExit(); }
__asm__(".text\n.intel_syntax noprefix\n"
        ".globl _RunFragment\n_RunFragment:\n"
        "  pushad\n  pushfd\n"
        "  mov dword ptr [_g_savedEsp], esp\n"
        "  mov eax, dword ptr [_g_regs]\n  mov ecx, dword ptr [_g_regs+4]\n  mov edx, dword ptr [_g_regs+8]\n"
        "  mov ebx, dword ptr [_g_regs+12]\n  mov ebp, dword ptr [_g_regs+16]\n  mov esi, dword ptr [_g_regs+20]\n"
        "  mov edi, dword ptr [_g_regs+24]\n"
        "  mov esp, dword ptr [_g_newEsp]\n"
        "  jmp dword ptr [_g_entry]\n"
        ".globl _FragmentExit\n_FragmentExit:\n"
        "  mov esp, dword ptr [_g_savedEsp]\n  popfd\n  popad\n  ret\n"
        ".att_syntax\n");
static uint8_t* g_stack;
static void ExitAt(uint8_t* at) {          // jmp FragmentExit, written over the instruction after the fragment
    DWORD old; VirtualProtect(at, 5, PAGE_EXECUTE_READWRITE, &old);
    at[0] = 0xE9; int rel = (int)((uint8_t*)FragmentExit - (at + 5)); memcpy(at + 1, &rel, 4);
    VirtualProtect(at, 5, old, &old); FlushInstructionCache(GetCurrentProcess(), at, 5);
}
static uint32_t* Run(uint8_t* entry, const Regs& r) {   // returns the fragment's stack pointer at entry
    memset(g_stack, 0, 0x4000);
    g_regs = r; g_entry = (uint32_t)(uintptr_t)entry; g_newEsp = (uint32_t)(uintptr_t)(g_stack + 0x2000);
    RunFragment();
    return (uint32_t*)(g_stack + 0x2000);
}
static int g_bad = 0;
static void Expect(const char* what, const char* key, int got, int want) {
    if (got != want) g_bad++;
    printf("  %-34s %-34s %d %s\n", what, key, got, got == want ? "ok" : "WRONG");
}
int main(int argc, char** argv) {
    bool withFix = argc > 1 && strcmp(argv[1], "fix") == 0;
    if (withFix) printf("dinput8 proxy: %p\n", LoadLibraryW(L"dinput8.dll"));
    HMODULE eng = LoadLibraryExW(L"empire.retail.dll", nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!eng) { printf("engine load failed %lu\n", GetLastError()); return 1; }
    if (!ResolveImports((uint8_t*)eng) || !SetupTls((uint8_t*)eng)) return 1;
    {   // run the engine's CRT start-up (static constructors), as the loader would have
        auto* nt = (IMAGE_NT_HEADERS*)((uint8_t*)eng + ((IMAGE_DOS_HEADER*)eng)->e_lfanew);
        typedef BOOL (WINAPI *DllEntry)(HINSTANCE, DWORD, LPVOID);
        DllEntry e = (DllEntry)((uint8_t*)eng + nt->OptionalHeader.AddressOfEntryPoint);
        printf("engine DllMain: %d\n", e((HINSTANCE)eng, DLL_PROCESS_ATTACH, nullptr)); fflush(stdout);
    }
    printf("engine at %p\n", eng);
    if (withFix) { GetProcAddress(eng, "entry_point"); Sleep(300); }
    uint8_t* b = (uint8_t*)eng;
    StrCtor mk = (StrCtor)(b + 0x000ED420);
    g_stack = (uint8_t*)VirtualAlloc(nullptr, 0x4000, MEM_COMMIT, PAGE_READWRITE);
    ExitAt(b + 0x0012A627); ExitAt(b + 0x0035C417); ExitAt(b + 0x00C0CD20); ExitAt(b + 0x00291C62); ExitAt(b + 0x00291BB9);
    // key, land flag, ship flag, record nationality (from 3), battle-audio flag set by a land unit / a ship
    struct Case { const char* key; int land, ship, nation, audioLand, audioShip; };
    const int F = withFix ? 1 : 0;
    const Case cases[] = {
        { "Inf_Heavy_Katana_Samurai",          0, 0, 3, 0, 0 },      // an ordinary unit: never changed
        { "Inf_Missile_Portuguese_Tercos",     1, 0, 0, 0, 0 },      // the engine's own special case
        { "Boshin_Inf_Line_Royal_Marines",     1, 1, 3, 1, 1 },      // a Fall of the Samurai key (3: nationality comes from FUN_10c1abb0, not run here)
        { "Port_Pikemen",                      F, F, F ? 0 : 3, 0, 0 },   // listed: like the Tercos with the fix
        { "Port_Galleon",                      F, F, F ? 0 : 3, 0, 0 },
        { "port_musketeers_garrison",          F, F, F ? 0 : 3, 0, 0 },   // the list ignores case
        { "Port_Lancer",                       0, 0, 3, 0, 0 },      // cavalry: not listed, stays Japanese
        { "Port_Pikemen_Extra",                0, 0, 3, 0, 0 },      // only whole keys match
    };
    printf("%s\n", withFix ? "with TWS2-Fixing-Project" : "vanilla engine");
    for (const Case& c : cases) {
        Regs r;
        // 1. unit record: cmp [esi+0xd4],3 ... (0x10C0CCE6 -> 0x10C0CD20); the key string is at record+0
        static uint8_t rec[0x200]; memset(rec, 0, sizeof(rec)); mk((EStr*)rec, nullptr, c.key); *(uint32_t*)(rec + 0xD4) = 3;
        r = {}; r.esi = (uint32_t)(uintptr_t)rec; Run(b + 0x00C0CCE6, r);
        Expect("record voice nationality", c.key, (int)*(uint32_t*)(rec + 0xD4), c.nation);
        // 2. land unit: push "Boshin" ... mov [ebp+0xfc8],al (0x1012A5D2 -> 0x1012A627); key at unit+0x20 in EDI
        static uint8_t unit[0x1800]; memset(unit, 0, sizeof(unit)); mk((EStr*)(unit + 0x20), nullptr, c.key); unit[0xFC8] = 0x55;
        r = {}; r.edi = (uint32_t)(uintptr_t)(unit + 0x20); r.ebp = (uint32_t)(uintptr_t)unit; Run(b + 0x0012A5D2, r);
        Expect("land unit European voice flag", c.key, unit[0xFC8], c.land);
        // 3. ship: push "Boshin" ... mov [edi+0x16d0],al (0x1035C3DF -> 0x1035C417); key at ship+0x218 in ESI
        static uint8_t ship[0x1800]; memset(ship, 0, sizeof(ship)); mk((EStr*)(ship + 0x218), nullptr, c.key); ship[0x16D0] = 0x55;
        r = {}; r.esi = (uint32_t)(uintptr_t)(ship + 0x218); r.edi = (uint32_t)(uintptr_t)ship; Run(b + 0x0035C3DF, r);
        Expect("ship European voice flag", c.key, ship[0x16D0], c.ship);
        // 4. battle audio set-up, land unit in the player's alliance (0x10291C12 -> 0x10291C62).
        //    The unit carries the flag the engine (or the fix) gave it above.
        static uint8_t self[0x400], alliance[0x40], army[0x200]; static uint32_t counter; static void* units[1];
        memset(self, 0, sizeof(self)); memset(alliance, 0, sizeof(alliance)); memset(army, 0, sizeof(army));
        units[0] = unit; *(void**)(army + 0x118) = units;
        memset(g_stack, 0, 0x4000);
        r = {}; r.ecx = (uint32_t)(uintptr_t)unit; r.esi = (uint32_t)(uintptr_t)army; r.edi = (uint32_t)(uintptr_t)self;
        r.edx = (uint32_t)(uintptr_t)alliance; r.ebp = (uint32_t)(uintptr_t)&counter;
        g_regs = r; g_entry = (uint32_t)(uintptr_t)(b + 0x00291C12); g_newEsp = (uint32_t)(uintptr_t)(g_stack + 0x2000);
        *(uint32_t*)(g_stack + 0x2000 + 0x24) = (uint32_t)(uintptr_t)alliance;      // [esp+0x24] = the alliance
        RunFragment();
        Expect("FotS battle sounds from land unit", c.key, self[0x358], c.audioLand);
        // 5. the same for a ship (0x10291B99 -> 0x10291BB9)
        memset(self, 0, sizeof(self));
        r = {}; r.eax = (uint32_t)(uintptr_t)ship; r.ebx = (uint32_t)(uintptr_t)&counter; r.ebp = (uint32_t)(uintptr_t)&counter;
        r.edx = (uint32_t)(uintptr_t)alliance; r.edi = (uint32_t)(uintptr_t)self; Run(b + 0x00291B99, r);
        Expect("FotS battle sounds from ship", c.key, self[0x358], c.audioShip);
    }
    printf("%s: %d results differ from what is expected\n", withFix ? "with TWS2-Fixing-Project" : "vanilla engine", g_bad);
    fflush(stdout);
    return g_bad;
}
