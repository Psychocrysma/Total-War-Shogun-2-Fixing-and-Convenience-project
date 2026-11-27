// Calls the engine's real campaign minimap getters FUN_11121990 (centre) and FUN_11121a80 (size) on a made-up
// campaign object, vanilla and with TWS2-Fixing-Project (src/patch_minimap.cpp).
// Build: i686-w64-mingw32-g++ -O1 -static -o minimap_check.exe minimap_check.cpp
// Run next to empire.retail.dll: "wine minimap_check.exe" (vanilla) or, with dinput8.dll + tws2fix.ini beside it,
// WINEDLLOVERRIDES="dinput8=n,b" wine minimap_check.exe fix
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
    typedef float* (__fastcall *Getter)(void* self, void*, float* out);
    Getter centre = (Getter)(b + 0x01121990), size = (Getter)(b + 0x01121A80);
    // campaign UI object: +0x10DE flag, +0xA98 -> A; A+8 -> B (B+0xEA0 -> C, C+0x30 -> campaign key string;
    // B+0x1494 -> theatre list -> table); A+0xB0 -> D, D+0x6E8 -> theatre record (x0,y0,x1,y1 at +0)
    static uint8_t self[0x1200], A[0x200], B[0x1600], C[0x40], D[0x800], list[8], table[0x100];
    static uint8_t recs[2][0xA0]; static EStr key;
    *(void**)(self + 0xA98) = A; *(void**)(A + 8) = B; *(void**)(A + 0xB0) = D;
    *(void**)(B + 0xEA0) = C; *(void**)(C + 0x30) = &key;
    *(void**)(B + 0x1494) = list; *(void**)list = table; *(uint32_t*)(table + 0x80) = 2; *(void**)(table + 0x84) = recs;
    mk((EStr*)(recs[0] + 0x5C), nullptr, "japan"); mk((EStr*)(recs[1] + 0x5C), nullptr, "other");
    float other[4] = { 0, 0, 100, 100 }; memcpy(recs[1], other, 16);
    struct Case { const char* what; const char* camp; float box[4]; bool nullRec; };
    const Case cases[] = {
        { "Sengoku, CA's box",            "main_japan", { -305, -240, 270, 70 }, false },
        { "Sengoku, Hokkaido add-on box", "main_japan", { -305, -240, 432, 196 }, false },
        { "Sengoku, 200% map box",        "main_japan", { -575, -310, 575, 310 }, false },
        { "Sengoku, no theatre record",   "main_japan", { 0, 0, 0, 0 }, true },
        { "Sengoku, broken record",       "main_japan", { 5, 5, 5, 5 }, false },
        { "Fall of the Samurai",          "jap_boshin", { -305, -240, 432, 196 }, false },
    };
    printf("%s\n", withFix ? "with TWS2-Fixing-Project" : "vanilla engine");
    for (const Case& c : cases) {
        mk(&key, nullptr, c.camp);
        memcpy(recs[0], c.box, 16);
        *(void**)(D + 0x6E8) = c.nullRec ? nullptr : recs[0];
        float ce[2] = { 99, 99 }, sz[2] = { 99, 99 };
        centre(self, nullptr, ce); size(self, nullptr, sz);
        printf("  %-30s centre %7.1f,%7.1f  size %6.1f x %5.1f  -> area %7.1f,%7.1f .. %6.1f,%6.1f\n", c.what, ce[0], ce[1], sz[0], sz[1],
               ce[0] - sz[0] / 2, ce[1] - sz[1] / 2, ce[0] + sz[0] / 2, ce[1] + sz[1] / 2);
    }
    fflush(stdout);
    return 0;
}
