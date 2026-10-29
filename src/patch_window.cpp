// Window-related patches: borderless window and cursor clipping.
// Both work through the engine's import table (USER32 calls), so they need no engine addresses
// and apply to any build of empire.retail.dll.
#include "common.h"

namespace {

using PFN_CreateWindowExW       = HWND (WINAPI*)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
using PFN_SetWindowLongW        = LONG (WINAPI*)(HWND, int, LONG);
using PFN_SetWindowPos          = BOOL (WINAPI*)(HWND, HWND, int, int, int, int, UINT);
using PFN_MoveWindow            = BOOL (WINAPI*)(HWND, int, int, int, int, BOOL);
using PFN_ChangeDisplaySettingsW= LONG (WINAPI*)(DEVMODEW*, DWORD);
using PFN_AdjustWindowRect      = BOOL (WINAPI*)(LPRECT, DWORD, BOOL);

PFN_CreateWindowExW        o_CreateWindowExW = nullptr;
PFN_SetWindowLongW         o_SetWindowLongW = nullptr;
PFN_SetWindowPos           o_SetWindowPos = nullptr;
PFN_MoveWindow             o_MoveWindow = nullptr;
PFN_ChangeDisplaySettingsW o_ChangeDisplaySettingsW = nullptr;
PFN_AdjustWindowRect       o_AdjustWindowRect = nullptr;

HWND  g_mainWindow = nullptr;     // the engine's main top-level window, once created
bool  g_borderlessApplied = false;
RECT  g_monitorRect = {0, 0, 0, 0};

constexpr DWORD kDecorations = WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU | WS_BORDER | WS_DLGFRAME;

bool LooksLikeMainWindow(HWND parent, DWORD style, int w, int h) {
    if (parent != nullptr) return false;
    if (style & WS_CHILD) return false;
    if (w < 320 || h < 240) return false;   // splash/helper windows
    return true;
}

RECT MonitorRectFor(int x, int y) {
    POINT pt = {x, y};
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {sizeof(mi)};
    if (mon && GetMonitorInfoW(mon, &mi)) return mi.rcMonitor;
    RECT r = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    return r;
}

std::string Narrow(LPCWSTR s) {
    if (!s) return "(null)";
    if (reinterpret_cast<uintptr_t>(s) < 0x10000) { char b[32]; snprintf(b, sizeof b, "(atom %u)", (unsigned)(uintptr_t)s); return b; }
    char buf[256]; WideCharToMultiByte(CP_UTF8, 0, s, -1, buf, sizeof buf, nullptr, nullptr); return buf;
}

HWND WINAPI my_CreateWindowExW(DWORD exStyle, LPCWSTR cls, LPCWSTR name, DWORD style, int x, int y, int w, int h,
                                HWND parent, HMENU menu, HINSTANCE inst, LPVOID param) {
    Log("CreateWindowExW class=%s name=%s style=0x%08lX ex=0x%08lX pos=(%d,%d) size=%dx%d parent=%p",
        Narrow(cls).c_str(), Narrow(name).c_str(), style, exStyle, x, y, w, h, parent);
    bool main = LooksLikeMainWindow(parent, style, w, h);
    int reqW = w, reqH = h;   // the resolution the engine asked for, before any borderless conversion
    if (main && g_cfg.borderless && (style & WS_CAPTION)) {
        // Windowed mode with a frame: turn it into a borderless window covering the monitor.
        g_monitorRect = MonitorRectFor(x, y);
        style = (style & ~kDecorations & ~WS_OVERLAPPED) | WS_POPUP;
        exStyle &= ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE);
        x = g_monitorRect.left; y = g_monitorRect.top;
        w = g_monitorRect.right - g_monitorRect.left; h = g_monitorRect.bottom - g_monitorRect.top;
        g_borderlessApplied = true;
        Log("  -> borderless: style=0x%08lX pos=(%d,%d) size=%dx%d", style, x, y, w, h);
    }
    HWND hwnd = o_CreateWindowExW(exStyle, cls, name, style, x, y, w, h, parent, menu, inst, param);
    if (main && hwnd && !g_mainWindow) { g_mainWindow = hwnd; Log("  main window = %p", hwnd); PatchAltInput_OnMainWindow(hwnd); }
    if (main && hwnd) {
        RECT cr = {0, 0, 0, 0};
        if (!(style & WS_CAPTION) || g_borderlessApplied) { cr.right = reqW; cr.bottom = reqH; }   // popup/fullscreen: size is the client size
        else if (!GetClientRect(hwnd, &cr)) { cr.right = reqW; cr.bottom = reqH; }
        PatchUIScale_OnResolution(cr.right, cr.bottom);
    }
    return hwnd;
}

