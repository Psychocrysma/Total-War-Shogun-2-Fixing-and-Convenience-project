// Movement-range cell size (for enlarged campaign maps).
//
// The campaign movement map (campaign_maps/<map>/pathfinding.esf) is a grid whose cell size is stored in its
// header (grid+0x10, getter FUN_107ce380), 2.0 map units on CA's maps. The path search uses true distances
// and follows any cell size, but the code that floods the grid to draw the yellow movement-range area
// (FUN_107753c0, FUN_10776700, FUN_10777600, driven by FUN_107772f0) never reads the grid's cell size: it
// divides map offsets by a hard-coded 2.0 (fixed point 0x00200000 = 2.0 * 2^20), either pushed to the CRT
// 64-bit divide or stored as the divisor for FUN_1070c4b0. A map whose grid is rescaled to 4.0-unit cells
// (Expanded Japan k200 scale pack) therefore shows about twice the real reach.
//
// The fix writes the movement grid's real cell size (fixed point, value * 2^20) into all 42 of those
// immediates. Every site is checked as a whole instruction at start-up; on any mismatch nothing is written.
//
// movement_cell_size=auto (default): the value is taken from the map itself. The campaign loader
// (FUN_107520d0, reading campaign_maps/<map>/pathfinding.esf) builds each movement grid with the
// constructor FUN_107621f0 (thiscall, 3 stack arguments, ret 0xC; one caller, 0x10752532), which reads
// the grid's cell size from the file into grid+0x10. The constructor is wrapped: after it returns, the
// grid's cell size is written into the 42 sites if they hold a different value. Every campaign load
// rebuilds its grids, so vanilla maps get 2.0, a 4x map 4.0, and switching between them works.
// A number forces that cell size (2.0 vanilla, 4.0 k200, 3.0 k150, 2.44949 k122); 0 = off.
//
// Analysis and site list: "Shogun 2 DLL patch: movement-range overlay cell size for the 4x map". The bytes
// 00 00 20 00 occur 150 times in .text; the other uses (bit 21 polygon flags in the pathfinder, e.g.
// 0x1077b5ed) are not touched. Addresses are for empire.retail.dll of the May 2023 build.
#include "common.h"
#include "shaku/hook.h"
#include "shaku/gamemem.h"
#include <cmath>
#include <cstring>

