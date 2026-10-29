// fix_group.cpp -- single unit group fix (G, Ctrl+G and Ctrl+number accept one selected unit).
//
// From ShakuEight/Shogun2-fix (fixes/single-unit-group-fix/src/groupfix.cpp), Copyright 2026 ShakuEight,
// Apache License 2.0 (third_party/LICENSE-APACHE-2.0). Ported to this project: the fix itself is
// unchanged; the development-only parts (toggle key, tones, environment variables, research probe and
// its fifteen probe-only hooks, sampler thread, harness exports) are removed, the detour stubs come from
// the GCC HOOK_DETOUR macro, and the log lines go to tws2fix.log.
//
// Requested feature, not a bug: in battle, G groups the selected units (a group card and a number),
// Ctrl+number groups them under that number, Ctrl+G locks a group's formation. The game refuses all of
// it for a single selected unit: a group needs two or more.
//
// Engine decode (ShakuEight's notes):
// - The battle UI's selection manager (singleton [0x11bbead8]) holds the selected units in a list at
//   +0x24 (count +0x28, array +0x2C) and its unit groups at +0x40 (count) / +0x44 (array; a group
//   entry: unit count +0x0C, units +0x10, number slot +0x14).
// - FUN_10f89a40 (ECX = manager, slot or -1) makes a group of the selection: it starts with
//   CMP dword [ESI+0x28],1 / JBE return, so one selected unit returns before anything happens. With
//   two or more it calls the manager's virtual +0x20 (the group create, which issues the battle
//   command BCQ_ARMY_CREATE_UNIT_GROUP), records the group under its number and refreshes the cards.
//   Its callers: FUN_10f9de40 (G: ungroup when the selection is exactly a group, else a group in the
//   first free number slot) and FUN_10f91e40 (Ctrl+number: that slot).
// - The engine side has no unit count check: the command's executor FUN_102a5e40 makes the group
//   (FUN_10166e40), adds each unit (group virtual +0x70, FUN_1041dcc0) and finalises it
//   (FUN_101ffa70); removing units (+0x74, FUN_10472ed0) never disbands a group. The two script
//   bindings that refuse groups of one ("group must have 2 or more units") are the Lua
//   unit_controller's, not the player's.
// - The group frame (the yellow box with the number around a group's unit cards): the selection
//   manager's FUN_110e7280 keeps one "group_frame" component per UI group and places it with
//   FUN_11223470 (ECX = frame, the group's unit list, the two-row card layout flag), which starts
//   with CMP dword [EBP+4],1 / JA: one unit or none hides the frame and returns. Its two-row branch
//   for an odd count reads a second card while looking for the row break.
// - G reaches the group through the button "button_group" (default_keys.xml maps toggle_group to
//   it); an inactive button swallows its key. The land orders panel's button state table builder
//   FUN_111016d0 sets the group button (table entry +0x240) active only when the selected unit count
//   (the list FUN_110ee490 fills, [ESP+0x38]) is above 1 and there is room for another group, else
//   inactive (CMP dword [ESP+0x38],1 / JBE at +0x366). Found with ShakuEight's probe in a live
//   session: the state setter FUN_110e22f0 called from FUN_111024e0 set "inactive" at one selected
//   unit and "active" at two.
//
// The fix:
// - the immediates of the three count tests become 0 (selection, frame, group button state): only an
//   empty selection or group is refused. Each whole test is byte-checked first; only the immediate is
//   written (0 = fix on, 1 = the game's own value, written back at unload);
// - an entry hook on FUN_11223470 clears the two-row flag for a one-unit group, so its frame takes
//   the single-row branch (sized from the first card and the count) and never reads a second card.
// A hook right after the selection test (FUN_10f89a40+0x9) counts the groups made from the selection
// and logs every one made from a single unit; the first one-unit frame placed is logged too.

#include "log.h"
#include "hook.h"
#include "gamemem.h"
#include "stack_offsets_group.h"
#include "stack_offsets_group_mid.h"
#include <windows.h>
#include <cstdint>
#include <cstring>

static uint32_t g_base = 0;

// ---------------------------------------------------------------------------
// The fix sites: three unit-count tests "more than 1". Each is byte-checked as a whole; only its
// immediate is written, 1 (the game) or 0 (the fix). Never hooked.
// ---------------------------------------------------------------------------