LONG WINAPI my_SetWindowLongW(HWND hwnd, int index, LONG value) {
    if (g_borderlessApplied && hwnd == g_mainWindow && index == GWL_STYLE) {
        LONG v = (value & ~kDecorations & ~WS_OVERLAPPED) | WS_POPUP;
        if (v != value) Log("SetWindowLongW(GWL_STYLE) 0x%08lX -> 0x%08lX (borderless kept)", value, v);
        value = v;
    }
    return o_SetWindowLongW(hwnd, index, value);
}

BOOL WINAPI my_SetWindowPos(HWND hwnd, HWND after, int x, int y, int cx, int cy, UINT flags) {
    if (hwnd == g_mainWindow && !(flags & SWP_NOSIZE) && !g_borderlessApplied) PatchUIScale_OnResolution(cx, cy);
    if (g_borderlessApplied && hwnd == g_mainWindow) {
        int mw = g_monitorRect.right - g_monitorRect.left, mh = g_monitorRect.bottom - g_monitorRect.top;
        if (!(flags & SWP_NOSIZE) && (cx != mw || cy != mh)) { Log("SetWindowPos size %dx%d -> %dx%d", cx, cy, mw, mh); cx = mw; cy = mh; }
        if (!(flags & SWP_NOMOVE) && (x != g_monitorRect.left || y != g_monitorRect.top)) { x = g_monitorRect.left; y = g_monitorRect.top; }
    }
    return o_SetWindowPos(hwnd, after, x, y, cx, cy, flags);
}

BOOL WINAPI my_MoveWindow(HWND hwnd, int x, int y, int w, int h, BOOL repaint) {
    if (hwnd == g_mainWindow && !g_borderlessApplied) PatchUIScale_OnResolution(w, h);
    if (g_borderlessApplied && hwnd == g_mainWindow) {
        Log("MoveWindow (%d,%d %dx%d) -> monitor rect", x, y, w, h);
        x = g_monitorRect.left; y = g_monitorRect.top;
        w = g_monitorRect.right - g_monitorRect.left; h = g_monitorRect.bottom - g_monitorRect.top;
    }
    return o_MoveWindow(hwnd, x, y, w, h, repaint);
}

LONG WINAPI my_ChangeDisplaySettingsW(DEVMODEW* mode, DWORD flags) {
    if (mode) Log("ChangeDisplaySettingsW %lux%lu @%luHz flags=0x%lX", mode->dmPelsWidth, mode->dmPelsHeight, mode->dmDisplayFrequency, flags);
    else      Log("ChangeDisplaySettingsW(restore) flags=0x%lX", flags);
    return o_ChangeDisplaySettingsW(mode, flags);
}

BOOL WINAPI my_AdjustWindowRect(LPRECT r, DWORD style, BOOL menu) {
    if (g_cfg.borderless && (style & WS_CAPTION)) {
        // The engine sizes its frame from the client size; with no frame the rect stays the client rect.
        return TRUE;
    }
    return o_AdjustWindowRect(r, style, menu);
}