namespace {

struct CellSite { uint32_t rva; int len; uint8_t bytes[11]; };
const CellSite kSites[] = {
    { 0x0077558B,  5, { 0x68, 0x00, 0x00, 0x20, 0x00 } },   // FUN_107753c0: push 0x200000
    { 0x007755A7,  5, { 0x68, 0x00, 0x00, 0x20, 0x00 } },   // FUN_107753c0: push 0x200000
    { 0x007756A7,  8, { 0xC7, 0x44, 0x24, 0x34, 0x00, 0x00, 0x20, 0x00 } },   // FUN_107753c0: mov dword ptr [esp + 0x34], 0x200000
    { 0x007757EA,  8, { 0xC7, 0x44, 0x24, 0x40, 0x00, 0x00, 0x20, 0x00 } },   // FUN_107753c0: mov dword ptr [esp + 0x40], 0x200000
    { 0x007758CD,  5, { 0x68, 0x00, 0x00, 0x20, 0x00 } },   // FUN_107753c0: push 0x200000
    { 0x007758E9,  5, { 0x68, 0x00, 0x00, 0x20, 0x00 } },   // FUN_107753c0: push 0x200000
    { 0x007759EC,  5, { 0x68, 0x00, 0x00, 0x20, 0x00 } },   // FUN_107753c0: push 0x200000
    { 0x00775A08,  5, { 0x68, 0x00, 0x00, 0x20, 0x00 } },   // FUN_107753c0: push 0x200000
    { 0x00775B04,  8, { 0xC7, 0x44, 0x24, 0x34, 0x00, 0x00, 0x20, 0x00 } },   // FUN_107753c0: mov dword ptr [esp + 0x34], 0x200000
    { 0x00775C4A, 11, { 0xC7, 0x84, 0x24, 0x88, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00 } },   // FUN_107753c0: mov dword ptr [esp + 0x88], 0x200000
    { 0x00775D34,  5, { 0x68, 0x00, 0x00, 0x20, 0x00 } },   // FUN_107753c0: push 0x200000
    { 0x00775D50,  5, { 0x68, 0x00, 0x00, 0x20, 0x00 } },   // FUN_107753c0: push 0x200000
    { 0x00775E56,  5, { 0x68, 0x00, 0x00, 0x20, 0x00 } },   // FUN_107753c0: push 0x200000
    { 0x00775E72,  5, { 0x68, 0x00, 0x00, 0x20, 0x00 } },   // FUN_107753c0: push 0x200000
    { 0x00775F71,  8, { 0xC7, 0x44, 0x24, 0x7C, 0x00, 0x00, 0x20, 0x00 } },   // FUN_107753c0: mov dword ptr [esp + 0x7c], 0x200000
    { 0x007760B7, 11, { 0xC7, 0x84, 0x24, 0x88, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00 } },   // FUN_107753c0: mov dword ptr [esp + 0x88], 0x200000
    { 0x0077618A,  5, { 0x68, 0x00, 0x00, 0x20, 0x00 } },   // FUN_107753c0: push 0x200000
    { 0x007761C1,  5, { 0x68, 0x00, 0x00, 0x20, 0x00 } },   // FUN_107753c0: push 0x200000
    { 0x007762C7,  5, { 0x68, 0x00, 0x00, 0x20, 0x00 } },   // FUN_107753c0: push 0x200000
    { 0x007762E3,  5, { 0x68, 0x00, 0x00, 0x20, 0x00 } },   // FUN_107753c0: push 0x200000
    { 0x007763E2,  8, { 0xC7, 0x44, 0x24, 0x7C, 0x00, 0x00, 0x20, 0x00 } },   // FUN_107753c0: mov dword ptr [esp + 0x7c], 0x200000
    { 0x00776528, 11, { 0xC7, 0x84, 0x24, 0x88, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00 } },   // FUN_107753c0: mov dword ptr [esp + 0x88], 0x200000
    { 0x007765F0,  5, { 0x68, 0x00, 0x00, 0x20, 0x00 } },   // FUN_107753c0: push 0x200000
    { 0x00776627,  5, { 0x68, 0x00, 0x00, 0x20, 0x00 } },   // FUN_107753c0: push 0x200000
    { 0x00776AE1,  5, { 0x68, 0x00, 0x00, 0x20, 0x00 } },   // FUN_10776700: push 0x200000
    { 0x00776AFD,  5, { 0x68, 0x00, 0x00, 0x20, 0x00 } },   // FUN_10776700: push 0x200000
    { 0x00776B4E,  5, { 0x68, 0x00, 0x00, 0x20, 0x00 } },   // FUN_10776700: push 0x200000
    { 0x00776B7F,  5, { 0x68, 0x00, 0x00, 0x20, 0x00 } },   // FUN_10776700: push 0x200000
    { 0x00776D38,  8, { 0xC7, 0x44, 0x24, 0x54, 0x00, 0x00, 0x20, 0x00 } },   // FUN_10776700: mov dword ptr [esp + 0x54], 0x200000
    { 0x00776FFB,  5, { 0x68, 0x00, 0x00, 0x20, 0x00 } },   // FUN_10776700: push 0x200000
    { 0x00777030,  5, { 0x68, 0x00, 0x00, 0x20, 0x00 } },   // FUN_10776700: push 0x200000
    { 0x007771D3,  8, { 0xC7, 0x44, 0x24, 0x74, 0x00, 0x00, 0x20, 0x00 } },   // FUN_10776700: mov dword ptr [esp + 0x74], 0x200000
    { 0x007778D6,  5, { 0x68, 0x00, 0x00, 0x20, 0x00 } },   // FUN_10777600: push 0x200000
    { 0x00777909,  5, { 0x68, 0x00, 0x00, 0x20, 0x00 } },   // FUN_10777600: push 0x200000
    { 0x007779F6,  5, { 0x68, 0x00, 0x00, 0x20, 0x00 } },   // FUN_10777600: push 0x200000
    { 0x00777A2B,  5, { 0x68, 0x00, 0x00, 0x20, 0x00 } },   // FUN_10777600: push 0x200000
    { 0x00777B15,  5, { 0x68, 0x00, 0x00, 0x20, 0x00 } },   // FUN_10777600: push 0x200000
    { 0x00777B48,  5, { 0x68, 0x00, 0x00, 0x20, 0x00 } },   // FUN_10777600: push 0x200000
    { 0x00777D1A,  5, { 0x68, 0x00, 0x00, 0x20, 0x00 } },   // FUN_10777600: push 0x200000
    { 0x00777D4D,  5, { 0x68, 0x00, 0x00, 0x20, 0x00 } },   // FUN_10777600: push 0x200000
    { 0x00777E2D,  5, { 0x68, 0x00, 0x00, 0x20, 0x00 } },   // FUN_10777600: push 0x200000
    { 0x00777E62,  5, { 0x68, 0x00, 0x00, 0x20, 0x00 } },   // FUN_10777600: push 0x200000
};
constexpr int kSiteCount = (int)(sizeof(kSites) / sizeof(kSites[0]));
constexpr uint32_t kVanilla = 0x00200000;   // 2.0 in 2^20 fixed point
constexpr uint32_t kMinCell = 0x00080000, kMaxCell = 0x02000000;   // 0.5 .. 32 map units

uint8_t* g_base = nullptr;
bool g_sitesOk = false;
uint32_t g_current = kVanilla;   // what the 42 sites hold now
uint32_t g_mapCell = kVanilla;   // the movement grid's cell size (from the map, or forced)
int g_grids = 0;

int ApplyCell(uint32_t imm) {
    if (!g_sitesOk || imm == g_current) return 0;
    int ok = 0;
    for (int i = 0; i < kSiteCount; i++)
        if (WriteCode(g_base + kSites[i].rva + kSites[i].len - 4, &imm, 4)) ok++;
    g_current = imm;
    return ok;
}

// --- auto: the grid constructor -------------------------------------------------------------
constexpr uint32_t RVA_GRID_CTOR = 0x007621F0;   // FUN_107621f0
const uint8_t kGridCtorHead[6] = { 0x81, 0xEC, 0xF8, 0x00, 0x00, 0x00 };   // sub esp, 0xF8
typedef void* (__fastcall* GridCtorFn)(void* self, void* edx, uint32_t a, void* b, void* c);
GridCtorFn o_GridCtor = nullptr;

void* __fastcall my_GridCtor(void* self, void* edx, uint32_t a, void* b, void* c) {
    void* r = o_GridCtor(self, edx, a, b, c);
    uint32_t cell = 0;
    memcpy(&cell, static_cast<uint8_t*>(self) + 0x10, 4);
    g_grids++;
    if (cell < kMinCell || cell > kMaxCell) {
        Log("movement_cell_size: grid %d has cell size 0x%08X (outside 0.5 .. 32); ignored", g_grids, cell);
        return r;
    }
    uint32_t before = g_current;
    g_mapCell = cell;
    int n = ApplyCell(cell);
    if (n) Log("movement_cell_size: map grid %d has %.4f-unit cells; movement-range overlay set to it (%d of %d sites, was %.4f)",
               g_grids, cell / 1048576.0, n, kSiteCount, before / 1048576.0);
    else if (g_grids <= 8) Log("movement_cell_size: map grid %d has %.4f-unit cells (overlay already matches)", g_grids, cell / 1048576.0);
    return r;
}

// --- the movement budget of the yellow area ---------------------------------------------------
// FUN_107e40f0 and FUN_107e4510 compute the movement-range area: they take the army's movement points
// (EBP / ESI, fetched through the army's vfunc +0x14 when the caller passes -1) and hand them, converted to
// float, to the area setup FUN_1075c2f0 and to the flood FUN_1071b100. The flood charges true map distance
// per step (FUN_1077ec90), so the points act as a distance budget directly; the actual move and the red
// path use a conversion tied to the movement grid that comes out at half that on a 4.0-unit grid. The
// points are scaled by 2.0 / cell size where both functions take them (push 1; movd xmm0, reg), so the
// area matches the real reach. 2.0-unit grids (CA's maps) give a factor of 1: nothing changes.
enum { B_RANGE1 = 0, B_RANGE2, B_COUNT };
const HookDef kBudget[B_COUNT] = {
    { "FUN_107e40f0+0x28A", "movement-range budget (EBP)", 0x007E437A, 6, { 0x6A, 0x01, 0x66, 0x0F, 0x6E, 0xC5 }, -1, false },
    { "FUN_107e4510+0x1D0", "movement-range budget (ESI)", 0x007E46E0, 6, { 0x6A, 0x01, 0x66, 0x0F, 0x6E, 0xC6 }, -1, false },
};
HookSlot g_budgetSlots[B_COUNT];
volatile LONG g_cBudget = 0;
int ScaleBudget(int points) {
    if (g_mapCell == kVanilla || points <= 0) return points;
    long long scaled = ((long long)points * kVanilla + g_mapCell / 2) / g_mapCell;
    InterlockedIncrement(&g_cBudget);
    if (g_cfg.movement_debug_log) { static LONG n = 0; if (InterlockedIncrement(&n) <= 20) Log("movedbg: movement-range budget %d -> %lld (cell %.4f)", points, scaled, g_mapCell / 1048576.0); }
    return (int)scaled;
}
extern "C" void __stdcall movecell_OnBudgetEbp(uint8_t* ctx) { int* r = reinterpret_cast<int*>(ctx + 0x0C); *r = ScaleBudget(*r); }
extern "C" void __stdcall movecell_OnBudgetEsi(uint8_t* ctx) { int* r = reinterpret_cast<int*>(ctx + 0x08); *r = ScaleBudget(*r); }
extern "C" uint8_t* movecell_BudgetTramps[B_COUNT];
uint8_t* movecell_BudgetTramps[B_COUNT] = {};
HOOK_DETOUR(movecell_B0, movecell_OnBudgetEbp, movecell_BudgetTramps, 0)
HOOK_DETOUR(movecell_B1, movecell_OnBudgetEsi, movecell_BudgetTramps, 1)
const HookDetour g_budgetDetours[B_COUNT] = { movecell_B0, movecell_B1 };

bool InstallBudget() {
    Mem_Init();
    for (int h = 0; h < B_COUNT; h++)
        if (!Hook_SiteMatches(g_base, kBudget[h])) { Log("movement_cell_size: bytes at RVA 0x%06X (%s) differ; range budget NOT scaled", kBudget[h].rva, kBudget[h].func); return false; }
    if (!Hook_Prepare(g_base, kBudget, B_COUNT, nullptr, movecell_BudgetTramps, g_budgetSlots)) { Log("movement_cell_size: trampoline memory; range budget NOT scaled"); return false; }
    Hook_Patch(g_budgetDetours, g_budgetSlots, B_COUNT);
    return true;
}

// --- diagnostics (movement_debug_log=1): which campaign pathfinding code runs, and who reads the cell size
enum { D_DRIVER = 0, D_DRIVER_CALLER, D_GETTER, D_COUNT };
const HookDef kDiag[D_COUNT] = {
    { "FUN_107772f0", "range flood driver",            0x007772F0, 6, { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8 }, -1, false },
    { "FUN_1081c850", "range flood driver's caller",   0x0081C850, 6, { 0x64, 0xA1, 0x2C, 0x00, 0x00, 0x00 }, -1, false },
    { "FUN_107ce380", "grid cell-size getter",         0x007CE380, 7, { 0x8B, 0x44, 0x24, 0x04, 0x8B, 0x49, 0x10 }, -1, false },
};
HookSlot g_diagSlots[D_COUNT];
volatile LONG g_cDriver = 0, g_cCaller = 0, g_cGetter = 0;
LONG g_lDriver = 0, g_lCaller = 0, g_lGetter = 0;
DWORD g_lastSummary = 0;
uint32_t g_getterCallers[64]; int g_getterCallerN = 0;

void Summary() {
    DWORD now = GetTickCount();
    if (now - g_lastSummary < 2000) return;
    LONG d = g_cDriver, c = g_cCaller, gt = g_cGetter;
    if (d == g_lDriver && c == g_lCaller && gt == g_lGetter) return;
    g_lastSummary = now;
    Log("movedbg: since the last line: range driver %ld calls, its caller %ld, cell-size getter %ld",
        d - g_lDriver, c - g_lCaller, gt - g_lGetter);
    g_lDriver = d; g_lCaller = c; g_lGetter = gt;
}
extern "C" void __stdcall movecell_OnDriver(const uint8_t*) { InterlockedIncrement(&g_cDriver); Summary(); }
extern "C" void __stdcall movecell_OnCaller(const uint8_t*) { InterlockedIncrement(&g_cCaller); Summary(); }
extern "C" void __stdcall movecell_OnGetter(const uint8_t* ctx) {
    InterlockedIncrement(&g_cGetter);
    uint32_t ret = *reinterpret_cast<const uint32_t*>(ctx + 0x24) - static_cast<uint32_t>(g_engine.base);
    uint32_t grid = *reinterpret_cast<const uint32_t*>(ctx + 0x1C);
    for (int i = 0; i < g_getterCallerN; i++) if (g_getterCallers[i] == ret) { Summary(); return; }
    if (g_getterCallerN < 64) g_getterCallers[g_getterCallerN++] = ret;
    uint32_t cell = 0; ReadDword(grid + 0x10, &cell);
    Log("movedbg: cell size read by engine code at RVA 0x%08X (grid %08X, cell %.4f)", ret, grid, cell / 1048576.0);
    Summary();
}
extern "C" uint8_t* movecell_Trampolines[D_COUNT];
uint8_t* movecell_Trampolines[D_COUNT] = {};
HOOK_DETOUR(movecell_D0, movecell_OnDriver, movecell_Trampolines, 0)
HOOK_DETOUR(movecell_D1, movecell_OnCaller, movecell_Trampolines, 1)
HOOK_DETOUR(movecell_D2, movecell_OnGetter, movecell_Trampolines, 2)
const HookDetour g_diagDetours[D_COUNT] = { movecell_D0, movecell_D1, movecell_D2 };

void InstallDiag() {
    Mem_Init();
    for (int h = 0; h < D_COUNT; h++)
        if (!Hook_SiteMatches(g_base, kDiag[h])) { Log("movedbg: bytes at RVA 0x%06X (%s) differ; diagnostics not installed", kDiag[h].rva, kDiag[h].func); return; }
    if (!Hook_Prepare(g_base, kDiag, D_COUNT, nullptr, movecell_Trampolines, g_diagSlots)) { Log("movedbg: trampoline memory; diagnostics not installed"); return; }
    Hook_Patch(g_diagDetours, g_diagSlots, D_COUNT);
    Log("movedbg: ON - logging campaign movement-range code activity and every reader of the grid cell size");
}

} // namespace

