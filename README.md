# TWS2-Fixing-Project — engine fix library for Total War: SHOGUN 2

A small DLL that loads beside the original (32-bit) Total War: SHOGUN 2 and fixes engine behaviour at run
time. It contains no Creative Assembly code and modifies no game file: everything happens in memory after
the game starts, and removing one file removes the library.

Target: the Steam build of 24 May 2023 (`empire.retail.dll` linked 2023-05-12, PE timestamp `0x645E92BB`).
Patches that work through Windows import tables apply to any build; patches that use engine addresses are
only applied on the verified build unless you override that in `tws2fix.ini`.

## Install

The library is still delivered as `dinput8.dll` — that file name is how the game loads it and cannot change. Its own files are `tws2fix.ini` and `tws2fix.log`. (Earlier test builds were called s2fix; if you have an `s2fix.ini` in the game folder, rename it.)

1. Copy `dinput8.dll` and `tws2fix.ini` into the game folder, next to `shogun2.exe`
   (for example `C:\Program Files (x86)\Steam\steamapps\common\Total War SHOGUN 2`).
2. Start the game normally. A file `tws2fix.log` appears in the game folder.
3. To uninstall, delete `dinput8.dll`.

If you already use another `dinput8.dll` in that folder (Rebrond's borderless mod, a ReShade or DXVK
component), only one can be named `dinput8.dll`; remove the other first.

Linux (Proton): add `WINEDLLOVERRIDES="dinput8=n,b" %command%` to the game's Steam launch options, or
Proton will use its own DirectInput and ignore the library. Use the Windows build of the game through Proton;
Feral's native Linux port is a different executable and cannot load it.

## How it loads

The engine imports one function from `dinput8.dll`, so Windows loads this file from the game folder when
the engine loads. The library forwards every DirectInput call to the real `dinput8.dll` in `System32`, and
installs its own hooks when the launcher (`shogun2.exe`) asks the engine for its entry point. That moment is
after the engine is fully loaded and before it runs any code, so the hooks are in place for the first
window the game creates.

## Patches in this build (0.9.0)

