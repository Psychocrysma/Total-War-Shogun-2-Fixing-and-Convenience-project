// Input layer for the Alt group-move.
//
// In Shogun 2 the left button selects (a left drag is always the selection box) and move orders are on the
// right button: a right press on the ground starts the order, a right drag lays the formation with green
// ghosts, the release issues it. The block move (patch_altmove.cpp) reshapes that order, so the engine has
// to be in a right-button hold. While alt_group_move is on, a left press made with Alt held is therefore
// shown to the engine as a right press, for the whole hold, and Alt is hidden from the engine meanwhile
// (Alt + right button may have a meaning of its own). Everything else is passed through unchanged.
//
// The decision is made once per press (Alt held when the left button goes down) and lasts until the next
// press. The engine reads input three ways, all covered:
//   - DirectInput mouse and keyboard devices it creates through our DirectInput8Create proxy (vtable hooks
//     on IDirectInput8::CreateDevice and the devices' GetDeviceState / GetDeviceData; the two devices can
//     share one vtable, so every hook tells them apart by the device pointer),
//   - its USER32 imports GetAsyncKeyState / GetKeyboardState (import-table hooks),
//   - window messages to its main window (subclass).
// The block move reads the physical keys through this DLL's own GetAsyncKeyState import, which is not hooked.
#include "common.h"
#define DIRECTINPUT_VERSION 0x0800
#define INITGUID
#include <initguid.h>
#include <dinput.h>
#include <cstring>

