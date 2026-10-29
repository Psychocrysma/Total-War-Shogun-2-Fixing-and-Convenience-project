// UI scaling: revives the engine's dormant "UI rect" path.
//
// The engine lays the UI out on a logical rectangle (ui_rect_left/top/width/height, debug variables that
// nothing in the shipped game sets), renders it to an overlay target, and composites that target over the
// backbuffer. With the rect left at zero the UI is laid out at screen size, so nothing scales. If the rect
// is made smaller than the screen, the layout shrinks, but the overlay target is still created at backbuffer
// size by one virtual helper, so the UI would just sit small in the corner. This patch
//   1. writes the smaller rect into the config objects when the game window is created (so W/H are known), and
//   2. detours that helper to report the rect size, so the smaller overlay is stretched to the full screen.
// Mouse coordinates are remapped by the engine from the same rect.
//
// Addresses are for empire.retail.dll of the May 2023 build (base 0x10000000). Object values live at +0x5C.
#include "common.h"
#include "shaku/hook.h"
#include <cmath>
#include <cstring>

namespace {

constexpr uint32_t kObjRectLeft   = 0x01C9F848;   // ui_rect_left   (int)
constexpr uint32_t kObjRectTop    = 0x01CA0110;   // ui_rect_top    (int)
constexpr uint32_t kObjRectWidth  = 0x01C9F728;   // ui_rect_width  (int)
constexpr uint32_t kObjRectHeight = 0x01CA06F0;   // ui_rect_height (int)
constexpr uint32_t kObjEnable     = 0x01C9F7E8;   // enable_ui_scaling (bool, defaults to 1)
constexpr uint32_t kValueOffset   = 0x5C;
constexpr uint32_t kFnOverlaySize = 0x013F7D20;   // virtual: void GetOverlayTargetSize(uint32_t out[2])
const uint8_t kFnOverlaySizePrologue[5] = {0x53, 0x8B, 0x5C, 0x24, 0x08};   // push ebx; mov ebx,[esp+8]

constexpr uint32_t kFnGetUiRect   = 0x013F7B40;   // virtual: int* GetUiRect(int out[4]) -> {left, top, right, bottom}
const uint8_t kFnGetUiRectPrologue[9] = {0x56, 0x57, 0x8B, 0xF9, 0xB9, 0x28, 0xF7, 0xC9, 0x11};   // push esi; push edi; mov edi,ecx; mov ecx,obj
// Return addresses (RVA) of the only two GetUiRect calls that must see the *scaled* rect: the UI layout
// size (FUN_113f7c00) and the UI projection matrix (FUN_113d6be0). Every other caller - the viewport
// setters and the mouse/coordinate mappings - gets the full screen, which makes the layout stretch to
// the screen and the mouse map screen pixels onto the scaled layout.
const uint32_t kSmallRectCallers[] = { 0x013F7C17, 0x013D6BF4 };

uint32_t g_uiW = 0, g_uiH = 0;     // current logical UI size, 0 = scaling inactive

// Battle drag-selection box. FUN_110ef580 (battle UI per-frame update) positions the "box_selection"
// component from the drag start and the cursor in screen pixels (start = normalised * screen size,
// cursor = NDC * screen size), then calls the component's SetPosition and SetSize. Under the scaled
// layout the component wants logical coordinates, so the box appeared at the cursor times the scale.
// Hooked just after the four pixel values are on the stack (0x110EF70B, right after the SetVisible):
// [esp+0x20] start x, [esp+0x10] start y, [esp+0x24] cursor x, [esp+0x18] cursor y, ESI = cursor x.
constexpr uint32_t RVA_BOX_SITE = 0x010EF70B;
const HookDef kBoxHook = { "FUN_110ef580+0x18B", "drag box pixels on the stack", RVA_BOX_SITE, 8, { 0x39, 0x74, 0x24, 0x20, 0x8D, 0x54, 0x24, 0x2C }, -1, false };
HookSlot g_boxSlot;
bool g_boxHooked = false;
long g_boxFrames = 0;
int g_screenW = 0, g_screenH = 0;
bool g_detourOk = false;
bool g_rectDetourOk = false;

using PFN_GetUiRect = int* (__fastcall*)(void* self, void* edx, int* out);
PFN_GetUiRect o_GetUiRect = nullptr;

// Diagnostics: every distinct caller of the rect getter, logged once (ui_scale_log_callers=1).
uint32_t g_seenCallers[256]; int g_seenCount = 0;

// Frame pointer kept so the diagnostics can read the engine's stack above our return address.
__attribute__((optimize("no-omit-frame-pointer"))) int* __fastcall my_GetUiRect(void* self, void* edx, int* out) {
    int* r = o_GetUiRect(self, edx, out);
    if (g_cfg.ui_scale_mode == 2 && g_uiW && g_uiH && g_screenW && g_screenH) {
        uintptr_t ret = reinterpret_cast<uintptr_t>(__builtin_return_address(0)) - g_engine.base;
        bool small = false;
        for (uint32_t c : kSmallRectCallers) if (ret == c) small = true;
        for (int i = 0; i < g_cfg.ui_scale_extra_count; i++) if (ret == g_cfg.ui_scale_extra_callers[i]) small = true;
        if (g_cfg.ui_scale_log_callers) {
            bool seen = false;
            for (int i = 0; i < g_seenCount; i++) if (g_seenCallers[i] == ret) { seen = true; break; }
            if (!seen && g_seenCount < 256) {
                g_seenCallers[g_seenCount++] = static_cast<uint32_t>(ret);
                // the return addresses further up the stack (engine code only), to see who asked
                char chain[256]; int n = 0;
                auto fp = reinterpret_cast<uint32_t*>(__builtin_frame_address(0));
                for (int i = 2; i < 40 && n < 230; i++) {
                    uint32_t v = fp[i];
                    if (v > g_engine.base + 0x1000 && v < g_engine.base + 0x01590000) n += snprintf(chain + n, sizeof(chain) - n, " %08X", static_cast<uint32_t>(v - g_engine.base));
                }
                Log("ui_scale: GetUiRect caller RVA 0x%08X (%s); stack:%s", static_cast<uint32_t>(ret), small ? "scaled rect" : "screen", chain);
            }
        }
        if (!small) { out[0] = 0; out[1] = 0; out[2] = g_screenW; out[3] = g_screenH; }
    }
    return r;
}

using PFN_OverlaySize = uint32_t* (__fastcall*)(void* self, void* edx, uint32_t* out);
PFN_OverlaySize o_OverlaySize = nullptr;

uint32_t* __fastcall my_OverlaySize(void* self, void* edx, uint32_t* out) {
    if (g_cfg.ui_scale_mode == 1 && g_cfg.ui_scale_rt && g_uiW && g_uiH) { out[0] = g_uiW; out[1] = g_uiH; return out; }
    return o_OverlaySize(self, edx, out);
}

void WriteInt(uint32_t objRva, int v) { *reinterpret_cast<int*>(g_engine.base + objRva + kValueOffset) = v; }
int  ReadInt(uint32_t objRva)         { return *reinterpret_cast<int*>(g_engine.base + objRva + kValueOffset); }

extern "C" void __stdcall uiscale_OnDragBox(uint8_t* ctx) {
    if (!g_uiW || !g_uiH || !g_screenW || !g_screenH) return;
    uint8_t* sp = ctx + 0x24;                      // the engine's ESP at the site
    float fx = static_cast<float>(g_uiW) / static_cast<float>(g_screenW);
    float fy = static_cast<float>(g_uiH) / static_cast<float>(g_screenH);
    auto sc = [](uint8_t* p, float f) { int v = *reinterpret_cast<int*>(p); *reinterpret_cast<int*>(p) = static_cast<int>(std::lround(v * f)); };
    sc(sp + 0x20, fx); sc(sp + 0x24, fx); sc(sp + 0x10, fy); sc(sp + 0x18, fy);
    *reinterpret_cast<int*>(ctx + 0x08) = *reinterpret_cast<int*>(sp + 0x24);   // ESI = cursor x
    g_boxFrames++;
}
extern "C" uint8_t* uiscale_Trampolines[1];
uint8_t* uiscale_Trampolines[1] = {};
HOOK_DETOUR(uiscale_D0, uiscale_OnDragBox, uiscale_Trampolines, 0)
const HookDetour g_boxDetour[1] = { uiscale_D0 };

void InstallDragBoxHook() {
    auto base = reinterpret_cast<uint8_t*>(g_engine.base);
    if (!Hook_SiteMatches(base, kBoxHook)) { Log("ui_scale: drag-box site bytes differ; box fix NOT applied"); return; }
    if (!Hook_Prepare(base, &kBoxHook, 1, nullptr, uiscale_Trampolines, &g_boxSlot)) { Log("ui_scale: drag-box trampoline memory; box fix NOT applied"); return; }
    Hook_Patch(g_boxDetour, &g_boxSlot, 1);
    g_boxHooked = true;
    Log("ui_scale: battle drag-selection box mapped to the scaled layout (FUN_110ef580 hooked)");
}

} // namespace

