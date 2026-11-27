// Checks which of the engine's USER32 import slots TWS2-Fixing-Project replaces, with and without s2fix's
// files in the folder. s2fix's borderless module needs SetWindowLongW, SetWindowPos, MoveWindow,
// PeekMessageW and GetClientRect untouched. The engine is mapped without resolving imports, so an untouched
// slot still holds its hint/name RVA from the file.
// Build: i686-w64-mingw32-g++ -O1 -static -o s2fix_compat_check.exe s2fix_compat_check.cpp
// Run next to empire.retail.dll, dinput8.dll and tws2fix.ini: WINEDLLOVERRIDES="dinput8=n,b" wine s2fix_compat_check.exe
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
int main() {
    HMODULE di = LoadLibraryW(L"dinput8.dll");
    HMODULE eng = LoadLibraryExW(L"empire.retail.dll", nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!di || !eng) { printf("load failed\n"); return 2; }
    GetProcAddress(eng, "entry_point"); Sleep(300);
    uint8_t* b = (uint8_t*)eng;
    auto nt = (IMAGE_NT_HEADERS32*)(b + ((IMAGE_DOS_HEADER*)b)->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    const char* s2fixNeeds[] = { "SetWindowLongW", "SetWindowPos", "MoveWindow", "PeekMessageW", "GetClientRect" };
    const char* others[] = { "CreateWindowExW", "GetAsyncKeyState" };
    int touched = 0;
    for (auto d = (IMAGE_IMPORT_DESCRIPTOR*)(b + dir.VirtualAddress); d->Name; ++d) {
        if (_stricmp((char*)b + d->Name, "USER32.dll")) continue;
        auto names = (IMAGE_THUNK_DATA32*)(b + d->OriginalFirstThunk);
        auto funcs = (IMAGE_THUNK_DATA32*)(b + d->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++funcs) {
            if (IMAGE_SNAP_BY_ORDINAL32(names->u1.Ordinal)) continue;
            const char* n = (char*)((IMAGE_IMPORT_BY_NAME*)(b + names->u1.AddressOfData))->Name;
            bool hooked = funcs->u1.Function != names->u1.AddressOfData;
            for (const char* w : s2fixNeeds) if (!strcmp(n, w)) { printf("  s2fix needs %-16s %s\n", n, hooked ? "HOOKED by us" : "untouched"); touched += hooked; }
            for (const char* w : others) if (!strcmp(n, w)) printf("  (ours)       %-16s %s\n", n, hooked ? "hooked" : "untouched");
        }
    }
    printf("%d of the slots s2fix needs are hooked\n", touched);
    return 0;
}