namespace {

inline bool Phys(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// --- the per-press decision --------------------------------------------------------------------
bool g_swap = false;       // this press: left button shown to the engine as the right button
bool g_prevL = false;
long g_cHolds = 0;

void Tick() {
    if (!g_cfg.alt_group_move) return;
    bool l = Phys(VK_LBUTTON);
    if (l && !g_prevL) {
        g_swap = Phys(VK_MENU);
        if (g_swap) { g_cHolds++; if (g_cHolds <= 3) DLog("alt_input: Alt + left press -> shown to the engine as a right-button hold (%ld)", g_cHolds); }
    }
    g_prevL = l;
}
// Alt is hidden while a swapped hold is in progress.
bool HideAlt() { Tick(); return g_swap && Phys(VK_LBUTTON); }
bool Swapping() { Tick(); return g_swap; }

// --- USER32 imports of the engine ------------------------------------------------------------
using PFN_GetAsyncKeyState = SHORT (WINAPI*)(int);
using PFN_GetKeyboardState = BOOL (WINAPI*)(PBYTE);
PFN_GetAsyncKeyState o_GetAsyncKeyState = nullptr;
PFN_GetKeyboardState o_GetKeyboardState = nullptr;

SHORT WINAPI my_GetAsyncKeyState(int vk) {
    if ((vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU) && HideAlt()) return 0;
    if (Swapping()) {
        if (vk == VK_LBUTTON) return 0;
        if (vk == VK_RBUTTON) return static_cast<SHORT>(o_GetAsyncKeyState(VK_RBUTTON) | o_GetAsyncKeyState(VK_LBUTTON));
    }
    return o_GetAsyncKeyState(vk);
}
BOOL WINAPI my_GetKeyboardState(PBYTE state) {
    BOOL r = o_GetKeyboardState(state);
    if (r && state) {
        if (HideAlt()) { state[VK_MENU] = 0; state[VK_LMENU] = 0; state[VK_RMENU] = 0; }
        if (Swapping()) { state[VK_RBUTTON] |= state[VK_LBUTTON]; state[VK_LBUTTON] = 0; }
    }
    return r;
}

// --- DirectInput devices ---------------------------------------------------------------------
using PFN_CreateDevice   = HRESULT (WINAPI*)(IDirectInput8W*, REFGUID, LPDIRECTINPUTDEVICE8W*, LPUNKNOWN);
using PFN_GetDeviceState = HRESULT (WINAPI*)(IDirectInputDevice8W*, DWORD, LPVOID);
using PFN_GetDeviceData  = HRESULT (WINAPI*)(IDirectInputDevice8W*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
PFN_CreateDevice o_CreateDevice = nullptr;

// Up to four hooked vtables, each with its own originals (keyboard and mouse may or may not share one).
struct VtHook { void** vt; PFN_GetDeviceState state; PFN_GetDeviceData data; };
VtHook g_vt[4]; int g_vtCount = 0;
void* g_kbd = nullptr;     // the engine's keyboard device
void* g_mouse = nullptr;   // the engine's mouse device

const VtHook* FindVt(void* dev) {
    void** vt = *reinterpret_cast<void***>(dev);
    for (int i = 0; i < g_vtCount; i++) if (g_vt[i].vt == vt) return &g_vt[i];
    return nullptr;
}

bool SetVtableEntry(void** vtable, int index, void* fn, void** original) {
    DWORD old = 0;
    if (!VirtualProtect(&vtable[index], sizeof(void*), PAGE_READWRITE, &old)) return false;
    if (original) *original = vtable[index];
    vtable[index] = fn;
    DWORD tmp = 0; VirtualProtect(&vtable[index], sizeof(void*), old, &tmp);
    return true;
}

// Keyboard: what the engine last heard about Alt through events, so a hold can tell it "Alt up" when it
// begins and "Alt down" again when it ends with Alt still held.
bool g_engineAltDown = false;
bool g_hideWas = false;
DWORD g_seq = 0;

HRESULT WINAPI my_GetDeviceState(IDirectInputDevice8W* dev, DWORD size, LPVOID data) {
    const VtHook* h = FindVt(dev);
    if (!h) return DIERR_GENERIC;
    HRESULT r = h->state(dev, size, data);
    if (FAILED(r) || !data) return r;
    auto b = static_cast<uint8_t*>(data);
    if (dev == g_kbd && size == 256 && HideAlt()) { b[DIK_LMENU] = 0; b[DIK_RMENU] = 0; }
    if (dev == g_mouse && size >= sizeof(DIMOUSESTATE) && Swapping()) {
        uint8_t* btn = b + 12;                                    // rgbButtons
        btn[1] |= btn[0]; btn[0] = 0;
    }
    return r;
}

HRESULT WINAPI my_GetDeviceData(IDirectInputDevice8W* dev, DWORD objSize, LPDIDEVICEOBJECTDATA data, LPDWORD count, DWORD flags) {
    const VtHook* h = FindVt(dev);
    if (!h) return DIERR_GENERIC;
    DWORD capacity = count ? *count : 0;
    HRESULT r = h->data(dev, objSize, data, count, flags);
    if (FAILED(r) || !data || !count || objSize < sizeof(DIDEVICEOBJECTDATA) || (flags & DIGDD_PEEK)) return r;
    auto base = reinterpret_cast<uint8_t*>(data);
    if (dev == g_mouse) {
        if (Swapping())
            for (DWORD i = 0; i < *count; i++) {
                auto e = reinterpret_cast<DIDEVICEOBJECTDATA*>(base + i * objSize);
                if (e->dwOfs == DIMOFS_BUTTON0) e->dwOfs = DIMOFS_BUTTON1;
            }
        return r;
    }
    if (dev != g_kbd) return r;
    bool hide = HideAlt();
    DWORD w = 0;
    for (DWORD i = 0; i < *count; i++) {
        auto e = reinterpret_cast<DIDEVICEOBJECTDATA*>(base + i * objSize);
        if (e->dwOfs == DIK_LMENU || e->dwOfs == DIK_RMENU) {
            if (hide) continue;
            g_engineAltDown = (e->dwData & 0x80) != 0;
        }
        if (e->dwSequence > g_seq) g_seq = e->dwSequence;
        if (w != i) memcpy(base + w * objSize, e, objSize);
        w++;
    }
    bool want = !hide && Phys(VK_MENU);
    if ((hide != g_hideWas || !hide) && g_engineAltDown != want && w < capacity) {
        auto e = reinterpret_cast<DIDEVICEOBJECTDATA*>(base + w * objSize);
        memset(e, 0, objSize);
        e->dwOfs = DIK_LMENU; e->dwData = want ? 0x80 : 0; e->dwTimeStamp = GetTickCount(); e->dwSequence = ++g_seq;
        g_engineAltDown = want; w++;
    }
    g_hideWas = hide;
    *count = w;
    return r;
}

void HookDeviceVtable(void* dev, const char* what) {
    void** vt = *reinterpret_cast<void***>(dev);
    for (int i = 0; i < g_vtCount; i++) if (g_vt[i].vt == vt) { Log("alt_input: DirectInput %s device hooked (shares a vtable)", what); return; }
    if (g_vtCount >= 4) return;
    VtHook& h = g_vt[g_vtCount];
    void* a = nullptr; void* b = nullptr;
    if (!SetVtableEntry(vt, 9, (void*)my_GetDeviceState, &a) || !SetVtableEntry(vt, 10, (void*)my_GetDeviceData, &b)) return;
    h.vt = vt; h.state = (PFN_GetDeviceState)a; h.data = (PFN_GetDeviceData)b;
    g_vtCount++;
    Log("alt_input: DirectInput %s device hooked (GetDeviceState/GetDeviceData)", what);
}

HRESULT WINAPI my_CreateDevice(IDirectInput8W* di, REFGUID guid, LPDIRECTINPUTDEVICE8W* out, LPUNKNOWN outer) {
    HRESULT r = o_CreateDevice(di, guid, out, outer);
    if (SUCCEEDED(r) && out && *out) {
        if (IsEqualGUID(guid, GUID_SysKeyboard)) { g_kbd = *out; HookDeviceVtable(*out, "keyboard"); }
        else if (IsEqualGUID(guid, GUID_SysMouse)) { g_mouse = *out; HookDeviceVtable(*out, "mouse"); }
    }
    return r;
}

// --- window messages -------------------------------------------------------------------------------
WNDPROC o_WndProc = nullptr;
HWND g_subclassed = nullptr;
LRESULT CALLBACK my_WndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (HideAlt()) {
        if ((m == WM_SYSKEYDOWN || m == WM_SYSKEYUP || m == WM_KEYDOWN || m == WM_KEYUP) && (wp == VK_MENU || wp == VK_LMENU || wp == VK_RMENU)) return 0;
        if (m == WM_SYSKEYDOWN) { m = WM_KEYDOWN; lp &= ~(1 << 29); }
        if (m == WM_SYSKEYUP)   { m = WM_KEYUP;   lp &= ~(1 << 29); }
        if (m == WM_SYSCHAR) return 0;
    }
    if (Swapping()) {
        if (m == WM_LBUTTONDOWN) m = WM_RBUTTONDOWN;
        else if (m == WM_LBUTTONUP) m = WM_RBUTTONUP;
        else if (m == WM_LBUTTONDBLCLK) m = WM_RBUTTONDOWN;
        if (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST && (wp & MK_LBUTTON)) wp = (wp & ~MK_LBUTTON) | MK_RBUTTON;
    }
    return CallWindowProcW(o_WndProc, h, m, wp, lp);
}

} // namespace

// The block move asks whether the current hold is an Alt hold shown to the engine as a right hold.
bool PatchAltInput_SwapActive() { return g_swap; }

// Called from the window patch once the engine's main window exists.
void PatchAltInput_OnMainWindow(HWND hwnd) {
    if (!g_cfg.alt_group_move || !hwnd || g_subclassed) return;
    o_WndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(my_WndProc)));
    if (o_WndProc) { g_subclassed = hwnd; Log("alt_input: main window key and mouse messages filtered"); }
}

