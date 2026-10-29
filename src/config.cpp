#include "common.h"

Config g_cfg;

static bool ReadBool(const wchar_t* section, const wchar_t* key, bool def, const std::wstring& ini) {
    return GetPrivateProfileIntW(section, key, def ? 1 : 0, ini.c_str()) != 0;
}

void ConfigLoad(const std::wstring& ini) {
    // Missing file -> defaults. GetPrivateProfile* handles that quietly.
    g_cfg.log                 = ReadBool(L"general", L"log", g_cfg.log, ini);
    g_cfg.allow_unknown_build = ReadBool(L"general", L"allow_unknown_build", g_cfg.allow_unknown_build, ini);
    g_cfg.cursor_clip         = ReadBool(L"patches", L"cursor_clip", g_cfg.cursor_clip, ini);
    g_cfg.borderless          = ReadBool(L"patches", L"borderless", g_cfg.borderless, ini);
    g_cfg.agent_chance_clamp  = ReadBool(L"patches", L"agent_chance_clamp", g_cfg.agent_chance_clamp, ini);
    wchar_t buf[32] = L"0";
    GetPrivateProfileStringW(L"patches", L"ui_scale", L"auto", buf, 32, ini.c_str());
    for (wchar_t* c = buf; *c; ++c) *c = static_cast<wchar_t>(towlower(*c));
    if (wcscmp(buf, L"auto") == 0 || wcscmp(buf, L"on") == 0 || wcscmp(buf, L"1080p") == 0) { g_cfg.ui_scale_auto = true; g_cfg.ui_scale = 0.0f; }
    else { g_cfg.ui_scale_auto = false; g_cfg.ui_scale = (wcscmp(buf, L"off") == 0) ? 0.0f : static_cast<float>(wcstod(buf, nullptr)); }
    g_cfg.ui_scale_rt         = ReadBool(L"patches", L"ui_scale_rt", g_cfg.ui_scale_rt, ini);
    g_cfg.ui_scale_log_callers = ReadBool(L"patches", L"ui_scale_log_callers", false, ini);
    g_cfg.ui_scale_log_box     = ReadBool(L"patches", L"ui_scale_log_box", false, ini);
    {
        wchar_t list[512] = L"";
        GetPrivateProfileStringW(L"patches", L"ui_scale_small_callers", L"", list, 512, ini.c_str());
        wchar_t* p = list;
        while (*p && g_cfg.ui_scale_extra_count < 16) {
            while (*p == L' ' || *p == L',' || *p == L';') ++p;
            if (!*p) break;
            wchar_t* end = nullptr;
            unsigned long v = wcstoul(p, &end, 16);
            if (end == p) break;
            g_cfg.ui_scale_extra_callers[g_cfg.ui_scale_extra_count++] = static_cast<uint32_t>(v);
            p = end;
        }
    }
    g_cfg.ui_scale_mode       = GetPrivateProfileIntW(L"patches", L"ui_scale_mode", g_cfg.ui_scale_mode, ini.c_str());
    GetPrivateProfileStringW(L"patches", L"settlement_box", L"1000", buf, 32, ini.c_str());
    g_cfg.settlement_box      = static_cast<float>(wcstod(buf, nullptr));
    GetPrivateProfileStringW(L"patches", L"movement_cell_size", L"auto", buf, 32, ini.c_str());
    g_cfg.movement_cell_auto  = (wcscmp(buf, L"auto") == 0 || wcscmp(buf, L"on") == 0);
    g_cfg.movement_debug_log  = ReadBool(L"patches", L"movement_debug_log", false, ini);
    g_cfg.movement_cell_size  = g_cfg.movement_cell_auto ? 0.0f : static_cast<float>(wcstod(buf, nullptr));
    g_cfg.vram_mb             = GetPrivateProfileIntW(L"patches", L"vram_mb", 0, ini.c_str());
    g_cfg.battle_fixes        = ReadBool(L"battle", L"battle_fixes", g_cfg.battle_fixes, ini);
    g_cfg.fix_path            = ReadBool(L"battle", L"fix_path", g_cfg.fix_path, ini);
    g_cfg.fix_artillery       = ReadBool(L"battle", L"fix_artillery", g_cfg.fix_artillery, ini);
    g_cfg.fix_fire_by_rank    = ReadBool(L"battle", L"fix_fire_by_rank", g_cfg.fix_fire_by_rank, ini);
    g_cfg.fix_siege           = ReadBool(L"battle", L"fix_siege", g_cfg.fix_siege, ini);
    g_cfg.fix_wall_manning    = ReadBool(L"battle", L"fix_wall_manning", g_cfg.fix_wall_manning, ini);
    g_cfg.fix_zombie_yari     = ReadBool(L"battle", L"fix_zombie_yari", g_cfg.fix_zombie_yari, ini);
    g_cfg.fix_wall_collapse   = ReadBool(L"battle", L"fix_wall_collapse", g_cfg.fix_wall_collapse, ini);
    g_cfg.fix_single_unit_group = ReadBool(L"battle", L"fix_single_unit_group", g_cfg.fix_single_unit_group, ini);
    g_cfg.fix_guard_mode      = ReadBool(L"battle", L"fix_guard_mode", g_cfg.fix_guard_mode, ini);
    g_cfg.fix_kneel_fire      = ReadBool(L"battle", L"fix_kneel_fire", g_cfg.fix_kneel_fire, ini);
    g_cfg.alt_group_move      = ReadBool(L"battle", L"alt_group_move", g_cfg.alt_group_move, ini);
    g_cfg.alt_rotate_speed    = GetPrivateProfileIntW(L"battle", L"alt_rotate_speed", g_cfg.alt_rotate_speed, ini.c_str());
    g_cfg.alt_debug_log = ReadBool(L"battle", L"alt_debug_log", false, ini);
}
