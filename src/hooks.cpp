#include "common.h"
#include <cstring>

bool WriteCode(void* address, const void* bytes, size_t len) {
    DWORD old = 0;
    if (!VirtualProtect(address, len, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(address, bytes, len);
    DWORD tmp = 0;
    VirtualProtect(address, len, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), address, len);
    return true;
}

void* HookImport(HMODULE module, const char* importDll, const char* funcName, void* replacement) {
    auto base = reinterpret_cast<uint8_t*>(module);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    auto desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
    for (; desc->Name; ++desc) {
        const char* name = reinterpret_cast<const char*>(base + desc->Name);
        if (_stricmp(name, importDll) != 0) continue;
        auto thunkNames = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + desc->OriginalFirstThunk);
        auto thunkFuncs = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + desc->FirstThunk);
        for (; thunkNames->u1.AddressOfData; ++thunkNames, ++thunkFuncs) {
            if (IMAGE_SNAP_BY_ORDINAL32(thunkNames->u1.Ordinal)) continue;
            auto imp = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + thunkNames->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(imp->Name), funcName) != 0) continue;
            void* original = reinterpret_cast<void*>(thunkFuncs->u1.Function);
            DWORD old = 0;
            if (!VirtualProtect(&thunkFuncs->u1.Function, sizeof(DWORD), PAGE_READWRITE, &old)) return nullptr;
            thunkFuncs->u1.Function = reinterpret_cast<DWORD>(replacement);
            DWORD tmp = 0;
            VirtualProtect(&thunkFuncs->u1.Function, sizeof(DWORD), old, &tmp);
            return original;
        }
    }
    return nullptr;
}

void* InstallDetour(void* target, void* hook, size_t stolenLen) {
    if (stolenLen < 5) return nullptr;
    // trampoline: stolen bytes + jmp back
    auto tramp = reinterpret_cast<uint8_t*>(VirtualAlloc(nullptr, stolenLen + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp) return nullptr;
    memcpy(tramp, target, stolenLen);
    tramp[stolenLen] = 0xE9;
    *reinterpret_cast<int32_t*>(tramp + stolenLen + 1) =
        static_cast<int32_t>(reinterpret_cast<uintptr_t>(target) + stolenLen - (reinterpret_cast<uintptr_t>(tramp) + stolenLen + 5));
    // patch target: jmp hook, pad with int3
    uint8_t patch[32];
    patch[0] = 0xE9;
    *reinterpret_cast<int32_t*>(patch + 1) =
        static_cast<int32_t>(reinterpret_cast<uintptr_t>(hook) - (reinterpret_cast<uintptr_t>(target) + 5));
    for (size_t i = 5; i < stolenLen; ++i) patch[i] = 0xCC;
    if (!WriteCode(target, patch, stolenLen)) { VirtualFree(tramp, 0, MEM_RELEASE); return nullptr; }
    return tramp;
}
