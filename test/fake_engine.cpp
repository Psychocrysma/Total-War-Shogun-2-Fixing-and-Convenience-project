#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dinput.h>
#include <cstdio>
extern "C" __declspec(dllexport) int RunEngine() {
    IDirectInput8W* di = nullptr;
    HRESULT hr = DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W, (void**)&di, nullptr);
    printf("fake engine: DirectInput8Create hr=0x%08lX\n", hr);
    WNDCLASSEXW wc = {sizeof(wc)}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"S2TestClass";
    RegisterClassExW(&wc);
    RECT r = {0,0,1280,720}; AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND w = CreateWindowExW(0, L"S2TestClass", L"Total War: SHOGUN 2 (test)", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, r.right-r.left, r.bottom-r.top, nullptr, nullptr, wc.hInstance, nullptr);
    LONG st = GetWindowLongW(w, GWL_STYLE); RECT wr; GetWindowRect(w, &wr);
    printf("fake engine: window %p style=0x%08lX rect=(%ld,%ld)-(%ld,%ld)\n", w, st, wr.left, wr.top, wr.right, wr.bottom);
    SetWindowLongW(w, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
    SetWindowPos(w, nullptr, 50, 50, 800, 600, SWP_NOZORDER);
    GetWindowRect(w, &wr); st = GetWindowLongW(w, GWL_STYLE);
    printf("fake engine: after resize style=0x%08lX rect=(%ld,%ld)-(%ld,%ld)\n", st, wr.left, wr.top, wr.right, wr.bottom);
    Sleep(400);
    return 0;
}
BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID){ return TRUE; }
