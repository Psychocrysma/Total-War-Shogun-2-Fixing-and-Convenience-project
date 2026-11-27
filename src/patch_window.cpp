// Window-related patches: borderless window and cursor clipping.
// Both work through the engine's import table (USER32 calls), so they need no engine addresses
// and apply to any build of empire.retail.dll.
#include "common.h"
#include <string>
#include <cstring>

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

// ---- s2fix (DudeCmonMan's d3d11.dll proxy) compatibility ------------------------------------------
// s2fix loads late (when the game creates its DirectX 11 device) and its borderless module installs only
// if the engine's import slots for SetWindowLongW, SetWindowPos, MoveWindow, PeekMessageW and GetClientRect
// still hold Windows' own functions. With s2fix installed (s2prof.dll + d3d11.dll in the game folder) and
// s2fix_compat=1 we leave those slots alone, give way to its borderless mode and cursor clip when its
// s2prof.ini has them on, and follow the game's resolution with a timer on the game window instead of the
// SetWindowPos / MoveWindow hooks. In DirectX 9 s2fix does not load; the cursor clip notices that (it
// stands down only while s2prof.dll is actually loaded), but our borderless stays off.
bool g_s2fixInstalled = false, g_s2fixBorderless = false, g_s2fixClip = false;
bool g_skipWindowHooks = false;          // leave SetWindowLongW / SetWindowPos / MoveWindow to s2fix
void** g_getClientRectSlot = nullptr;    // the engine's GetClientRect import slot (s2fix's hook once it loads)
WNDPROC o_WatchProc = nullptr;
constexpr UINT_PTR kResTimer = 0x7F2F;

void** FindImportSlot(HMODULE module, const char* importDll, const char* funcName) {
    auto base = reinterpret_cast<uint8_t*>(module);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (auto d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d) {
        if (_stricmp(reinterpret_cast<const char*>(base + d->Name), importDll) != 0) continue;
        auto names = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + d->OriginalFirstThunk);
        auto funcs = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + d->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++funcs) {
            if (IMAGE_SNAP_BY_ORDINAL32(names->u1.Ordinal)) continue;
            auto imp = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(imp->Name), funcName) == 0) return reinterpret_cast<void**>(&funcs->u1.Function);
        }
    }
    return nullptr;
}

