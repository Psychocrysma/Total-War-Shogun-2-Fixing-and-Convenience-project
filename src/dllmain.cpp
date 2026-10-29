// dinput8.dll proxy: forwards DirectInput to the real system DLL and installs the fixes.
#include "common.h"

EngineInfo g_engine;

namespace {

HMODULE g_self = nullptr;
HMODULE g_realDinput = nullptr;
std::wstring g_dir;   // folder containing this DLL (the game folder)

// --- real dinput8 forwarding ---------------------------------------------------------------
HMODULE RealDinput() {
    if (g_realDinput) return g_realDinput;
    wchar_t sys[MAX_PATH];
    UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring path(sys, n);
    path += L"\\dinput8.dll";
    g_realDinput = LoadLibraryW(path.c_str());
    if (!g_realDinput) Log("FATAL: could not load the system dinput8.dll (%lu)", GetLastError());
    return g_realDinput;
}

template <typename T> T Real(const char* name) {
    HMODULE m = RealDinput();
    return m ? reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(m, name))) : nullptr;
}

// --- engine detection ------------------------------------------------------------------------
bool FindEngine() {
    HMODULE m = GetModuleHandleW(kEngineName);
    if (!m) return false;
    auto base = reinterpret_cast<uint8_t*>(m);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    g_engine.module = m;
    g_engine.base = reinterpret_cast<uintptr_t>(m);
    g_engine.sizeOfImage = nt->OptionalHeader.SizeOfImage;
    g_engine.timestamp = nt->FileHeader.TimeDateStamp;
    g_engine.knownBuild = (g_engine.timestamp == kKnownEngineTimestamp);
    bool laa = (nt->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) != 0;
    Log("engine: %ls base=0x%08X size=0x%X timestamp=0x%08X (%s) LAA=%d", kEngineName,
        (unsigned)g_engine.base, g_engine.sizeOfImage, g_engine.timestamp,
        g_engine.knownBuild ? "known May 2023 build" : "UNKNOWN build: address-based patches disabled unless allow_unknown_build=1",
        laa ? 1 : 0);
    return true;
}

LONG g_installed = 0;

void InstallAll(const char* how) {
    if (InterlockedCompareExchange(&g_installed, 1, 0) != 0) return;   // once
    if (!FindEngine()) { Log("engine DLL not found at install time (%s); nothing patched", how); g_installed = 0; return; }
    Log("installing patches (%s)", how);
    PatchWindow_Install();
    PatchAgentChance_Install();
    PatchUIScale_Install();
    PatchCampaign_Install();
    PatchBattle_Install();
    PatchAltMove_Install();
    PatchAltInput_Install();
    PatchUIDiag_Install();
    Log("init complete");
}

// The launcher (shogun2.exe) loads the engine with LoadLibrary and enters it through
// GetProcAddress(engine, "entry_point"). At that moment the engine is fully loaded and its import
// table is final, so that is where the engine hooks are installed. Hooking the launcher's own
// GetProcAddress import is safe from DllMain: the launcher's imports were resolved long ago.
using PFN_GetProcAddress = FARPROC (WINAPI*)(HMODULE, LPCSTR);
PFN_GetProcAddress o_GetProcAddress = nullptr;

FARPROC WINAPI my_GetProcAddress(HMODULE module, LPCSTR name) {
    if (!g_installed) {
        HMODULE eng = GetModuleHandleW(kEngineName);
        if (eng && module == eng) {
            if (reinterpret_cast<uintptr_t>(name) > 0xFFFF) Log("launcher asked the engine for \"%s\"", name);
            InstallAll("launcher GetProcAddress");
        }
    }
    return o_GetProcAddress(module, name);
}

DWORD WINAPI FallbackThread(LPVOID) {
    // If the launcher never went through GetProcAddress (different launcher, injector, ...),
    // install from here once the engine is mapped. There is a small race with engine start-up in
    // that case, which the log will show.
    for (int i = 0; i < 300 && !g_installed; ++i) {
        Sleep(10);
        if (i >= 50 && GetModuleHandleW(kEngineName)) { InstallAll("fallback thread"); break; }
    }
    return 0;
}

