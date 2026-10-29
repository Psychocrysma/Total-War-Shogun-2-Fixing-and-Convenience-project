// Agent action success chance: signed/unsigned clamp bug.
//
// Each agent action (assassinate, sabotage, apprehend, bribe, incite, ...) has a function in the engine
// that adds up the agent's skill, the target's defence and the modifiers, then clamps the result to
// 5..95 percent. Nine of the fourteen such functions treat the total as *unsigned*, so a negative total
// (a weak agent against a strong target) wraps to a huge number and the upper clamp returns 95 instead
// of 5. That is the "level-1 metsuke has 95% against a level-6 monk" bug. The other five functions use
// signed comparisons and behave correctly.
//
// The fix changes two opcodes in each affected function:
//   jae  (0x73) -> jge   (0x7D)      lower clamp
//   cmova (0F 47) -> cmovg (0F 4F)   upper clamp
// Addresses are for empire.retail.dll of the May 2023 build (base 0x10000000); the bytes at every site
// are verified before anything is written, so a different build is never patched by accident.
#include "common.h"

namespace {

struct Site { uint32_t rva; uint8_t expect; uint8_t replace; const char* what; };

// RVA = address - 0x10000000
const Site kSites[] = {
    // FUN_1086ebb0 (assassinate)
    {0x0086EE25, 0x73, 0x7D, "assassinate: jae->jge"},  {0x0086EE4A, 0x47, 0x4F, "assassinate: cmova->cmovg"},
    // FUN_1086ee80
    {0x0086F02A, 0x73, 0x7D, "action 2: jae->jge"},     {0x0086F04E, 0x47, 0x4F, "action 2: cmova->cmovg"},
    // FUN_1086f080
    {0x0086F44A, 0x73, 0x7D, "action 3: jae->jge"},     {0x0086F46E, 0x47, 0x4F, "action 3: cmova->cmovg"},
    // FUN_1086f670
    {0x0086F7AA, 0x73, 0x7D, "action 4: jae->jge"},     {0x0086F7CF, 0x47, 0x4F, "action 4: cmova->cmovg"},
    // FUN_1086f800
    {0x0086F8E8, 0x73, 0x7D, "action 5: jae->jge"},     {0x0086F90C, 0x47, 0x4F, "action 5: cmova->cmovg"},
    // FUN_1086fef0
    {0x0087001B, 0x73, 0x7D, "action 6: jae->jge"},     {0x00870040, 0x47, 0x4F, "action 6: cmova->cmovg"},
    // FUN_10870070 (religious action)
    {0x00870346, 0x73, 0x7D, "action 7: jae->jge"},     {0x0087036B, 0x47, 0x4F, "action 7: cmova->cmovg"},
    // FUN_10870390
    {0x008705A3, 0x73, 0x7D, "action 8: jae->jge"},     {0x008705C6, 0x47, 0x4F, "action 8: cmova->cmovg"},
    // FUN_10870600 (sabotage)
    {0x008707BB, 0x73, 0x7D, "sabotage: jae->jge"},     {0x008707DE, 0x47, 0x4F, "sabotage: cmova->cmovg"},
};

} // namespace

void PatchAgentChance_Install() {
    if (!g_cfg.agent_chance_clamp) { Log("agent_chance_clamp: off"); return; }
    if (!g_engine.knownBuild && !g_cfg.allow_unknown_build) { Log("agent_chance_clamp: skipped, engine build not verified"); return; }
    // verify every site first; patch nothing if any byte differs
    for (const Site& s : kSites) {
        auto p = reinterpret_cast<const uint8_t*>(g_engine.base + s.rva);
        if (*p != s.expect) { Log("agent_chance_clamp: byte mismatch at rva 0x%08X (%s): found 0x%02X expected 0x%02X; patch NOT applied", s.rva, s.what, *p, s.expect); return; }
    }
    int n = 0;
    for (const Site& s : kSites) {
        auto p = reinterpret_cast<uint8_t*>(g_engine.base + s.rva);
        if (WriteCode(p, &s.replace, 1)) ++n; else Log("agent_chance_clamp: write failed at rva 0x%08X (%s)", s.rva, s.what);
    }
    Log("agent_chance_clamp: %d of %d bytes patched (9 agent-action chance functions now clamp signed)", n, (int)(sizeof(kSites) / sizeof(kSites[0])));
}