// Called from the window hooks whenever the game window's client size is known or changes.
void PatchUIScale_OnResolution(int w, int h) {
    if (!g_cfg.ui_scale_enabled() || !g_engine.knownBuild) return;
    if (w < 640 || h < 480) return;
    if (w == g_screenW && h == g_screenH) return;
    g_screenW = w; g_screenH = h;
    float scale = g_cfg.ui_scale;
    if (g_cfg.ui_scale_auto) {
        // "auto": give the UI the size it has at 1920x1080. At 1080p or below that is 1.0, i.e. no change.
        scale = std::fmin(static_cast<float>(w) / 1920.0f, static_cast<float>(h) / 1080.0f);
        Log("ui_scale: auto -> %.3f for %dx%d", scale, w, h);
    }
    // The layout code floors the logical size at 1280x1024; keep the rect at or above that so the
    // overlay target and the layout agree.
    float maxScale = std::fmin(static_cast<float>(w) / 1280.0f, static_cast<float>(h) / 1024.0f);
    if (scale > maxScale) { Log("ui_scale %.3f capped to %.3f for %dx%d (layout floor 1280x1024)", scale, maxScale, w, h); scale = maxScale; }
    if (scale <= 1.001f) { g_uiW = g_uiH = 0; WriteInt(kObjRectWidth, 0); WriteInt(kObjRectHeight, 0); Log("ui_scale: inactive at %dx%d", w, h); return; }
    g_uiW = static_cast<uint32_t>(std::lround(w / scale));
    g_uiH = static_cast<uint32_t>(std::lround(h / scale));
    WriteInt(kObjRectLeft, 0); WriteInt(kObjRectTop, 0);
    WriteInt(kObjRectWidth, static_cast<int>(g_uiW)); WriteInt(kObjRectHeight, static_cast<int>(g_uiH));
    Log("ui_scale: screen %dx%d, scale %.3f -> UI rect %ux%u (mode %d, enable_ui_scaling=%d, rt detour=%s, rect detour=%s)",
        w, h, scale, g_uiW, g_uiH, g_cfg.ui_scale_mode, *reinterpret_cast<uint8_t*>(g_engine.base + kObjEnable + kValueOffset),
        g_detourOk ? "on" : "off", g_rectDetourOk ? "on" : "off");
}