// ---- cursor clipping --------------------------------------------------------------------------
BOOL CALLBACK FindMainWindowProc(HWND hwnd, LPARAM lparam) {
    DWORD pid = 0; GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    if (!IsWindowVisible(hwnd) || GetParent(hwnd)) return TRUE;
    RECT r; GetClientRect(hwnd, &r);
    if (r.right < 320 || r.bottom < 240) return TRUE;
    *reinterpret_cast<HWND*>(lparam) = hwnd;
    return FALSE;
}

DWORD WINAPI CursorClipThread(LPVOID) {
    bool clipped = false;
    DWORD lastSearch = 0;
    for (;;) {
        Sleep(50);
        HWND hwnd = g_mainWindow;
        if (!hwnd || !IsWindow(hwnd)) {
            DWORD now = GetTickCount();
            if (now - lastSearch > 1000) { lastSearch = now; HWND found = nullptr; EnumWindows(FindMainWindowProc, reinterpret_cast<LPARAM>(&found)); if (found) { g_mainWindow = found; Log("cursor clip: found main window %p by enumeration", found); } }
            if (clipped) { ClipCursor(nullptr); clipped = false; }
            continue;
        }
        bool active = GetForegroundWindow() == hwnd && !IsIconic(hwnd);
        if (active) {
            RECT r; GetClientRect(hwnd, &r);
            POINT tl = {r.left, r.top}, br = {r.right, r.bottom};
            ClientToScreen(hwnd, &tl); ClientToScreen(hwnd, &br);
            RECT clip = {tl.x, tl.y, br.x, br.y};
            ClipCursor(&clip);
            clipped = true;
        } else if (clipped) {
            ClipCursor(nullptr);
            clipped = false;
        }
    }
    return 0;
}

} // namespace

bool IsEngineAddress(const void* p) {
    auto v = reinterpret_cast<uintptr_t>(p);
    return g_engine.base && v >= g_engine.base && v < g_engine.base + g_engine.sizeOfImage;
}

void PatchWindow_Install() {
    HMODULE eng = g_engine.module;
    o_CreateWindowExW        = (PFN_CreateWindowExW)       HookImport(eng, "USER32.dll", "CreateWindowExW",        (void*)my_CreateWindowExW);
    o_SetWindowLongW         = (PFN_SetWindowLongW)        HookImport(eng, "USER32.dll", "SetWindowLongW",         (void*)my_SetWindowLongW);
    o_SetWindowPos           = (PFN_SetWindowPos)          HookImport(eng, "USER32.dll", "SetWindowPos",           (void*)my_SetWindowPos);
    o_MoveWindow             = (PFN_MoveWindow)            HookImport(eng, "USER32.dll", "MoveWindow",             (void*)my_MoveWindow);
    o_ChangeDisplaySettingsW = (PFN_ChangeDisplaySettingsW)HookImport(eng, "USER32.dll", "ChangeDisplaySettingsW", (void*)my_ChangeDisplaySettingsW);
    o_AdjustWindowRect       = (PFN_AdjustWindowRect)      HookImport(eng, "USER32.dll", "AdjustWindowRect",       (void*)my_AdjustWindowRect);
    Log("window hooks: CreateWindowExW=%s SetWindowLongW=%s SetWindowPos=%s MoveWindow=%s ChangeDisplaySettingsW=%s AdjustWindowRect=%s",
        o_CreateWindowExW ? "ok" : "MISSING", o_SetWindowLongW ? "ok" : "MISSING", o_SetWindowPos ? "ok" : "MISSING",
        o_MoveWindow ? "ok" : "MISSING", o_ChangeDisplaySettingsW ? "ok" : "MISSING", o_AdjustWindowRect ? "ok" : "MISSING");
    Log("borderless=%d cursor_clip=%d", g_cfg.borderless ? 1 : 0, g_cfg.cursor_clip ? 1 : 0);
    if (g_cfg.cursor_clip) {
        HANDLE t = CreateThread(nullptr, 0, CursorClipThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
}
