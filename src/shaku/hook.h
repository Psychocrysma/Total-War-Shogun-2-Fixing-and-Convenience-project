// hook.h -- the entry / mid-function hook engine shared by the battle fixes (src/shaku/fix_*.cpp).
//
// From ShakuEight/Shogun2-fix (proxy/hook.h), Copyright 2026 ShakuEight, Apache License 2.0
// (third_party/LICENSE-APACHE-2.0). Changed for this project: the HOOK_DETOUR / HOOK_DETOUR_RET
// macros emit the same byte sequence through GCC top-level assembly instead of MSVC __asm, since
// this project is built with mingw-w64. Everything a handler sees (the pushad/pushfd context and the
// generated stack offsets) is unchanged.
//
// A fix describes its patch sites in a HookDef table (stolen bytes recorded from the analysed
// binary) and installs them in four steps:
//
//   1. Hook_GameModule      find empire.retail.dll (optionally require the analysed SizeOfImage)
//   2. Hook_SiteMatches     byte-check EVERY site before writing any; on a mismatch install nothing
//   3. Hook_Prepare         build the trampolines (stolen bytes as they are in memory + JMP back)
//   4. Hook_Patch           write JMP detour + NOP padding into each prepared site
//
// Hook_Remove restores the original bytes. The detour of a hook is a naked stub that saves the
// registers, passes a pointer to them to a __stdcall handler and jumps through the hook's
// trampoline; HOOK_DETOUR below emits it. The stack offsets a handler reads come from a header
// generated for exactly that sequence (pushad, pushfd, mark:ctx, push:ctxptr).
//
// Mid-function sites need no special case: the stolen bytes are whole instructions with no
// branch into them, so the trampoline runs them and jumps back. An absolute address inside the
// stolen bytes (relocOff >= 0) is compared after adding the module's rebase delta.
#pragma once
#include <windows.h>
#include <cstdint>

static const int HOOK_MAX_STOLEN = 16;
static const uint32_t HOOK_ANALYSED_IMAGE_BASE = 0x10000000;   // Ghidra names
static const uint32_t HOOK_ANALYSED_IMAGE_SIZE = 0x01D88000;   // SizeOfImage of the analysed build

struct HookDef {
    const char* func;       // Ghidra name (or name+offset for a mid-function site)
    const char* role;
    uint32_t    rva;
    int         len;        // stolen bytes, whole instructions, no branch into the range, >= 5
    uint8_t     expected[HOOK_MAX_STOLEN];
    int         relocOff;   // offset of an absolute address inside the stolen bytes, or -1
    bool        probeOnly;  // the fix decides (Hook_Prepare's `use`); the engine ignores it
};

// Per-site install state, one per HookDef, owned by the fix.
struct HookSlot {
    uint8_t* target;                       // null = not prepared
    uint8_t  original[HOOK_MAX_STOLEN];
    int      len;
    bool     active;                       // patched
};

typedef void (*HookDetour)();

uint8_t* Hook_GameModule(bool requireSize, uint32_t* sizeOut);
uint32_t Hook_RebaseDelta(const uint8_t* base);
void Hook_Expected(const HookDef& d, uint32_t delta, uint8_t* out);
bool Hook_SiteMatches(const uint8_t* base, const HookDef& d);
bool Hook_Prepare(uint8_t* base, const HookDef* defs, int n, const bool* use, uint8_t** tramps,
                  HookSlot* slots);
void Hook_Patch(const HookDetour* detours, HookSlot* slots, int n);
void Hook_Remove(HookSlot* slots, int n);
bool Hook_WriteCode(uint8_t* at, const void* bytes, int n);

// ---------------------------------------------------------------------------------------------
// Detour stubs (GCC top-level assembly, Intel syntax). Requirements, which the fix sources meet:
//   - `handler` is `extern "C" ... __stdcall handler(const uint8_t* ctx)`: one 4-byte argument, so
//     its decorated symbol is _handler@4 (both macro variants).
//   - `trampArray` is a file-scope `uint8_t* trampArray[N]` declared `extern "C"` (or given an asm
//     label); g++ mangles a `static` variable's symbol (_ZL...), which the stub could not reference.
// The emitted bytes are exactly the MSVC stub's: 60 9C 54 E8 .. 9D 61 FF 25 .. (HOOK_DETOUR).
// ---------------------------------------------------------------------------------------------
#define HOOK_STR_(x) #x
#define HOOK_STR(x) HOOK_STR_(x)

#define HOOK_DETOUR(name, handler, trampArray, index)                                       \
    extern "C" void name();                                                                 \
    __asm__(".text\n"                                                                       \
            ".intel_syntax noprefix\n"                                                      \
            ".globl _" #name "\n"                                                           \
            "_" #name ":\n"                                                                 \
            "  pushad\n"                                                                    \
            "  pushfd\n"                                                                    \
            "  push esp\n"                                                                  \
            "  call _" #handler "@4\n"                                                      \
            "  popfd\n"                                                                     \
            "  popad\n"                                                                     \
            "  jmp dword ptr [_" #trampArray " + " HOOK_STR(index) "*4]\n"                  \
            ".att_syntax prefix\n");

// Entry hook whose handler may answer for the game function: HOOK_CONTINUE (0) runs the function,
// HOOK_RETURN(value) returns to the caller at once with EAX = value and `popBytes` of stack
// arguments removed (the function's own RET N). Only for entry hooks.
#define HOOK_CONTINUE 0u
#define HOOK_RETURN(value) (0x80000000u | ((uint32_t)(value) & 0x7FFFFFFFu))
#define HOOK_DETOUR_RET(name, handler, trampArray, index, popBytes)                         \
    extern "C" void name();                                                                 \
    __asm__(".text\n"                                                                       \
            ".intel_syntax noprefix\n"                                                      \
            ".globl _" #name "\n"                                                           \
            "_" #name ":\n"                                                                 \
            "  pushad\n"                                                                    \
            "  pushfd\n"                                                                    \
            "  push esp\n"                                                                  \
            "  call _" #handler "@4\n"                                                      \
            "  test eax, eax\n"                                                             \
            "  jnz 1f\n"                                                                    \
            "  popfd\n"                                                                     \
            "  popad\n"                                                                     \
            "  jmp dword ptr [_" #trampArray " + " HOOK_STR(index) "*4]\n"                  \
            "1:\n"                                                                          \
            "  and eax, 0x7FFFFFFF\n"                                                       \
            "  mov dword ptr [esp + 0x20], eax\n"                                           \
            "  popfd\n"                                                                     \
            "  popad\n"                                                                     \
            "  ret " HOOK_STR(popBytes) "\n"                                                \
            ".att_syntax prefix\n");
