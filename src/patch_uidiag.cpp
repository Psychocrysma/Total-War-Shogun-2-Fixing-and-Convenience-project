// UI diagnostics (ui_scale_log_box=1): who shows and positions the battle drag-selection box.
//
// The drag-selection rectangle is the UI component "box_selection" (ui/battle ui/box_selection), made by
// FUN_110d2270 (this = a 4-byte holder in the battle HUD at +0xD0C; it stores the component pointer).
// Under UI scaling the box appears at the cursor position times the scale, so the engine positions it
// from screen pixels. To find that code this file remembers the holder when the box is made and logs,
// once per caller, every call of the component's SetVisible (FUN_10f9c360) on the box, with the engine
// return addresses up the stack. Diagnostics only; nothing is changed.
#include "common.h"
#include "shaku/hook.h"
#include "shaku/gamemem.h"
#include <cstring>

namespace {

enum { H_BOX_CTOR = 0, H_SET_VISIBLE, HOOK_COUNT };
const HookDef kHooks[HOOK_COUNT] = {
    { "FUN_110d2270", "box_selection holder ctor", 0x010D2270, 5, { 0x83, 0xEC, 0x18, 0x53, 0x56 }, -1, false },
    { "FUN_10f9c360", "UIComponent::SetVisible",   0x00F9C360, 8, { 0x83, 0xEC, 0x0C, 0x53, 0x8B, 0x5C, 0x24, 0x14 }, -1, false },
};
constexpr int CTX_ECX = 0x1C, CTX_RET = 0x24, CTX_ARG1 = 0x28;

uint8_t* g_base = nullptr;
HookSlot g_slots[HOOK_COUNT];
uint32_t g_holder = 0;
uint32_t g_seen[64]; int g_seenCount = 0;

inline uint32_t R32(const uint8_t* ctx, int off) { return *reinterpret_cast<const uint32_t*>(ctx + off); }

extern "C" void __stdcall uidiag_OnBoxCtor(const uint8_t* ctx) {
    g_holder = R32(ctx, CTX_ECX);
    Log("uidiag: box_selection holder at %08X", g_holder);
}

extern "C" void __stdcall uidiag_OnSetVisible(const uint8_t* ctx) {
    if (!g_holder) return;
    uint32_t comp = 0; ReadDword(g_holder, &comp);
    if (!comp || comp != R32(ctx, CTX_ECX)) return;
    if ((R32(ctx, CTX_ARG1) & 0xFF) && (GetAsyncKeyState(VK_MENU) & 0x8000) && (GetAsyncKeyState(VK_LBUTTON) & 0x8000)) {
        static DWORD last = 0; if (GetTickCount() - last > 1000) { last = GetTickCount(); DLog("altmove: the engine started a selection box during an Alt-hold"); }
    }
    if (!g_cfg.ui_scale_log_box) return;
    uint32_t ret = R32(ctx, CTX_RET) - static_cast<uint32_t>(g_engine.base);
    uint32_t vis = R32(ctx, CTX_ARG1) & 0xFF;
    uint32_t key = ret ^ (vis << 31);
    for (int i = 0; i < g_seenCount; i++) if (g_seen[i] == key) return;
    if (g_seenCount < 64) g_seen[g_seenCount++] = key;
    char chain[300]; int n = 0;
    for (int i = 0; i < 48 && n < 270; i++) {
        uint32_t v = R32(ctx, CTX_ARG1 + 4 * i);
        if (v > g_engine.base + 0x1000 && v < g_engine.base + 0x01590000) n += snprintf(chain + n, sizeof(chain) - n, " %08X", static_cast<uint32_t>(v - g_engine.base));
    }
    Log("uidiag: box_selection SetVisible(%u) from RVA 0x%08X; stack:%s", vis, ret, chain);
}

extern "C" uint8_t* uidiag_Trampolines[HOOK_COUNT];
uint8_t* uidiag_Trampolines[HOOK_COUNT] = {};
HOOK_DETOUR(uidiag_D0, uidiag_OnBoxCtor,    uidiag_Trampolines, 0)
HOOK_DETOUR(uidiag_D1, uidiag_OnSetVisible, uidiag_Trampolines, 1)
const HookDetour g_detours[HOOK_COUNT] = { uidiag_D0, uidiag_D1 };

} // namespace

void PatchUIDiag_Install() {
    if (!g_cfg.ui_scale_log_box && !(g_cfg.alt_group_move && g_cfg.alt_debug_log)) return;
    if (!g_engine.knownBuild) { Log("uidiag: skipped, engine build not verified"); return; }
    Mem_Init();
    g_base = reinterpret_cast<uint8_t*>(g_engine.base);
    for (int h = 0; h < HOOK_COUNT; h++)
        if (!Hook_SiteMatches(g_base, kHooks[h])) { Log("uidiag: bytes at RVA 0x%06X (%s) differ; not installed", kHooks[h].rva, kHooks[h].func); return; }
    if (!Hook_Prepare(g_base, kHooks, HOOK_COUNT, nullptr, uidiag_Trampolines, g_slots)) { Log("uidiag: trampoline memory; not installed"); return; }
    Hook_Patch(g_detours, g_slots, HOOK_COUNT);
    if (g_cfg.ui_scale_log_box) Log("uidiag: ON, logging who shows the battle drag-selection box");
}

void PatchUIDiag_Remove() {
    if (!g_base) return;
    Hook_Remove(g_slots, HOOK_COUNT);
}