void OnAttach(HMODULE self) {
    g_self = self;
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(self, path, MAX_PATH);
    g_dir = path;
    size_t slash = g_dir.find_last_of(L"\\/");
    g_dir = (slash == std::wstring::npos) ? L"." : g_dir.substr(0, slash);
    ConfigLoad(g_dir + L"\\tws2fix.ini");
    if (g_cfg.log) LogOpen(g_dir + L"\\tws2fix.log");
    Log("TWS2-Fixing-Project %s loaded (dinput8 proxy) from %ls", TWS2FIX_VERSION, g_dir.c_str());
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    Log("process: %ls", exe);
    // Pin this module for the life of the process. The launcher's import table and the engine's point
    // at code in here; if the loader unmapped this DLL during process shutdown before the launcher's own
    // exit code ran, a last call through those pointers would land in unmapped memory (seen as an
    // access violation at exit under Proton). A pinned module is only unmapped with the process.
    HMODULE pinned = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       reinterpret_cast<LPCWSTR>(&OnAttach), &pinned);
    HMODULE launcher = GetModuleHandleW(nullptr);
    o_GetProcAddress = (PFN_GetProcAddress)HookImport(launcher, "KERNEL32.dll", "GetProcAddress", (void*)my_GetProcAddress);
    Log("launcher GetProcAddress hook: %s%s", o_GetProcAddress ? "ok" : "not found (fallback thread only)", pinned ? " (module pinned)" : "");
    HANDLE t = CreateThread(nullptr, 0, FallbackThread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(inst); OnAttach(inst); }
    else if (reason == DLL_PROCESS_DETACH) {
        // Put the launcher's GetProcAddress import back so nothing routes through here after this.
        if (o_GetProcAddress) HookImport(GetModuleHandleW(nullptr), "KERNEL32.dll", "GetProcAddress", (void*)o_GetProcAddress);
        PatchAltMove_Remove();
        PatchUIDiag_Remove();
        PatchUIScale_Remove();
        PatchBattle_Remove();
        Log("unloading");
        LogClose();
    }
    return TRUE;
}

// --- exported DirectInput entry points (forwarded) --------------------------------------------
extern "C" {

HRESULT WINAPI DirectInput8Create(HINSTANCE inst, DWORD version, REFIID riid, LPVOID* out, LPUNKNOWN outer) {
    using F = HRESULT (WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    auto f = Real<F>("DirectInput8Create");
    HRESULT r = f ? f(inst, version, riid, out, outer) : E_FAIL;
    if (SUCCEEDED(r) && out) PatchAltInput_OnDirectInput(*out, riid);
    return r;
}

HRESULT WINAPI DllCanUnloadNow() {
    using F = HRESULT (WINAPI*)();
    auto f = Real<F>("DllCanUnloadNow");
    return f ? f() : S_FALSE;
}

HRESULT WINAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* out) {
    using F = HRESULT (WINAPI*)(REFCLSID, REFIID, LPVOID*);
    auto f = Real<F>("DllGetClassObject");
    return f ? f(rclsid, riid, out) : E_FAIL;
}

HRESULT WINAPI DllRegisterServer() {
    using F = HRESULT (WINAPI*)();
    auto f = Real<F>("DllRegisterServer");
    return f ? f() : E_FAIL;
}

HRESULT WINAPI DllUnregisterServer() {
    using F = HRESULT (WINAPI*)();
    auto f = Real<F>("DllUnregisterServer");
    return f ? f() : E_FAIL;
}

LPCVOID WINAPI GetdfDIJoystick() {
    using F = LPCVOID (WINAPI*)();
    auto f = Real<F>("GetdfDIJoystick");
    return f ? f() : nullptr;
}

} // extern "C"