void DetectS2fix() {
    wchar_t dir[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, dir, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    wchar_t* slash = wcsrchr(dir, L'\\'); if (slash) *slash = 0;
    std::wstring d(dir);
    bool host = GetFileAttributesW((d + L"\\s2prof.dll").c_str()) != INVALID_FILE_ATTRIBUTES;
    bool proxy = GetFileAttributesW((d + L"\\d3d11.dll").c_str()) != INVALID_FILE_ATTRIBUTES;
    if (!host || !proxy) return;
    g_s2fixInstalled = true;
    std::wstring ini = d + L"\\s2prof.ini";
    // s2fix's own defaults when its ini has no value: borderless on, clip_cursor off
    g_s2fixBorderless = GetPrivateProfileIntW(L"window", L"borderless", 1, ini.c_str()) != 0;
    g_s2fixClip = g_s2fixBorderless && GetPrivateProfileIntW(L"window", L"clip_cursor", 0, ini.c_str()) != 0;
}

// The game window's client size as the engine sees it: through the engine's own import slot, so with s2fix's
// borderless window it is the game's resolution, not the monitor's.
void FollowResolution(HWND hwnd) {
    RECT r = {0, 0, 0, 0};
    typedef BOOL (WINAPI *GcrFn)(HWND, LPRECT);
    GcrFn gcr = g_getClientRectSlot ? reinterpret_cast<GcrFn>(*g_getClientRectSlot) : GetClientRect;
    if (gcr(hwnd, &r)) PatchUIScale_OnResolution(r.right - r.left, r.bottom - r.top);
}
LRESULT CALLBACK WatchProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_TIMER && wp == kResTimer) { FollowResolution(hwnd); return 0; }
    if (msg == WM_SIZE) FollowResolution(hwnd);
    return CallWindowProcW(o_WatchProc, hwnd, msg, wp, lp);
}
void StartResolutionWatch(HWND hwnd) {
    o_WatchProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WatchProc)));
    if (o_WatchProc) SetTimer(hwnd, kResTimer, 500, nullptr);
    Log("s2fix_compat: following the game's resolution with a timer on the game window (%s)", o_WatchProc ? "ok" : "FAILED");
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
    if (main && hwnd && !g_mainWindow) {
        g_mainWindow = hwnd; Log("  main window = %p", hwnd);
        if (g_skipWindowHooks) StartResolutionWatch(hwnd);
        PatchAltInput_OnMainWindow(hwnd);
    }
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
        if (g_s2fixClip && GetModuleHandleW(L"s2prof.dll")) {   // s2fix is loaded and clips the cursor itself
            static bool said = false;
            if (!said) { said = true; Log("s2fix_compat: s2fix is loaded and clips the cursor; our cursor clip stands down"); }
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
    if (g_cfg.s2fix_compat) DetectS2fix();
    if (g_s2fixInstalled) {
        Log("s2fix_compat: s2fix found in the game folder (its borderless %s, its cursor clip %s)",
            g_s2fixBorderless ? "on" : "off", g_s2fixClip ? "on" : "off");
        if (g_s2fixBorderless && g_cfg.borderless) { g_cfg.borderless = false; Log("s2fix_compat: our borderless is off; s2fix's borderless mode is used"); }
        // Our borderless needs the three hooks; otherwise they are left to s2fix, whose borderless needs them untouched.
        g_skipWindowHooks = !g_cfg.borderless;
        if (g_skipWindowHooks) Log("s2fix_compat: SetWindowLongW, SetWindowPos and MoveWindow are left to s2fix");
        if (g_s2fixClip && g_cfg.cursor_clip) Log("s2fix_compat: our cursor clip gives way while s2fix is loaded (it clips the cursor itself)");
    }
    g_getClientRectSlot = FindImportSlot(eng, "USER32.dll", "GetClientRect");
    o_CreateWindowExW        = (PFN_CreateWindowExW)       HookImport(eng, "USER32.dll", "CreateWindowExW",        (void*)my_CreateWindowExW);
    if (!g_skipWindowHooks) {
        o_SetWindowLongW     = (PFN_SetWindowLongW)        HookImport(eng, "USER32.dll", "SetWindowLongW",         (void*)my_SetWindowLongW);
        o_SetWindowPos       = (PFN_SetWindowPos)          HookImport(eng, "USER32.dll", "SetWindowPos",           (void*)my_SetWindowPos);
        o_MoveWindow         = (PFN_MoveWindow)            HookImport(eng, "USER32.dll", "MoveWindow",             (void*)my_MoveWindow);
    }
    o_ChangeDisplaySettingsW = (PFN_ChangeDisplaySettingsW)HookImport(eng, "USER32.dll", "ChangeDisplaySettingsW", (void*)my_ChangeDisplaySettingsW);
    o_AdjustWindowRect       = (PFN_AdjustWindowRect)      HookImport(eng, "USER32.dll", "AdjustWindowRect",       (void*)my_AdjustWindowRect);
    Log("window hooks: CreateWindowExW=%s SetWindowLongW=%s SetWindowPos=%s MoveWindow=%s ChangeDisplaySettingsW=%s AdjustWindowRect=%s",
        o_CreateWindowExW ? "ok" : "MISSING", o_SetWindowLongW ? "ok" : (g_skipWindowHooks ? "left to s2fix" : "MISSING"), o_SetWindowPos ? "ok" : (g_skipWindowHooks ? "left to s2fix" : "MISSING"),
        o_MoveWindow ? "ok" : (g_skipWindowHooks ? "left to s2fix" : "MISSING"), o_ChangeDisplaySettingsW ? "ok" : "MISSING", o_AdjustWindowRect ? "ok" : "MISSING");
    Log("borderless=%d cursor_clip=%d", g_cfg.borderless ? 1 : 0, g_cfg.cursor_clip ? 1 : 0);
    if (g_cfg.cursor_clip) {
        HANDLE t = CreateThread(nullptr, 0, CursorClipThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
}
