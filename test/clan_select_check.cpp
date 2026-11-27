// Calls the real engine's clan lookup FUN_1101a930 on a test clan list. Build: i686-w64-mingw32-g++ -O1 -static -o clan_select_check.exe clan_select_check.cpp
// Run next to empire.retail.dll: "wine clan_select_check.exe" (vanilla) or, with dinput8.dll + tws2fix.ini beside it,
// WINEDLLOVERRIDES="dinput8=n,b" wine clan_select_check.exe fix
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <initializer_list>
struct EStr { uint32_t len, cap; wchar_t* text; };
typedef void (__fastcall *StrCtor)(EStr*, void*, const char*);
typedef void* (__fastcall *Find)(void* self, void*, uint32_t, uint32_t, wchar_t*);
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
    StrCtor mk = (StrCtor)(b + 0x000ED420);                  // wide string from a narrow literal
    typedef void* (__cdecl *PoolFn)();                         // FUN_100ef940: the string memory pool
    typedef void* (__fastcall *AllocFn)(void*, void*, uint32_t);   // FUN_100f3030 (thiscall pool, size)
    PoolFn pool = (PoolFn)(b + 0x000EF940); AllocFn alloc = (AllocFn)(b + 0x000F3030);
    Find find = (Find)(b + 0x0101A930);
    // Expanded Japan-like keys, alphabetical like the campaign's list
    const char* keys[] = { "aki", "akita", "date", "hatakeyama", "hatakeyama_mutsu", "menasunkur", "nasu", "oda", "sugi", "uesugi" };
    const int n = sizeof(keys) / sizeof(keys[0]);
    static uint8_t recs[n][0xB4];
    for (int i = 0; i < n; i++) mk((EStr*)recs[i], nullptr, keys[i]);
    static uint8_t list[0x40]; *(uint32_t*)(list + 0x2C) = n; *(void**)(list + 0x30) = recs;
    static uint8_t self[0x100]; *(void**)(self + 0xA8) = list;
    const char* buttons[] = { "button_nasu", "button_menasunkur", "button_aki", "button_akita", "button_sugi", "button_uesugi",
                              "button_hatakeyama", "button_hatakeyama_mutsu", "button_oda", "button_date", "button_shimazu" };
    int bad = 0;
    for (const char* bt : buttons) {
        // the button name as the engine's narrow string { length, capacity, char* }, buffer from its pool
        uint32_t len = (uint32_t)strlen(bt);
        char* buf = (char*)alloc(pool(), nullptr, len + 1); memcpy(buf, bt, len + 1);
        void* r = find(self, nullptr, len, len, (wchar_t*)buf);   // the engine frees the buffer
        const char* got = "(none)";
        for (int i = 0; i < n; i++) if (r == recs[i]) got = keys[i];
        const char* want = strchr(bt, '_') + 1; bool known = false;
        for (int i = 0; i < n; i++) if (!strcmp(keys[i], want)) known = true;
        bool ok = known ? !strcmp(got, want) : r == nullptr;
        if (!ok) bad++;
        printf("%-24s -> %-18s %s\n", bt, got, ok ? "own clan" : "WRONG CLAN");
    }
    printf("%s: %d of %d buttons start the wrong clan\n", withFix ? "with TWS2-Fixing-Project" : "vanilla engine", bad, (int)(sizeof(buttons) / sizeof(buttons[0])));
    return bad;
}
