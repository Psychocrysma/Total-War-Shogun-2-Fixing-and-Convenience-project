// Battle fixes ported from ShakuEight/Shogun2-fix (Apache License 2.0; see src/shaku/ and
// third_party/LICENSE-APACHE-2.0). Each fix byte-checks its own patch sites against the analysed
// build before writing anything and refuses on any difference, so they are gated here only by the
// [battle] keys of tws2fix.ini and by this project's build check. They change the battle
// simulation: single player and replays only, since a changed simulation desyncs multiplayer.
#include "common.h"

// One pair per fix, defined in src/shaku/fix_*.cpp.
void KneelFix_Install(); void KneelFix_Remove();
void Pathfix_Install(); void Pathfix_Remove();
void ArtFix_Install(); void ArtFix_Remove();
void FbrFix_Install(); void FbrFix_Remove();
void SiegeFix_Install(); void SiegeFix_Remove();
void WallFix_Install(); void WallFix_Remove();
void ZombieFix_Install(); void ZombieFix_Remove();
void CollapseFix_Install(); void CollapseFix_Remove();
void GroupFix_Install(); void GroupFix_Remove();
void GuardFix_Install(); void GuardFix_Remove();

namespace {
struct Entry { const char* key; bool Config::*on; void (*install)(); void (*remove)(); bool installed; };
Entry g_fixes[] = {
    { "fix_path",              &Config::fix_path,              Pathfix_Install,     Pathfix_Remove,     false },
    { "fix_artillery",         &Config::fix_artillery,         ArtFix_Install,      ArtFix_Remove,      false },
    { "fix_fire_by_rank",      &Config::fix_fire_by_rank,      FbrFix_Install,      FbrFix_Remove,      false },
    { "fix_siege",             &Config::fix_siege,             SiegeFix_Install,    SiegeFix_Remove,    false },
    { "fix_wall_manning",      &Config::fix_wall_manning,      WallFix_Install,     WallFix_Remove,     false },
    { "fix_zombie_yari",       &Config::fix_zombie_yari,       ZombieFix_Install,   ZombieFix_Remove,   false },
    { "fix_wall_collapse",     &Config::fix_wall_collapse,     CollapseFix_Install, CollapseFix_Remove, false },
    { "fix_single_unit_group", &Config::fix_single_unit_group, GroupFix_Install,    GroupFix_Remove,    false },
    { "fix_guard_mode",        &Config::fix_guard_mode,        GuardFix_Install,    GuardFix_Remove,    false },
    { "fix_kneel_fire",        &Config::fix_kneel_fire,        KneelFix_Install,    KneelFix_Remove,    false },
};
}

void PatchBattle_Install() {
    if (!g_cfg.battle_fixes) { Log("battle fixes: off (battle_fixes=0)"); return; }
    if (!g_engine.knownBuild && !g_cfg.allow_unknown_build) { Log("battle fixes: skipped, engine build not verified"); return; }
    for (Entry& e : g_fixes) {
        if (!(g_cfg.*e.on)) { Log("%s: off", e.key); continue; }
        e.install();
        e.installed = true;
    }
}

void PatchBattle_Remove() {
    for (int i = static_cast<int>(sizeof(g_fixes) / sizeof(g_fixes[0])) - 1; i >= 0; i--)
        if (g_fixes[i].installed) g_fixes[i].remove();
}
