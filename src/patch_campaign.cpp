// Campaign-map and renderer limits.
//
// 1. Settlement render box. The campaign display builds a quadtree for settlement/building models with
//    fixed world bounds (-350,-10,-350)..(350,100,350). Models placed outside that box never enter the
//    tree and are never drawn; flags and units use other containers and still appear. Map mods larger
//    than the vanilla extent lose their settlement models past +-350. The patch rewrites the six bounds
//    before the constructor runs: three floats in a 16-byte vector in .rdata and one immediate in code.
//    The campaign tree display ("Campaign Tree Display", FUN_10834ba0) has the same limit: it hands its
//    spatial tree (FUN_108354a0 -> FUN_108b7a10) the half-size 350, from which the box
//    (-350,0,-350)..(350,0,350) is made (the minimum is the value with the sign bit flipped). Trees past
//    +-350 are never drawn. The same setting rewrites that immediate.
//
// 2. Video memory. The renderer computes its texture budget from a total it gets from the device, unless
//    the debug variable force_total_video_memory (bytes) is non-zero, in which case that value is used
//    instead. A 32-bit process is typically told ~2.9 GB regardless of the card; this writes the
//    configured figure into the variable before the device is created.
//
// Addresses are for empire.retail.dll of the May 2023 build (base 0x10000000).
#include "common.h"
#include <cstring>

namespace {

// --- settlement box ----------------------------------------------------------------------------
constexpr uint32_t kBoxVectorRva   = 0x0162A100;   // .rdata: float4 {-350, -10, -350, 350} = {minX, minY, minZ, maxX}
constexpr uint32_t kBoxMaxZImmRva  = 0x0082D537;   // code: mov dword ptr [esp+0x38], 0x43AF0000 (350.0f) = maxZ
constexpr uint32_t kBoxMaxYImmRva  = 0x0082D52F;   // code: mov dword ptr [esp+0x34], 0x42C80000 (100.0f) = maxY (left alone)
const uint8_t kBoxMaxZImmBytes[8] = {0xC7, 0x44, 0x24, 0x38, 0x00, 0x00, 0xAF, 0x43};
constexpr uint32_t kTreeBoxImmRva  = 0x00834BBB;   // code in FUN_10834ba0: mov dword ptr [esp], 0x43AF0000 (350.0f)
const uint8_t kTreeBoxImmBytes[7] = {0xC7, 0x04, 0x24, 0x00, 0x00, 0xAF, 0x43};

// --- video memory ------------------------------------------------------------------------------
constexpr uint32_t kObjForceTotalVideoMemory = 0x01C9BEF0;   // debug variable object; value (u32 bytes) at +0x5C
constexpr uint32_t kValueOffset = 0x5C;

bool WriteData(void* address, const void* bytes, size_t len) {
    DWORD old = 0;
    if (!VirtualProtect(address, len, PAGE_READWRITE, &old)) return false;
    memcpy(address, bytes, len);
    DWORD tmp = 0; VirtualProtect(address, len, old, &tmp);
    return true;
}

} // namespace

void PatchCampaign_Install() {
    // Settlement box
    if (g_cfg.settlement_box > 0.0f) {
        if (!g_engine.knownBuild && !g_cfg.allow_unknown_build) { Log("settlement_box: skipped, engine build not verified"); }
        else {
            auto vec = reinterpret_cast<float*>(g_engine.base + kBoxVectorRva);
            auto imm = reinterpret_cast<uint8_t*>(g_engine.base + kBoxMaxZImmRva);
            if (vec[0] != -350.0f || vec[1] != -10.0f || vec[2] != -350.0f || vec[3] != 350.0f || memcmp(imm, kBoxMaxZImmBytes, 8) != 0) {
                Log("settlement_box: constants do not match the expected build; patch NOT applied");
            } else {
                float s = g_cfg.settlement_box;
                float newVec[4] = {-s, -10.0f, -s, s};
                uint8_t newImm[8]; memcpy(newImm, kBoxMaxZImmBytes, 8); memcpy(newImm + 4, &s, 4);
                bool ok = WriteData(vec, newVec, 16) && WriteCode(imm, newImm, 8);
                Log("settlement_box: bounds set to +-%.0f map units (%s)", s, ok ? "ok" : "WRITE FAILED");
            }
            auto timm = reinterpret_cast<uint8_t*>(g_engine.base + kTreeBoxImmRva);
            if (memcmp(timm, kTreeBoxImmBytes, 7) != 0) {
                Log("tree_box: constant does not match the expected build; patch NOT applied");
            } else {
                float s = g_cfg.settlement_box;
                uint8_t newImm[7]; memcpy(newImm, kTreeBoxImmBytes, 7); memcpy(newImm + 3, &s, 4);
                bool ok = WriteCode(timm, newImm, 7);
                Log("tree_box: campaign tree bounds set to +-%.0f map units (%s)", s, ok ? "ok" : "WRITE FAILED");
            }
        }
    } else Log("settlement_box: off (vanilla +-350)");

    // Video memory
    if (g_cfg.vram_mb > 0) {
        if (!g_engine.knownBuild && !g_cfg.allow_unknown_build) { Log("vram_mb: skipped, engine build not verified"); }
        else {
            uint32_t mb = g_cfg.vram_mb > 4095 ? 4095u : static_cast<uint32_t>(g_cfg.vram_mb);
            uint32_t bytes = mb * 1024u * 1024u;
            *reinterpret_cast<uint32_t*>(g_engine.base + kObjForceTotalVideoMemory + kValueOffset) = bytes;
            Log("vram_mb: force_total_video_memory = %u MB (%u bytes)", mb, bytes);
        }
    } else Log("vram_mb: off (engine detection)");
}