void PatchMoveCell_Install() {
    float cell = g_cfg.movement_cell_size;
    bool autoMode = g_cfg.movement_cell_auto;
    if (!autoMode && cell <= 0.0f) { Log("movement_cell_size: off (vanilla 2.0)"); return; }
    if (!g_engine.knownBuild && !g_cfg.allow_unknown_build) { Log("movement_cell_size: skipped, engine build not verified"); return; }
    g_base = reinterpret_cast<uint8_t*>(g_engine.base);
    for (int i = 0; i < kSiteCount; i++)
        if (memcmp(g_base + kSites[i].rva, kSites[i].bytes, kSites[i].len) != 0) {
            Log("movement_cell_size: bytes at RVA 0x%06X differ from the expected build; patch NOT applied", kSites[i].rva); return;
        }
    g_sitesOk = true;
    if (g_cfg.movement_debug_log) InstallDiag();
    bool budgetOk = InstallBudget();
    if (autoMode) {
        if (memcmp(g_base + RVA_GRID_CTOR, kGridCtorHead, sizeof(kGridCtorHead)) != 0) { Log("movement_cell_size: grid constructor differs; auto NOT applied"); return; }
        o_GridCtor = reinterpret_cast<GridCtorFn>(InstallDetour(g_base + RVA_GRID_CTOR, reinterpret_cast<void*>(my_GridCtor), sizeof(kGridCtorHead)));
        Log("movement_cell_size: auto %s - the movement-range area follows the cell size of each campaign map's pathfinding grid (budget scaling %s)",
            o_GridCtor ? "ON" : "FAILED (grid constructor hook)", budgetOk ? "on" : "off");
        return;
    }
    if (cell < 0.5f || cell > 32.0f) { Log("movement_cell_size: %.3f is out of range (0.5 .. 32); patch NOT applied", cell); return; }
    uint32_t imm = static_cast<uint32_t>(std::lround(cell * 1048576.0));
    if (imm == kVanilla) { Log("movement_cell_size: 2.0 is the game's own value; nothing to do"); return; }
    g_mapCell = imm;
    int ok = ApplyCell(imm);
    Log("movement_cell_size: fixed - movement-range overlay uses %.4f-unit cells (0x%08X) at %d of %d sites%s",
        cell, imm, ok, kSiteCount, ok == kSiteCount ? "" : " - WRITE FAILED at some sites");
}
