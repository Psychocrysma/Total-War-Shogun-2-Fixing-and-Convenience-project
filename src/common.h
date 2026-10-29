// TWS2-Fixing-Project - engine fix library for Total War: SHOGUN 2 (original, 32-bit, May 2023 build)
// Shared declarations. No CA code is contained in or derived into this project; the library
// loads beside the game and patches engine behaviour in memory at run time.
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <unknwn.h>
#include <cstdint>
#include <cstdio>
// Diagnostic log line, written only when alt_debug_log=1 (Alt group-move testing).
#define DLog(...) do { if (g_cfg.alt_debug_log) Log(__VA_ARGS__); } while (0)
#include <string>

#define TWS2FIX_VERSION "0.8.0"

// ---- logging ----------------------------------------------------------------
void LogOpen(const std::wstring& path);
void Log(const char* fmt, ...);
void LogClose();

// ---- configuration (tws2fix.ini next to the DLL) --------------------------------
struct Config {
    bool log = true;
    bool cursor_clip = true;         // keep the mouse inside the game window while it is active
    bool borderless = false;         // windowed mode -> borderless window covering the monitor
    bool allow_unknown_build = false;// apply address-based patches even if the engine build differs
    bool agent_chance_clamp = true;  // fix the unsigned 5..95 clamp in agent action chances (the 95% bug)
    float ui_scale = 0.0f;           // >1.0 enlarges the UI by that factor (0 = off); ignored when ui_scale_auto
    bool  ui_scale_auto = true;      // "auto": UI the size it has at 1920x1080, whatever the resolution (off at <=1080p)
    bool ui_scale_rt = true;         // mode 1 only: also detour the overlay-target size helper
    int  ui_scale_mode = 2;          // 1 = sub-rect + overlay resize (draws 1:1 top-left), 2 = full-screen viewport over the scaled rect
    bool ui_scale_enabled() const { return ui_scale_auto || ui_scale > 1.001f; }
    bool ui_scale_log_callers = false;     // diagnostics: log each distinct caller of the UI rect getter once
    bool ui_scale_log_box = false;         // diagnostics: log who shows the battle drag-selection box
    uint32_t ui_scale_extra_callers[16] = {}; // diagnostics: extra caller RVAs that should see the scaled rect
    int ui_scale_extra_count = 0;
    float settlement_box = 1000.0f;  // half-size of the settlement render box in map units (vanilla 350; 0 = leave it)
    int   vram_mb = 0;               // >0: tell the engine it has this much video memory (max 4095)
    // [battle] section: ShakuEight's battle fixes (src/shaku/). Single player only.
    bool battle_fixes = true;        // master switch
    bool fix_path = true;            // pathfix: units through gates / doorways / corners
    bool fix_artillery = true;       // artillery aims at the formation centre
    bool fix_fire_by_rank = true;    // fire-by-rank first volley delay, Hattori/Ikko gun units, ranks beyond the third
    bool fix_siege = true;           // siege responsiveness (defenders answer orders on the walls)
    bool fix_wall_manning = true;    // units go straight onto the wall section
    bool fix_zombie_yari = true;     // broken yari wall unit runs instead of standing
    bool fix_wall_collapse = true;   // gatehouse men not killed at Start Battle by open gates
    bool fix_single_unit_group = true; // one unit can form a group
    bool fix_guard_mode = true;      // missile units in guard mode hold position / retarget
    bool fix_kneel_fire = true;      // Kneel Fire units take move orders
    bool alt_group_move = true;      // Alt + hold LMB moves the selection as a block; Ctrl while holding rotates
    bool alt_debug_log = false;      // per-hold diagnostics of the Alt group-move (traces, measurements)
    int  alt_rotate_speed = 64;      // rotation per pixel of mouse travel, in 1/65536 turn units
};
extern Config g_cfg;
void ConfigLoad(const std::wstring& iniPath);

// ---- engine module ------------------------------------------------------------
struct EngineInfo {
    HMODULE module = nullptr;
    uintptr_t base = 0;
    uint32_t sizeOfImage = 0;
    uint32_t timestamp = 0;
    bool knownBuild = false;         // timestamp matches the build this library was verified against
};
extern EngineInfo g_engine;
constexpr uint32_t kKnownEngineTimestamp = 0x645E92BBu;   // empire.retail.dll, 2023-05-12 19:25:47 UTC
constexpr const wchar_t* kEngineName = L"empire.retail.dll";

// ---- hooking helpers (hooks.cpp) ----------------------------------------------
// Replace an import-table entry of `module` for `importDll!funcName` with `replacement`.
// Returns the original function pointer, or nullptr if the import was not found.
void* HookImport(HMODULE module, const char* importDll, const char* funcName, void* replacement);
// Write bytes into code; returns true on success.
bool WriteCode(void* address, const void* bytes, size_t len);
// Install a 5-byte relative jump detour at `target` to `hook`. `stolenLen` bytes at target are copied
// into a trampoline (must be >= 5 and end on an instruction boundary; caller verifies).
void* InstallDetour(void* target, void* hook, size_t stolenLen);

// ---- patches ------------------------------------------------------------------
void PatchWindow_Install();      // borderless + cursor clip (IAT hooks + watcher thread)
void PatchAgentChance_Install(); // signed clamp for agent action success chance (engine bytes)
void PatchUIScale_Install();     // UI scaling via the engine's UI rect (engine bytes + detour)
void PatchUIScale_OnResolution(int w, int h);
void PatchCampaign_Install();    // settlement render box, video memory override (engine bytes)
void PatchBattle_Install();      // ShakuEight's battle fixes (src/shaku/fix_*.cpp), per [battle] keys
void PatchBattle_Remove();
void PatchAltMove_Install();     // Alt group-move (src/patch_altmove.cpp)
void PatchAltMove_Remove();
void PatchAltInput_Install();
void PatchAltInput_OnMainWindow(HWND hwnd);
bool PatchAltInput_SwapActive();
void PatchUIDiag_Install();
void PatchUIDiag_Remove();
void PatchUIScale_Remove();    // hides Alt from the engine while the left button is held (src/patch_altinput.cpp)
void PatchAltInput_OnDirectInput(void* iface, REFIID riid);
bool IsEngineAddress(const void* p);