| Key in `tws2fix.ini` | Default | What it does |
| --- | --- | --- |
| `cursor_clip` | on | Keeps the mouse inside the game window while the game is in the foreground, so edge-scrolling no longer throws the cursor onto a second monitor. Released on alt-tab or when a dialog is up. |
| `borderless` | on | Turns the game's windowed mode into a borderless window covering the monitor. Set the game to Windowed and your desktop resolution in its graphics options first. Has no effect in exclusive fullscreen. Switched off automatically when s2fix is installed with its own borderless mode on (see "Using it with s2fix"). |
| `agent_chance_clamp` | on | Fixes the agent-action success chance. Nine of the engine's fourteen action-chance functions (assassinate, sabotage, apprehend and others) clamp the percentage to 5..95 with an *unsigned* comparison, so a negative total (weak agent against a strong target) wraps around and comes out as 95% instead of 5%. The patch changes two opcodes per function (`jae`→`jge`, `cmova`→`cmovg`), after verifying every byte, and only on the verified build. |
| `ui_scale` | auto | `auto` gives the UI the size it has at 1920×1080 on any screen (nothing happens at 1080p or below); `off` disables it; a number is a factor of your own (1.333 at 2560×1440). It revives the engine's dormant "UI rect" variables: the UI is laid out and projected on a smaller logical screen, while the viewports and the mouse mapping keep the real screen, so the UI is rasterised at native resolution, not upscaled. The upper limit is `min(width/1280, height/1024)` because the engine floors the layout at 1280×1024. Loading screens are not scaled. The battle drag-selection box, which the engine positions in screen pixels, is mapped onto the scaled layout so it starts under the cursor. Tested at 2560×1440 under Proton. |
| `settlement_box` | 1000 | Half-size, in map units, of the box inside which the campaign display draws settlement and building models. The engine builds that quadtree with fixed bounds (−350,−10,−350)…(350,100,350); models placed outside never enter it and are never drawn, which is why large map mods lose settlements past ±350 while flags and units still show. The patch rewrites the four bounds constants before the tree is built. The campaign tree display has the same limit (its spatial tree is built for ±350 on both axes), so trees past ±350 vanish too; since 0.7.0 the same setting widens that box as well. On by default at 1000, which covers any map; the vanilla map is unaffected because everything it has is inside ±350 either way. |
| `movement_cell_size` | auto | For map mods that rescale the campaign map (Expanded Japan's 4× pack has 4.0-unit movement-grid cells instead of 2.0). The yellow movement-range area is computed by `FUN_107e40f0` / `FUN_107e4510`, which hand the army's movement points straight to a flood that charges true map distance, while the real move and the red path come out at half that on a 4.0-unit grid. With `auto`, each map's grid cell size is read when it loads (the grid constructor `FUN_107621f0` is wrapped) and the movement points given to the area are scaled by 2.0 / cell size; the 42 cell-size constants of the area's edge refinement (`FUN_107753c0`, `FUN_10776700`, `FUN_10777600`) are set to the same cell size. CA's maps (2.0) are unchanged. A number forces one value for every map; `0` turns it off. `movement_debug_log=1` logs the scaling. |
| `clan_select_exact` | on | The campaign selection screen finds a clicked button's clan with `FUN_1101a930`: it takes the button's name after its first `_` (`button_nasu` → `nasu`) and returns the first clan, in alphabetical key order, whose key *contains* that text. A clan whose key sits inside a longer key that sorts before it can never be started from its own button: with Expanded Japan's Hokkaido pack, Nasu's button started a Menasunkur campaign. The button name arrives as the engine's UTF-8 string and the clan keys are UTF-16. The patch wraps the function: the game's search runs as before, and if a clan's key is exactly the button's text, that clan is returned instead. Without an exact match nothing changes. Checked against the real engine under Wine with a test clan list (`test/clan_select_check.cpp`): unpatched, `button_nasu` returns Menasunkur; patched, every button returns its own clan. Entry bytes checked, verified build only. |
| `minimap_area` | auto | Campaign minimap on enlarged maps: the minimap uses the map's theatre box when it differs from CA's. See "Campaign minimap on enlarged maps" below. |
| `s2fix_compat` | on | Works alongside DudeCmonMan's s2fix: gives way to its borderless mode and cursor clip, and leaves alone the window functions its borderless mode needs. See "Using it with s2fix" below. |
| `vram_mb` | 0 (off) | Overrides the video-memory total the renderer budgets its textures from. The engine has a debug variable `force_total_video_memory` that replaces the device query when non-zero; this writes the configured figure (MB, max 4095) into it before the device is created. A 32-bit process is usually told ~2.9 GB whatever the card. |
| `log` | on | Writes `tws2fix.log`. Keep it on while testing; it records every window the engine creates and every patch applied. |

## Battle fixes (from ShakuEight's Shogun2-fix)

[ShakuEight/Shogun2-fix](https://github.com/ShakuEight/Shogun2-fix) is an Apache-2.0 project of battle-engine fixes
for the same build, also delivered as a `dinput8.dll` proxy, so the two could never be installed together. Its ten
fixes are ported into this library (`src/shaku/`, attribution in each file and in `NOTICE`) and switched by the
`[battle]` section of `tws2fix.ini`, all on by default. Every fix byte-checks its own patch sites against the
analysed build before writing anything and installs nothing on a difference; several carry run-time self-checks
that switch the fix off for the session on failure. **They change the battle simulation: single player and
replays only** — a changed simulation desyncs multiplayer. `battle_fixes=0` turns the whole section off.

| Key | Fix |
| --- | --- |
| `fix_path` | Units ordered through gates, doorways and round corners stop in a stretched block and file through one man at a time. The formation is laid past the narrow stretch so the men run through as a column and re-form beyond it. (ShakuEight's public release 1.0.) |
| `fix_artillery` | Artillery ordered to fire at a unit aims at the end of the enemy line instead of its middle. |
| `fix_fire_by_rank` | Fire by Rank: about 10 s before the first volley; Hattori and Ikko Ikki gun units stop firing after three volleys; ranks beyond the third never fire; reload waits. |
| `fix_siege` | In large sieges, once attackers climb the walls, the defender's units answer move orders late or not at all. |
| `fix_wall_manning` | A unit ordered to man a wall section marches to a staging point inside the castle first, re-forms, and only then walks onto the wall. |
| `fix_zombie_yari` | A yari wall unit that breaks mid-charge sometimes stands in its wall, counted as routing, and keeps killing. |
| `fix_wall_collapse` | Siege defence: men deployed on a gatehouse die at Start Battle when the campaign left the gates open. |
| `fix_single_unit_group` | G, Ctrl+G and Ctrl+number refuse a single unit; the fix lets one unit form a soft or locked group. |
| `fix_guard_mode` | Missile units in guard mode chase a target that left their range and keep shooting at shattered units. |
| `fix_kneel_fire` | Fall of the Samurai: units with Kneel Fire on refuse every move order. |

### Campaign minimap on enlarged maps (new in 0.9.0, `minimap_area`)

`src/patch_minimap.cpp`. Two engine functions give the campaign minimap its world area: `FUN_11121990` (centre) and `FUN_11121a80` (size). Every minimap feature uses them: click to camera (`CampaignUI.TheatreMapDimensions`), the camera marker, and the unit, settlement and region markers. For Sengoku they return a fixed area (centre −22.5, −80; 540 × 270, so x −292.5…247.5, y −215…55), whatever the map says. Only Fall of the Samurai reads its theatre record. A map mod that enlarges the map therefore gets a squeezed minimap whose clicks land in the wrong place, and changing the theatre box in `regions.esf` does nothing.

The patch wraps both functions. With `minimap_area=auto`, when the theatre record's box differs from CA's (−305, −240 … 270, 70), the minimap uses that box. CA's maps and the normal Expanded Japan map keep the fixed area their pictures are drawn for. `on` always uses the theatre box, `x0,y0,x1,y1` sets a box by hand, and `off` is the game's behaviour. A missing or unusable record keeps the game's area.

The map mod must ship minimap pictures drawn for its theatre box, as Expanded Japan's Hokkaido add-on does. Checked against the real engine under Wine (`test/minimap_check.cpp` calls the two engine functions on made-up campaign data, vanilla and patched), and in game with the Hokkaido add-on: clicking the minimap moves the camera to the place clicked. `minimap_debug_log=1` lists the campaign's theatres and which one the minimap reads, which confirms in game that the record is the map's theatre box.

### Using it with s2fix (new in 0.8.9)

[s2fix](https://github.com/DudeCmonMan/s2fix) by DudeCmonMan is a separate performance mod for the same game (frame rate, loading times, FSR, borderless fullscreen, campaign map speed). It loads as `d3d11.dll` and works beside this DLL: install both. None of the engine code the two patch is shared.

Both mods deal with the game window, so `s2fix_compat=1` (on by default, `[patches]`) makes this DLL give way when it finds `s2prof.dll` and `d3d11.dll` in the game folder:
- If s2fix's borderless mode is on (`[window] borderless` in `s2prof.ini`, on by default), it is used and ours is switched off; if it is off, ours stays on. Until 0.8.8 this DLL hooked the window functions (SetWindowLongW, SetWindowPos, MoveWindow) that s2fix's borderless mode checks, so its borderless mode switched itself off. They are now left to s2fix, and the UI scaling follows the game's resolution through a timer on the game window instead.
- If s2fix clips the cursor (`clip_cursor=1` in `s2prof.ini`), our cursor clip stands down while s2fix is loaded.
- s2fix loads only in DirectX 11 mode. In DirectX 9 it does nothing, our cursor clip carries on, and our borderless stays off unless you set `s2fix_compat=0`.

The log says what was decided (`s2fix_compat: ...` lines).

### European voices for a mod's units (new in 0.8.8, `[voices]` section)

`src/patch_unitvoices.cpp`. The game gives the Portuguese Tercos Fall of the Samurai's British voice set by naming their unit key in its code. `fots_voice_units` is a list of unit keys that get the same treatment, so a mod's European units can speak with that voice set (or with files that replace it, like a Portuguese voice mod) while keeping their own keys. The default list is the infantry and ships of Expanded Japan's Portugal add-on (DLL edition); keys that are not in the game do nothing, and an empty value switches it off.

| Where | What the engine does | What the patch adds |
| --- | --- | --- |
| Unit record, `FUN_10c0c9c0` at `0x10C0CCE6` | Voice nationality at record+0xD4 (0 British … 3 Japanese); the Tercos are forced to 0. | A listed key is set to 0. |
| Battle land unit, `FUN_10129db0` at `0x1012A618` | Byte unit+0xFC8 = key starts with `Boshin` or is the Tercos. | 1 for a listed key. |
| Battle ship, `FUN_1035ae20` at `0x1035C405` | Byte ship+0x16D0 = key starts with `Boshin`. | 1 for a listed key. |
| Battle audio set-up, `FUN_10291ae0` at `0x10291C12` / `0x10291B99` | A flagged unit of the player's alliance switches battle sound events to the `bosh_` set; the Tercos are skipped by key. | Listed keys are skipped too, so a Sengoku battle keeps its own sounds. |

The mod must ship the voice files and set the units' voice columns as the Royal Marines have them (`VO_actor_numbers` 24,25; `num_audio_VO_variations` 2; `audio_language` UK). Infantry and ships only: the voice set has no cavalry, general or artillery lines. Checked against the real engine under Wine (`test/unit_voices_check.cpp` runs the engine's code at the five sites on made-up units, vanilla and patched); what is actually heard needs an in-game test. `fots_voice_debug_log=1` logs each listed unit as its record loads and as it enters a battle.

### Battle rule fixes (new in 0.8.5, `[battle]` section)

Written for this project (`src/patch_battlerules.cpp`), single player only like the fixes above.

| Key | Default | What it does |
| --- | --- | --- |
| `ai_difficulty_accuracy` | 1 (vanilla) | On Hard, Very Hard and Legendary the AI's missile units shoot more accurately: the engine multiplies an AI unit's core marksmanship by 1.3 (Hard) or 1.5 (Very Hard and Legendary), ships by 1.15 / 1.3 (`FUN_1043d500`), and adds +20 / +30 projectile marksmanship (`FUN_104646c0`). Both feed the chance to hit and the kill chance. `0` removes that bonus and keeps the AI's other difficulty effects (melee +2 / +4, campaign handicaps). |
| `projectile_sound_priority` | 1 | The firing sound comes from `sound_bank_projectile_fire.xml`, scored by gun type (weight 4), shot type (1), distance, game mode and unit (10) in `FUN_10cc71b0`. A listed but different gun or shot type only loses points, so an entry made for a unit's cannon outscores the right matchlock, bow or torch entry when that unit fires its other weapon. The fix ranks entries that accept the projectile's gun and shot type above those that exclude it; otherwise the game's own scoring decides. |
| `fear_stacking` | 1 | Fear from traits, ancillaries, skills and effect bundles reaches a unit as the unit-record bonus `scares_men`. The battle unit builder (`FUN_10151f10`) only tests the total for zero: any non-zero total gives the flat morale-table fear of 4, and +1 from one source and −1 from another cancel to no fear. The fix makes the unit's fear the total itself (three +1 traits = 3; 0 or less = none). Fear that comes from a unit's own attributes is not touched. |
| `battle_rules_debug_log` | 0 | Logs the difficulty values the missile code sees, every changed firing-sound choice, and which battle-AI state each army group enters (for the naval defender AI investigation). |

The port keeps each fix's logic unchanged and removes ShakuEight's development scaffolding (toggle keys, tones,
research probes, test exports). Two things changed for the compiler: the detour stubs are emitted as GCC assembly
(byte-identical to the MSVC ones, so the generated stack-offset headers still apply) and the guarded memory reads
use `ReadProcessMemory` instead of SEH. Apart from `fix_path`, ShakuEight's own builds of these fixes are
development builds that have not had a public release; treat them as such until they have been played.

## Alt group-move (Total War: WARHAMMER-style block move)

A new control, not a bug fix. With several units selected:

- hold **Alt**, press and hold the **left** button on the ground, and move the mouse: green ghosts of the
  selection follow the cursor as a block, every unit keeping its place in the formation, its frontage and
  its facing;
- while still holding the button, hold **Ctrl** and move the mouse sideways: the block stays where it is
  and turns about its own middle; release Ctrl to carry on moving it;
- release the button to issue the order.

Keys in `[battle]`: `alt_group_move` (on), `alt_rotate_speed` (64; higher turns faster), `alt_debug_log`
(off; per-hold traces for testing). Single player only, like the battle fixes.

How it works. In Shogun 2 the left button only selects (a left drag is the selection box) and move orders
are on the right button. `src/patch_altinput.cpp` shows an Alt + left hold to the engine as a right-button
hold (DirectInput mouse, window messages, the engine's key-state imports) and hides Alt from it meanwhile,
so the engine runs its own formation drag with ghosts. `src/patch_altmove.cpp` latches the selection's
layout when the hold begins (position, heading and width of each unit, read through the engine's own unit
handle) and, while the engine builds the order, replaces each unit's slot point with the block position,
gives each unit its own facing in the placement, and keeps its width. The cursor's ground position is
taken from the engine's drag line (its cursor end), the block centre is frozen while Ctrl is held, and the
same replacement runs when the queued command is executed, so what the ghosts show is what is ordered.

## Things that are not engine patches

Two campaign-map limits that come up with large map mods turned out to be data, not engine code. They are
documented in `docs/minimap_and_map_limits.md` together with a small tool (`tools/theatre_bounds.py`) that
fixes one of them:

- **Minimap clicks land in the wrong place on an enlarged map.** The click → world mapping is a linear map
  from the radar image onto the *theatre bounds* stored in `campaign_maps/<map>/regions.esf`
  (vanilla: −305,−240 … 270,70). Change those bounds to the world rectangle the new radar image covers.
- **Battle minimap too dark since the May 2023 update.** The generator's colour-overlay pass darkens the
  map by 73%; `packs/@tws2_battle_minimap_fix.pack` (drop into the game's `data` folder) sets that overlay
  to 0 and the minimap is readable again. Confirmed in game.
- **255 regions on the minimap.** The region overlay is an 8-bit paletted TGA and regions are identified by
  palette index, so 255 is structural. Lifting it means replacing the region-map component's texture
  path; see the document for what that involves.

## Building

Windows: install a 32-bit mingw-w64 (for example MSYS2's `mingw-w64-i686-gcc` from a MINGW32 shell) and run `build.bat`. Visual Studio cannot build it: the battle fixes use GCC assembly.

Linux cross-build: `apt install gcc-mingw-w64-i686 g++-mingw-w64-i686` then `./build.sh`. Output is `dist/dinput8.dll`.

## Layout

- `src/dllmain.cpp` — DirectInput proxy exports, engine detection, hook installation
- `src/hooks.cpp` — import-table hook and inline detour helpers
- `src/patch_window.cpp` — borderless window, cursor clipping and s2fix compatibility
- `src/patch_agent_chance.cpp` — the agent-action chance clamp fix (engine byte patch, with addresses)
- `src/patch_uiscale.cpp` — UI scaling through the engine's UI rect variables and a caller-aware detour of the rect getter
- `src/patch_clanselect.cpp` — clan selection by exact key (detour of the faction lookup, with addresses)
- `src/patch_campaign.cpp` — settlement and tree render boxes and video-memory override (engine bytes, with addresses)
- `src/patch_battle.cpp` — registry of the battle fixes, gated by the `[battle]` keys
- `src/patch_battlerules.cpp` — AI difficulty accuracy, firing-sound selection and fear stacking
- `src/patch_unitvoices.cpp` — European voice set for listed unit keys
- `src/patch_minimap.cpp` — campaign minimap area from the map's theatre box
- `src/patch_movecell.cpp` — movement range on rescaled campaign maps
- `src/patch_altinput.cpp`, `src/patch_altmove.cpp` — Alt group-move (input swap and block placement)
- `src/patch_uidiag.cpp` — diagnostics for the battle drag-selection box
- `src/shaku/` — ShakuEight's hook engine (`hook.*`), guarded memory (`gamemem.*`) and the ten fixes (`fix_*.cpp`), Apache-2.0
- `third_party/`, `NOTICE` — licences and attribution
- `docs/` — findings on the campaign-map limits that are data rather than engine code
- `tools/` — `theatre_bounds.py` (minimap click alignment for enlarged maps) and the ESF/pack readers it needs
- `src/config.cpp`, `src/log.cpp` — `tws2fix.ini` and `tws2fix.log`
- `test/` — a stub launcher and stub engine used to smoke-test the loader under Wine; `real_engine_check.cpp`
  (every engine patch site checked against the real `empire.retail.dll`); and checks that run the real engine's code
  for individual fixes: `clan_select_check.cpp`, `unit_voices_check.cpp`, `s2fix_compat_check.cpp`, `minimap_check.cpp`

## Related projects

[JakeTheRipperYT/Shogun-2-2026-ultimate-engine-bug-fix-patch](https://github.com/JakeTheRipperYT/Shogun-2-2026-ultimate-engine-bug-fix-patch)
is a binary-only installer (no source, no licence) that rebuilds modified game packs and patches the engine files on
disk; nothing of it can be reused here. Of its engine-side items, the ones not covered by ShakuEight's fixes
(night-battle reinforcements, locked-formation overlap, projectile sound selection, tooltip wrapping) are candidates
for this project's own reverse-engineering; its balance changes (morale numbers, rosters, AI difficulty bonuses)
are out of scope.

[DudeCmonMan/s2fix](https://github.com/DudeCmonMan/s2fix) is a separate, GPL-3 project of **performance** fixes
for the same engine build (multithreaded draw submission, flip-model swap chain, VRAM reporting). It loads as a
`d3d11.dll` proxy, so the two can be installed together. Both implement borderless window and cursor clipping;
this DLL detects s2fix and gives way to it (see "Using it with s2fix").

## Credits

- **Claude** (Anthropic's AI model) did nearly all of the work: reverse-engineering the engine, writing the
  code, the tools and the documentation, and porting ShakuEight's battle fixes.
- **Psychocrysma** started and directed the project, chose what to fix, designed the Alt group-move, and
  tested every build in game (logs, screenshots and videos that drove each fix).
- **ShakuEight**: the ten battle fixes in `src/shaku/` come from
  [ShakuEight/Shogun2-fix](https://github.com/ShakuEight/Shogun2-fix) (Apache 2.0, see `NOTICE`).

## Licence

MIT. See `LICENSE`.