// Called from the DirectInput8Create proxy with the interface it returned.
void PatchAltInput_OnDirectInput(void* iface, REFIID riid) {
    if (!g_cfg.alt_group_move || !iface) return;
    if (!IsEqualIID(riid, IID_IDirectInput8W) && !IsEqualIID(riid, IID_IDirectInput8A)) return;
    void** vt = *reinterpret_cast<void***>(iface);
    static void** hooked = nullptr;
    if (vt == hooked) return;
    void* orig = nullptr;
    if (SetVtableEntry(vt, 3, (void*)my_CreateDevice, &orig)) { o_CreateDevice = (PFN_CreateDevice)orig; hooked = vt; Log("alt_input: IDirectInput8::CreateDevice hooked"); }
}

void PatchAltInput_Install() {
    if (!g_cfg.alt_group_move) return;
    HMODULE eng = g_engine.module;
    o_GetAsyncKeyState = (PFN_GetAsyncKeyState)HookImport(eng, "USER32.dll", "GetAsyncKeyState", (void*)my_GetAsyncKeyState);
    o_GetKeyboardState = (PFN_GetKeyboardState)HookImport(eng, "USER32.dll", "GetKeyboardState", (void*)my_GetKeyboardState);
    Log("alt_input: engine key-state imports hooked (GetAsyncKeyState=%s GetKeyboardState=%s); Alt + left hold is shown to the engine as a right hold",
        o_GetAsyncKeyState ? "ok" : "no", o_GetKeyboardState ? "ok" : "no");
}