enum { F_SELECTION = 0, F_FRAME, F_BUTTON, FIX_SITE_COUNT };
static const HookDef g_fixSites[FIX_SITE_COUNT] = {
    { "FUN_10f89a40+0x3", "group of the selection: more than 1 unit", 0xF89A43, 6,
      { 0x83, 0x7E, 0x28, 0x01, 0x76, 0x3E }, -1, false },          // CMP dword [ESI+0x28],1; JBE
    { "FUN_11223470+0xF", "group frame shown: more than 1 unit", 0x122347F, 7,
      { 0x83, 0x7D, 0x04, 0x01, 0x8B, 0x4B, 0x04 }, -1, false },    // CMP dword [EBP+4],1; MOV ECX,[EBX+4] (JA follows)
    { "FUN_111016d0+0x366", "group button active: more than 1 unit selected", 0x1101A36, 7,
      { 0x83, 0x7C, 0x24, 0x38, 0x01, 0x76, 0x21 }, -1, false },    // CMP dword [ESP+0x38],1; JBE (inactive)
};
static const int kImmOff[FIX_SITE_COUNT] = { 3, 3, 4 };       // offset of the immediate in each site
static const uint8_t IMM_GAME = 0x01, IMM_FIX = 0x00;

// ---------------------------------------------------------------------------
// Hooks (stolen bytes: whole instructions, no branch into the range)
// ---------------------------------------------------------------------------

enum {
    H_CREATED = 0,  // FUN_10f89a40+0x9: past the count test (ESI = manager; one push done, slot at the mid ARG1)
    H_FRAMESIZE,    // FUN_11223470: one group frame placed around its cards (ECX = frame, units list, two-row flag); fix
    HOOK_COUNT
};

static const HookDef g_hookDefs[HOOK_COUNT] = {
    { "FUN_10f89a40+0x9", "group of the selection made", 0xF89A49, 8,
      { 0x8B, 0x06, 0x53, 0x57, 0x8B, 0x7C, 0x24, 0x10 }, -1, false },              // MOV EAX,[ESI]; PUSH EBX; PUSH EDI; MOV EDI,[ESP+0x10]
    { "FUN_11223470", "group frame placed (fix: one card, single row)", 0x1223470, 5,
      { 0x83, 0xEC, 0x50, 0x53, 0x55 }, -1, false },                                // SUB ESP,0x50; PUSH EBX; PUSH EBP
};

static HookSlot g_slots[HOOK_COUNT] = {};
extern "C" uint8_t* group_Trampolines[HOOK_COUNT];   // read by the detour stubs below
uint8_t* group_Trampolines[HOOK_COUNT] = {};

// ---------------------------------------------------------------------------
// Engine layout (see the header)
// ---------------------------------------------------------------------------

static const uint32_t SEL_OFF_N = 0x28;                     // selection manager: selected unit count

static inline uint32_t R32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint32_t Rd(uint32_t addr) { uint32_t v = 0; ReadDword(addr, &v); return v; }
static uint32_t SelCount(uint32_t mgr) { return PtrPlausible(mgr) ? Rd(mgr + SEL_OFF_N) : 0xFFFFFFFFu; }

// ---------------------------------------------------------------------------
// Fix state
// ---------------------------------------------------------------------------

static volatile LONG g_fixOn = 0;
static volatile LONG g_fCreated = 0, g_fSingles = 0;
static volatile LONG g_fFrames1 = 0;                 // frames of one-unit groups placed, single-row forced
static bool g_installed = false;