void PatchUIScale_Install() {
    if (!g_cfg.ui_scale_enabled()) { Log("ui_scale: off"); return; }
    if (!g_engine.knownBuild && !g_cfg.allow_unknown_build) { Log("ui_scale: skipped, engine build not verified"); return; }
    auto fn = reinterpret_cast<uint8_t*>(g_engine.base + kFnOverlaySize);
    if (memcmp(fn, kFnOverlaySizePrologue, 5) != 0) { Log("ui_scale: overlay-size helper prologue mismatch; patch NOT applied"); return; }
    if (g_cfg.ui_scale_mode == 1 && g_cfg.ui_scale_rt) {
        o_OverlaySize = reinterpret_cast<PFN_OverlaySize>(InstallDetour(fn, reinterpret_cast<void*>(my_OverlaySize), 5));
        g_detourOk = o_OverlaySize != nullptr;
        Log("ui_scale: overlay-size detour %s", g_detourOk ? "installed" : "FAILED");
    }
    if (g_cfg.ui_scale_mode == 2) {
        auto fr = reinterpret_cast<uint8_t*>(g_engine.base + kFnGetUiRect);
        uint32_t operand = 0; memcpy(&operand, fr + 5, 4);   // relocated absolute address of ui_rect_width's object
        if (memcmp(fr, kFnGetUiRectPrologue, 5) != 0 || operand != static_cast<uint32_t>(g_engine.base + kObjRectWidth)) {
            Log("ui_scale: GetUiRect prologue mismatch (operand 0x%08X); mode 2 NOT applied", operand); return;
        }
        o_GetUiRect = reinterpret_cast<PFN_GetUiRect>(InstallDetour(fr, reinterpret_cast<void*>(my_GetUiRect), 9));
        g_rectDetourOk = o_GetUiRect != nullptr;
        Log("ui_scale: GetUiRect caller-aware detour %s (layout and projection see the scaled rect; viewports and mouse see the screen)", g_rectDetourOk ? "installed" : "FAILED");
        if (g_rectDetourOk) InstallDragBoxHook();
    }
    Log("ui_scale: %s; rect objects currently %d,%d %dx%d", g_cfg.ui_scale_auto ? "auto (1080p-sized UI)" : "manual factor",
        ReadInt(kObjRectLeft), ReadInt(kObjRectTop), ReadInt(kObjRectWidth), ReadInt(kObjRectHeight));
}

void PatchUIScale_Remove() {
    if (g_boxHooked) { Hook_Remove(&g_boxSlot, 1); Log("ui_scale: drag box frames remapped %ld", g_boxFrames); }
}