// Writes the immediates of every fix site for `on`; false when a code page cannot be written
// (the sites already written are put back to the game's value).
static bool ApplyImm(bool on) {
    uint8_t v = on ? IMM_FIX : IMM_GAME, back = on ? IMM_GAME : IMM_FIX;
    for (int i = 0; i < FIX_SITE_COUNT; i++) {
        if (Hook_WriteCode((uint8_t*)(uintptr_t)(g_base + g_fixSites[i].rva + kImmOff[i]), &v, 1)) continue;
        for (int k = 0; k < i; k++)
            Hook_WriteCode((uint8_t*)(uintptr_t)(g_base + g_fixSites[k].rva + kImmOff[k]), &back, 1);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------

// FUN_10f89a40+0x9: the selection passed the count test; the group is made next.
static void HandleCreated(const uint8_t* ctx) {
    uint32_t mgr = R32(ctx + GROUPMID_OFF_ESI), slot = R32(ctx + GROUPMID_OFF_ARG1), n = SelCount(mgr);
    InterlockedIncrement(&g_fCreated);
    if (n == 1) {
        InterlockedIncrement(&g_fSingles);
        Log("groupfix: a group of one unit made from the selection (slot %d)", (int)slot);
    }
}

// FUN_11223470 (ECX = the group's frame, arg1 = the group's unit list {?, count, array}, arg2 =
// the two-row card layout flag): the frame of a one-unit group. With the fix on the count test
// lets it through (fix site F_FRAME); the two-row layout's odd-count branch reads a second card,
// so a one-unit group always takes the single-row branch (arg2 = 0), which sizes the frame from
// the first card and the count.
static void HandleFrameSize(const uint8_t* ctx) {
    if (!InterlockedCompareExchange(&g_fixOn, 0, 0)) return;
    uint32_t list = R32(ctx + GROUP_OFF_ARG1);
    if (Rd(list + 4) != 1) return;
    uint32_t a2 = R32(ctx + GROUP_OFF_ARG2);
    if (a2 & 0xFF) {
        uint32_t esp = R32(ctx + GROUP_OFF_ESP_BEFORE_PUSHAD);   // the caller's argument slot: rewritten in place
        WriteDwordTry(esp + 8, a2 & ~0xFFu);
    }
    if (InterlockedIncrement(&g_fFrames1) == 1)
        Log("groupfix: first frame of a one-unit group placed (two-row flag was %u)", a2 & 0xFF);
}

// ---------------------------------------------------------------------------
// Detours
// ---------------------------------------------------------------------------

extern "C" void __stdcall group_Handler0(const uint8_t* ctx) { HandleCreated(ctx); }
extern "C" void __stdcall group_Handler1(const uint8_t* ctx) { HandleFrameSize(ctx); }

HOOK_DETOUR(group_Detour0, group_Handler0, group_Trampolines, 0)
HOOK_DETOUR(group_Detour1, group_Handler1, group_Trampolines, 1)

static const HookDetour g_detours[HOOK_COUNT] = { group_Detour0, group_Detour1 };

// ---------------------------------------------------------------------------
// Install / remove
// ---------------------------------------------------------------------------

void GroupFix_Install() {
    Mem_Init();
    uint32_t size = 0;
    uint8_t* base = Hook_GameModule(false, &size);
    if (!base) { Log("groupfix: REFUSED, empire.retail.dll is not loaded"); return; }
    if (size != HOOK_ANALYSED_IMAGE_SIZE) {
        Log("groupfix: REFUSED, SizeOfImage 0x%08X is not the analysed build", size);
        return;
    }
    for (int i = 0; i < FIX_SITE_COUNT; i++) {
        if (!Hook_SiteMatches(base, g_fixSites[i])) {
            Log("groupfix: REFUSED, bytes at RVA 0x%06X (%s) differ from the analysed binary", g_fixSites[i].rva,
                g_fixSites[i].func);
            return;
        }
    }
    for (int h = 0; h < HOOK_COUNT; h++) {
        if (!Hook_SiteMatches(base, g_hookDefs[h])) {
            Log("groupfix: REFUSED, bytes at RVA 0x%06X (%s) differ from the analysed binary", g_hookDefs[h].rva,
                g_hookDefs[h].func);
            return;
        }
    }
    g_base = (uint32_t)(uintptr_t)base;
    if (!Hook_Prepare(base, g_hookDefs, HOOK_COUNT, nullptr, group_Trampolines, g_slots)) {
        Log("groupfix: REFUSED, no trampoline memory");
        g_base = 0;
        return;
    }
    bool fixOff = false;
    const char* offWhy = "";
    if (!ApplyImm(true)) {
        Log("groupfix: cannot write the code pages of the fix sites; fix OFF");
        fixOff = true;
        offWhy = "OFF (code page not writable)";
    }
    InterlockedExchange(&g_fixOn, fixOff ? 0 : 1);
    g_installed = true;
    Hook_Patch(g_detours, g_slots, HOOK_COUNT);
    for (int h = 0; h < HOOK_COUNT; h++)
        if (g_slots[h].active)
            Log("groupfix: hook %s (%s) installed at RVA 0x%06X", g_hookDefs[h].func, g_hookDefs[h].role, g_hookDefs[h].rva);
    Log("groupfix: fix %s: G, Ctrl+G and Ctrl+number accept a single unit, its group frame is shown", fixOff ? offWhy : "ON");
}

void GroupFix_Remove() {
    Hook_Remove(g_slots, HOOK_COUNT);
    if (!g_base) return;
    if (g_installed) ApplyImm(false);    // the game's own immediate back
    g_installed = false;
    Log("groupfix: groups made from the selection %ld, of them single units %ld; frames of one-unit groups placed %ld",
        g_fCreated, g_fSingles, g_fFrames1);
}
